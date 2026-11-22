#pragma once

#include "text.h"

#include <algorithm>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

namespace detail {
inline bool wholeNumber(std::wstring_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}
// "1.6", "1.6.15": numbers joined by dots, at least two.
inline bool dottedNumber(std::wstring_view text) {
    int parts = 0;
    for (std::size_t start = 0;; ++parts) {
        const auto dot = text.find(L'.', start);
        if (!wholeNumber(text.substr(start, dot == std::wstring_view::npos ? std::wstring_view::npos : dot - start))) return false;
        if (dot == std::wstring_view::npos) return parts >= 1;
        start = dot + 1;
    }
}
// A version, build or platform tag a window title adds after the game's name (lower case): "1.6.15", "v2", "build-116",
// "(64-bit)", "[dx12]", "(64-bit, dx11)". A plain number ("battlefield 4", "fifa 23") is part of the name and is not a tag.
// `versionOnly`: versions and builds only, not platforms ("vulkan" can be a word of a name).
inline bool titleTag(std::wstring_view token, bool versionOnly = false) {
    if (token.size() >= 2 && ((token.front() == L'(' && token.back() == L')') || (token.front() == L'[' && token.back() == L']'))) {
        token = token.substr(1, token.size() - 2);
        // Several tags in one bracket: a tag only if every part is one ("(early access, v2)" stays).
        if (token.find(L',') != std::wstring_view::npos) {
            for (std::size_t start = 0;;) {
                const auto comma = token.find(L',', start);
                auto part = token.substr(start, comma == std::wstring_view::npos ? std::wstring_view::npos : comma - start);
                while (!part.empty() && part.front() == L' ') part.remove_prefix(1);
                while (!part.empty() && part.back() == L' ') part.remove_suffix(1);
                if (part.empty() || !titleTag(part, versionOnly)) return false;
                if (comma == std::wstring_view::npos) return true;
                start = comma + 1;
            }
        }
    }
    std::wstring plain;
    for (const wchar_t c : token) if (c != L' ' && c != L'-' && c != L'_') plain += c;
    const std::wstring_view text(plain);
    const auto numbered = [&](std::wstring_view prefix) {
        if (!text.starts_with(prefix)) return false;
        const auto rest = text.substr(prefix.size());
        return wholeNumber(rest) || dottedNumber(rest);
    };
    if (dottedNumber(text) || numbered(L"v") || numbered(L"build")) return true;
    if (versionOnly) return false;
    for (const wchar_t* word : {L"64bit", L"32bit", L"x64", L"x86", L"win64", L"win32", L"vulkan", L"opengl", L"directx"})
        if (text == word) return true;
    return numbered(L"directx") || numbered(L"dx") || numbered(L"d3d");
}

// Where the first version or build tag after the name's first word starts ("1.6.15", "v2", "build 116",
// "[build-116]"), or npos. Bracketed groups are one word.
inline std::size_t firstVersion(std::wstring_view name) {
    std::size_t pos = 0;
    for (bool first = true; pos < name.size(); first = false) {
        while (pos < name.size() && name[pos] == L' ') ++pos;
        if (pos >= name.size()) break;
        std::size_t end = name.find(L' ', pos);
        if (name[pos] == L'(' || name[pos] == L'[') {
            const auto close = name.find(name[pos] == L'(' ? L')' : L']', pos);
            end = close == std::wstring_view::npos ? std::wstring_view::npos : close + 1;
        }
        if (end == std::wstring_view::npos) end = name.size();
        const auto token = name.substr(pos, end - pos);
        if (!first) {
            if (titleTag(token, true)) {
                // A bare two-part number ("2.8", "1.5") can be part of a name ("kingdom hearts hd 2.8 final chapter
                // prologue"): it cuts only at the end of the title or before a dash ("stardew valley 1.6 - smapi").
                if (!dottedNumber(token) || std::count(token.begin(), token.end(), L'.') > 1) return pos;
                auto next = name.substr(end);
                while (!next.empty() && next.front() == L' ') next.remove_prefix(1);
                next = next.substr(0, next.find(L' '));
                if (next.empty() || next == L"-" || next == L"–" || next == L"—") return pos;
                pos = end;
                continue;
            }
            // "build 116": the word and the number after it.
            if (token == L"build") {
                auto next = name.substr(end);
                while (!next.empty() && next.front() == L' ') next.remove_prefix(1);
                if (wholeNumber(next.substr(0, next.find(L' ')))) return pos;
            }
        }
        pos = end;
    }
    return std::wstring_view::npos;
}
} // namespace detail

