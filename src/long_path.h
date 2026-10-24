#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>

namespace refarter {

// Win32 file calls refuse paths of MAX_PATH (260) characters or more unless the \\?\ prefix is given (the app has no
// longPathAware manifest). `text` is an absolute path; shorter ones, and ones already prefixed, are returned as they are.
// The prefix switches off Win32's own clean-up, so slashes are made backslashes first.
inline std::wstring extendedPathText(std::wstring text) {
    constexpr std::size_t limit = 260;
    if (text.size() < limit || text.rfind(L"\\\\?\\", 0) == 0 || text.rfind(L"\\\\.\\", 0) == 0) return text;
    std::replace(text.begin(), text.end(), L'/', L'\\');
    if (text.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + text.substr(2);
    if (text.size() >= 3 && text[1] == L':' && text[2] == L'\\') return L"\\\\?\\" + text;
    return text; // Relative or rooted without a drive: no prefix can describe it.
}

// The text to hand to CreateFileW and the like.
inline std::wstring extendedPath(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    return extendedPathText(error ? path.wstring() : absolute.lexically_normal().wstring());
}

} // namespace refarter
