#pragma once

#include <windows.h>

#include <cwctype>
#include <string>
#include <string_view>

// Text helpers shared by the UI, settings and file code.
namespace refarter {

// Lower-case copy of `text` (per-character towlower, so it never changes the length).
inline std::wstring lower(std::wstring_view text) {
    std::wstring result(text);
    for (auto& c : result) c = static_cast<wchar_t>(std::towlower(c));
    return result;
}

// UTF-8 bytes of `text`, for APIs that take narrow strings.
inline std::string utf8(std::wstring_view text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

} // namespace refarter