// A game's short name, display only (App::displayGameName): its stored name or window title (lower case) up to the first
// "|" or the first version or build tag after its first word, without platform tags at its end; the executable without
// ".exe" when nothing is left. "stardew valley 1.6.15 - running smapi" is "stardew valley"; "lichii | build-116 | …"
// is "lichii". A dash alone does not cut: "batman – arkham knight" and "s.t.a.l.k.e.r. 2 - heart of chornobyl" stay.
// " · " cuts too, which drops what the source picker added to labels saved before ("minecraft · javaw.exe").
// Stored names, clip names and the source picker keep the whole title (friendlySourceName).
inline std::wstring gameName(std::wstring_view title, std::wstring_view executable) {
    constexpr std::wstring_view edges = L" \t-|—–·:,";
    const auto trimEdges = [&](std::wstring& text) {
        const auto first = text.find_first_not_of(edges);
        if (first == std::wstring::npos) return text.clear();
        text.erase(text.find_last_not_of(edges) + 1);
        text.erase(0, first);
    };
    std::wstring name = friendlySourceName(title, {});
    auto cut = std::wstring::npos;
    for (const wchar_t* separator : {L"|", L" · "}) cut = std::min(cut, name.find(separator));
    if (cut != std::wstring::npos) name.resize(cut);
    trimEdges(name);
    if (const auto version = detail::firstVersion(name); version != std::wstring::npos) {
        name.resize(version);
        trimEdges(name);
    }
    while (!name.empty()) {
        // The last word, or the last bracketed group ("(64 bit)" holds a space).
        std::size_t start = 0;
        if (name.back() == L')' || name.back() == L']') {
            start = name.rfind(name.back() == L')' ? L'(' : L'[');
            if (start == std::wstring::npos) break;
        } else if (const auto space = name.find_last_of(L' '); space != std::wstring::npos) start = space + 1;
        const std::wstring_view last = std::wstring_view(name).substr(start);
        // A word left alone is the name ("v8"); only a bare number or bracket of tags gives way to the executable.
        if (start == 0 && last.front() != L'(' && last.front() != L'[' && !detail::dottedNumber(last)) break;
        if (!detail::titleTag(last)) {
            // "build 116": the number and the word before it.
            auto before = std::wstring_view(name).substr(0, start);
            while (!before.empty() && before.back() == L' ') before.remove_suffix(1);
            if (!detail::wholeNumber(last) || !before.ends_with(L"build") ||
                (before.size() > 5 && before[before.size() - 6] != L' ')) break;
            start = before.size() - 5;
        }
        name.resize(start);
        trimEdges(name);
    }
    if (name.empty()) {
        // The file name, whether given alone (a target's name) or as a full path.
        if (const auto slash = executable.find_last_of(L"\\/"); slash != std::wstring_view::npos) executable.remove_prefix(slash + 1);
        name = friendlySourceName({}, executable);
        if (name.size() > 4 && name.ends_with(L".exe")) name.resize(name.size() - 4);
    }
    return name;
}

// What the source picker added to a window's label to tell like-named windows apart (" · javaw.exe", " · window 2");
// empty when it added nothing. Kept after a game's short name so the footer and tray still say which window it is.
// Read from the label's end (the picker's forms of sourceLabels), not by its title: a game may change its window title
// after it was picked.
inline std::wstring pickerSuffix(std::wstring_view label, std::wstring_view executable) {
    std::wstring_view rest = label, ordinal;
    constexpr std::wstring_view window = L" · window ";
    if (const auto at = rest.rfind(window); at != std::wstring_view::npos && detail::wholeNumber(rest.substr(at + window.size()))) {
        ordinal = rest.substr(at);
        rest = rest.substr(0, at);
    }
    const auto exe = L" · " + friendlySourceName({}, executable);
    const bool named = exe.size() > 3 && rest.size() > exe.size() && rest.ends_with(exe);
    return (named ? exe : std::wstring{}) + std::wstring(ordinal);
}

// The app in front when a whole-monitor clip or screenshot is requested.
struct ForegroundApp {
    bool listed = false;    // Its executable is on the auto-capture list.
    bool onMonitor = false; // Its window is on the recorded monitor.
    std::wstring title, executable;
};

// A whole-monitor save is named after the game in front, as a capture of its window would be, when the user listed it
// and it is on the recorded monitor. Otherwise (nothing, Refarter, another app or monitor in front) after the monitor.
inline std::wstring monitorSaveName(std::wstring_view monitor, const std::optional<ForegroundApp>& front) {
    if (front && front->listed && front->onMonitor) return friendlySourceName(front->title, front->executable);
    return lower(std::wstring(monitor));
}

// `shown`: the name the source row, footer and tray may show for the window instead of its title (its short or custom
// name); empty when that is the title itself.
struct SourceName { std::wstring title, executable, shown; };

// Only duplicate names expose executable identity: alike titles, or alike shown names (two titles that shorten alike).
// Like windows from the same executable also get an ordinal, so every menu choice, and every shown source, is distinct.
// Titles are cut first (shownName), so two long titles cut alike are told apart the same way.
inline std::vector<std::wstring> sourceLabels(const std::vector<SourceName>& sources) {
    std::vector<std::wstring> names, shown, executables, labels;
    for (const auto& source : sources) {
        names.push_back(shownName(friendlySourceName(source.title, source.executable)));
        shown.push_back(source.shown.empty() ? names.back() : shownName(source.shown));
        executables.push_back(friendlySourceName({}, source.executable));
    }
    // duplicate: alike another window; numbered: alike another window of the same executable.
    std::vector<bool> duplicate(names.size()), numbered(names.size());
    for (std::size_t i = 0; i < names.size(); ++i)
        for (std::size_t j = i + 1; j < names.size(); ++j) {
            if (names[i] != names[j] && shown[i] != shown[j]) continue;
            duplicate[i] = duplicate[j] = true;
            if (executables[i] == executables[j]) numbered[i] = numbered[j] = true;
        }
    labels = names;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (!duplicate[i]) continue;
        if (!executables[i].empty() && executables[i] != names[i]) labels[i] += L" · " + executables[i];
        if (!numbered[i]) continue;
        // Counted among this executable's numbered windows, so two of them never share a number.
        int ordinal = 0;
        for (std::size_t j = 0; j <= i; ++j)
            if (numbered[j] && executables[j] == executables[i]) ++ordinal;
        labels[i] += L" · window " + std::to_wstring(ordinal);
    }
    return labels;
}

} // namespace refarter
