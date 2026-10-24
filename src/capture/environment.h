#pragma once

#include <windows.h>
#include <string_view>

namespace refarter::capture {

// Whether the process environment sets `name` to exactly `value` (non-empty). Reads the
// developer switches (REFARTER_* variables for tests and measurements); none of them is a saved preference.
inline bool environmentIs(const wchar_t* name, std::wstring_view value) {
    constexpr DWORD capacity = 16; // Longer values never match a switch.
    wchar_t text[capacity]{};
    const DWORD length = GetEnvironmentVariableW(name, text, capacity);
    return length != 0 && length == value.size() && std::wstring_view(text, length) == value;
}

} // namespace refarter::capture
