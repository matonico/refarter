#pragma once

#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace refarter {

struct HttpsAnswer {
    DWORD status = 0;      // 0: no answer (offline, timeout, TLS failure).
    bool complete = false; // An answer arrived and, for 200, `sink` took the whole body.
};

// One HTTPS GET over WinHTTP, blocking, for a worker thread. Redirects are followed (never from HTTPS to HTTP), no
// cookies or credentials are sent, and every step times out after `timeoutMs`. For a 200 the body goes to `sink` in
// chunks with the announced length (0 when the server gave none); a sink that returns false stops the read, and the
// answer is then incomplete. Other statuses read no body.
HttpsAnswer httpsGet(const std::wstring& userAgent, std::wstring_view host, const std::wstring& path, const wchar_t* headers,
                     DWORD timeoutMs, const std::function<bool(const char* data, DWORD size, std::uint64_t length)>& sink);

} // namespace refarter
