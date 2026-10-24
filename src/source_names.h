#pragma once

#include "text.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace refarter {

inline std::wstring friendlySourceName(std::wstring_view title, std::wstring_view executable) {
    const auto trim = [](std::wstring_view value) {
        while (!value.empty() && std::iswspace(value.front())) value.remove_prefix(1);
        while (!value.empty() && std::iswspace(value.back())) value.remove_suffix(1);
        return value;
    };
    title = trim(title);
    std::wstring name(title.empty() ? trim(executable) : title);
    name = lower(name);
    return name;
}

struct SourceName { std::wstring title, executable; };

// Only duplicate friendly names expose executable identity. Same-title windows
// from the same executable also get an ordinal, so every menu choice is distinct.
inline std::vector<std::wstring> sourceLabels(const std::vector<SourceName>& sources) {
    std::vector<std::wstring> names, executables, labels;
    for (const auto& source : sources) {
        names.push_back(friendlySourceName(source.title, source.executable));
        executables.push_back(friendlySourceName({}, source.executable));
    }
    labels = names;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (std::count(names.begin(), names.end(), names[i]) < 2) continue;
        if (!executables[i].empty() && executables[i] != names[i]) labels[i] += L" · " + executables[i];
        int sameExecutable = 0, ordinal = 0;
        for (std::size_t j = 0; j < names.size(); ++j) {
            if (names[j] == names[i] && executables[j] == executables[i]) {
                ++sameExecutable;
                if (j <= i) ++ordinal;
            }
        }
        if (sameExecutable > 1) labels[i] += L" · window " + std::to_wstring(ordinal);
    }
    return labels;
}

} // namespace refarter
