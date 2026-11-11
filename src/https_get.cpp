#include "https_get.h"

#include <winhttp.h>
#include <vector>

namespace refarter {

namespace {
struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle = nullptr) : h(handle) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
} // namespace

HttpsAnswer httpsGet(const std::wstring& userAgent, std::wstring_view host, const std::wstring& path, const wchar_t* headers,
                     DWORD timeoutMs, const std::function<bool(const char*, DWORD, std::uint64_t)>& sink) {
    HttpsAnswer answer;
    Handle session(WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) return answer;
    const int timeout = static_cast<int>(timeoutMs);
    if (!WinHttpSetTimeouts(session.h, timeout, timeout, timeout, timeout)) return answer;
    Handle connection(WinHttpConnect(session.h, std::wstring(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.h) return answer;
    Handle request(WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.h) return answer;
    DWORD disable = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
    if (!WinHttpSendRequest(request.h, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, headers ? static_cast<DWORD>(-1L) : 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return answer;
    if (!WinHttpReceiveResponse(request.h, nullptr)) return answer;
    DWORD size = sizeof(answer.status);
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &answer.status, &size, WINHTTP_NO_HEADER_INDEX)) {
        answer.status = 0;
        return answer;
    }
    if (answer.status != 200) { answer.complete = true; return answer; } // 404 and the rest need no body.
    std::uint64_t length = 0;
    wchar_t text[32]{};
    DWORD textSize = sizeof(text);
    if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, text, &textSize, WINHTTP_NO_HEADER_INDEX))
        length = _wcstoui64(text, nullptr, 10);
    std::vector<char> chunk(64 * 1024);
    while (true) {
        DWORD read = 0;
        if (!WinHttpReadData(request.h, chunk.data(), static_cast<DWORD>(chunk.size()), &read)) return answer;
        if (read == 0) break;
        if (!sink(chunk.data(), read, length)) return answer;
    }
    answer.complete = true;
    return answer;
}

} // namespace refarter
