#include "update_client.h"
#include "https_get.h"
#include "update_check.h"

#include <string>

namespace refarter {

namespace {
constexpr DWORD timeoutMs = 10'000; // Each of resolve, connect, send and receive.

struct Response {
    DWORD status = 0;
    std::string body;
    bool complete = false;
};

// One HTTPS GET of the release API. Anything unexpected is an incomplete response; nothing is written to disk and
// no cookies, credentials or other headers than the two below are sent.
Response fetch(const std::wstring& userAgent) {
    Response response;
    const auto answer = httpsGet(userAgent, update::apiHost, update::apiPath(),
        L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28", timeoutMs,
        [&](const char* data, DWORD size, std::uint64_t) {
            if (response.body.size() + size > update::maxResponseBytes) return false; // Far larger than any release answer.
            response.body.append(data, size);
            return true;
        });
    response.status = answer.status;
    response.complete = answer.complete;
    return response;
}
} // namespace

void UpdateClient::start(HWND window, UINT message, std::chrono::seconds delay, bool manual, std::wstring running) {
    task_.start(window, message, [delay, manual, running = std::move(running)](const auto& worker) {
        UpdateResult result;
        if (worker.waitFor(delay)) return result; // Cancelled while waiting: dropped, nothing asked or posted.
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
        return result;
    });
}

} // namespace refarter
