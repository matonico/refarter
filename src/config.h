#pragma once

#include "text.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace refarter {

// Settings kept between launches, in %LOCALAPPDATA%\Refarter\refarter.ini.
// The first launch creates it from the embedded config/refarter.ini template.
std::filesystem::path configPath();

// "[section]" and "key = value" lines; ';' and '#' start comments. Names are
// case-insensitive and stored lower-case as "section/key"; surrounding quotes are dropped.
inline std::map<std::wstring, std::wstring> parseConfig(std::wstring_view text) {
    const auto trim = [](std::wstring_view part) {
        const auto first = part.find_first_not_of(L" \t\r");
        if (first == std::wstring_view::npos) return std::wstring_view{};
        return part.substr(first, part.find_last_not_of(L" \t\r") - first + 1);
    };
    std::map<std::wstring, std::wstring> values;
    std::wstring section;
    for (size_t start = 0; start < text.size();) {
        const auto end = std::min(text.find(L'\n', start), text.size());
        const auto line = trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const auto equals = line.find(L'=');
        if (equals == std::wstring_view::npos || section.empty()) continue;
        auto value = trim(line.substr(equals + 1));
        if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') value = value.substr(1, value.size() - 2);
        values[section + L'/' + lower(trim(line.substr(0, equals)))] = std::wstring(value);
    }
    return values;
}

// The file is read once at startup; lookups are in memory. Writes go to both.
class Config {
public:
    void load();
    void loadFrom(std::filesystem::path path); // load() for another file (tests).
    std::wstring get(std::wstring_view section, std::wstring_view key, std::wstring fallback = {}) const;
    bool set(std::wstring_view section, std::wstring_view key, const std::wstring& value);
    // Everything read, keyed "section/key" as parseConfig returns it.
    const std::map<std::wstring, std::wstring>& values() const { return values_; }

private:
    std::filesystem::path path_;
    bool utf16_ = false;
    std::map<std::wstring, std::wstring> values_;
};

} // namespace refarter
