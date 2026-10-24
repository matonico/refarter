#include "update_client.h"
#include "update_check.h"

#include <winhttp.h>
#include <system_error>
#include <condition_variable>
#include <string>
#include <thread>
#include <vector>

namespace refarter {

namespace {
constexpr DWORD timeoutMs = 10'000; // Each of resolve, connect, send and receive.

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle = nullptr) : h(handle) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct Response {
    DWORD status = 0;
    std::string body;
    bool complete = false;
};

// One HTTPS GET of the release API. Anything unexpected is an incomplete response; nothing is written to disk and
// no cookies, credentials or other headers than the two below are sent.
Response fetch(const std::wstring& userAgent) {
    Response response;
    Handle session(WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) return response;
    if (!WinHttpSetTimeouts(session.h, timeoutMs, timeoutMs, timeoutMs, timeoutMs)) return response;
    const std::wstring host(update::apiHost);
    Handle connection(WinHttpConnect(session.h, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.h) return response;
    const auto path = update::apiPath();
    Handle request(WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.h) return response;
    DWORD disable = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
    const wchar_t headers[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28";
    if (!WinHttpSendRequest(request.h, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return response;
    if (!WinHttpReceiveResponse(request.h, nullptr)) return response;
    DWORD size = sizeof(response.status);
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &size, WINHTTP_NO_HEADER_INDEX)) {
        response.status = 0;
        return response;
    }
    if (response.status != 200) { response.complete = true; return response; } // 404 and the rest need no body.
    std::vector<char> chunk(8192);
    while (true) {
        DWORD read = 0;
        if (!WinHttpReadData(request.h, chunk.data(), static_cast<DWORD>(chunk.size()), &read)) return response;
        if (read == 0) break;
        if (response.body.size() + read > update::maxResponseBytes) return response; // Far larger than any release answer.
        response.body.append(chunk.data(), read);
    }
    response.complete = true;
    return response;
}
} // namespace

struct UpdateClient::State {
    std::mutex mutex;
    std::condition_variable wake;
    bool cancelled = false;
    bool finished = false;         // result is set.
    std::optional<UpdateResult> result;
    HWND window = nullptr;
    UINT message = 0;
};

void UpdateClient::start(HWND window, UINT message, std::chrono::seconds delay, bool manual, std::wstring running) {
    cancel();
    auto state = std::make_shared<State>();
    state->window = window;
    state->message = message;
    try {
        std::thread([state, delay, manual, running = std::move(running)] {
            {
                std::unique_lock lock(state->mutex);
                if (state->wake.wait_for(lock, delay, [&] { return state->cancelled; })) return;
            }
            UpdateResult result;
            result.manual = manual;
            const auto response = fetch(L"Refarter/" + running);
            if (response.complete && response.status == 404) result.outcome = UpdateResult::Outcome::NoRelease;
            else if (response.complete && response.status == 200) {
                if (const auto release = update::parseRelease(response.body)) {
                    if (update::isNewer(release->tag, running)) {
                        result.outcome = UpdateResult::Outcome::Newer;
                        result.tag = release->tag;
                        result.url = release->url;
                    } else result.outcome = UpdateResult::Outcome::Latest;
                }
            } // Everything else (offline, timeout, 403/429 rate limit, 5xx, bad JSON) stays Failed.
            std::lock_guard lock(state->mutex);
            if (state->cancelled) return;
            state->result = std::move(result);
            state->finished = true;
            PostMessageW(state->window, state->message, 0, 0);
        }).detach();
        state_ = std::move(state);
    } catch (const std::system_error&) {
        // No thread: no check this time. The caller sees running() false and the next start tries again.
    }
}

void UpdateClient::cancel() {
    if (!state_) return;
    {
        std::lock_guard lock(state_->mutex);
        state_->cancelled = true;
    }
    state_->wake.notify_all();
    state_.reset();
}

bool UpdateClient::running() const { return state_ != nullptr; }

std::optional<UpdateResult> UpdateClient::take() {
    if (!state_) return std::nullopt;
    std::optional<UpdateResult> result;
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->finished) return std::nullopt;
        result = std::move(state_->result);
    }
    state_.reset();
    return result;
}

} // namespace refarter
