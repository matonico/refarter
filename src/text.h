#pragma once

#include <windows.h>

#include <cwchar>
#include <cwctype>
#include <string>
#include <string_view>

// Text helpers shared by the UI, settings and file code.
namespace refarter {

// Executable paths as auto-capture and per-game settings identify games: compared ignoring case, so both always agree.
inline bool samePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

// One field of a line-based record file: no line break or NUL that would split or end the record.
inline bool singleLine(const std::wstring& text) { return text.find_first_of(L"\r\n\0", 0, 3) == std::wstring::npos; }

// Lower-case copy of `text` (per-character towlower, so it never changes the length).
inline std::wstring lower(std::wstring_view text) {
    std::wstring result(text);
    for (auto& c : result) c = static_cast<wchar_t>(std::towlower(c));
    return result;
}

// The first `limit` characters of `text`, never ending inside a surrogate pair or on a space.
inline std::wstring cutText(std::wstring_view text, std::size_t limit) {
    std::wstring cut(text.substr(0, limit));
    if (!cut.empty() && IS_HIGH_SURROGATE(cut.back())) cut.pop_back();
    while (!cut.empty() && std::iswspace(cut.back())) cut.pop_back();
    return cut;
}

// A game or window name as menus, the offer card, the tray and status text show it: at most `limit` characters, cut
// with "…" (never inside a surrogate pair), so one long window title cannot stretch a menu or hide what follows it.
// Only what is shown is cut; stored names and clip names keep the whole name.
inline std::wstring shownName(std::wstring_view name, std::size_t limit = 48) {
    if (limit < 2 || name.size() <= limit) return std::wstring(name);
    return cutText(name, limit - 1) + L"…";
}

// UTF-8 bytes of `text`, for APIs that take narrow strings.
inline std::string utf8(std::wstring_view text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

} // namespace refarter
