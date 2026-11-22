#pragma once

#include "text.h"

#include <algorithm>
#include <string>
#include <vector>

// Lists of games kept by executable path (auto-capture, per-game settings): one way to find, replace or remove an
// entry, so both lists always agree on which game a path names.
namespace refarter {

template <typename T>
T* findByPath(std::vector<T>& items, const std::wstring& path) {
    const auto found = std::find_if(items.begin(), items.end(), [&](const T& item) { return samePath(item.path, path); });
    return found == items.end() ? nullptr : &*found;
}

template <typename T>
const T* findByPath(const std::vector<T>& items, const std::wstring& path) {
    return findByPath(const_cast<std::vector<T>&>(items), path);
}

template <typename T>
void eraseByPath(std::vector<T>& items, const std::wstring& path) {
    std::erase_if(items, [&](const T& item) { return samePath(item.path, path); });
}

// A label on one line of a record file and of a menu: line breaks and tabs become spaces (a menu reads a tab as the
// start of grey detail text).
inline std::wstring oneLineLabel(std::wstring label) {
    for (auto& c : label) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    return label;
}

} // namespace refarter
