#pragma once

#include "text.h"

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace refarter {

struct ClipFile {
    std::filesystem::path path;
    FILETIME written{}; // Last write: when the clip finished saving, and kept when copied.
    std::uint64_t bytes = 0;
    bool favourite = false;
    FILETIME created{}; // When the file was created here (a restored or copied clip is new); zero if unknown.
    std::wstring source; // The game remembered for a renamed clip (see clip_sources.h); empty when none is.
};

inline bool sameClip(const ClipFile& a, const ClipFile& b) {
    return a.path == b.path && a.bytes == b.bytes && CompareFileTime(&a.written, &b.written) == 0;
}

inline std::uint64_t clipBytes(const std::vector<ClipFile>& clips) {
    std::uint64_t total = 0;
    for (const auto& clip : clips) {
        if (clip.bytes > std::numeric_limits<std::uint64_t>::max() - total)
            return std::numeric_limits<std::uint64_t>::max();
        total += clip.bytes;
    }
    return total;
}

// Logical video-file sizes, in decimal units; not allocated space or drive usage.
inline std::wstring formatClipBytes(std::uint64_t bytes) {
    if (bytes < 1000) return std::to_wstring(bytes) + L"b";
    double value = static_cast<double>(bytes);
    const wchar_t* units[] = {L"kb", L"mb", L"gb", L"tb", L"pb", L"eb"};
    std::size_t unit = 0;
    value /= 1000;
    while (value >= 999.95 && unit + 1 < std::size(units)) { value /= 1000; ++unit; }
    wchar_t text[32]{};
    swprintf_s(text, L"%.1f%s", value, units[unit]);
    return text;
}

// Shell media duration is in 100-nanosecond units. Round to the nearest second.
inline std::wstring formatClipDuration(std::uint64_t ticks) {
    const auto seconds = ticks / 10000000 + (ticks % 10000000 >= 5000000 ? 1 : 0);
    wchar_t text[48]{};
    if (seconds >= 3600) swprintf_s(text, L"%llu:%02llu:%02llu", seconds / 3600, seconds / 60 % 60, seconds % 60);
    else swprintf_s(text, L"%llu:%02llu", seconds / 60, seconds % 60);
    return text;
}

// Videos a player can open. Unfinished saves end in ".partial" and are skipped.
inline bool isClipFile(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    extension = lower(extension);
    for (const auto* kind : {L".mp4", L".mkv", L".mov", L".m4v", L".webm", L".avi", L".wmv"}) {
        if (extension == kind) return true;
    }
    return false;
}

// Compressed copies are named "20mb_<original name>" (see player::exportPath), with " (n)"
// before the extension when that name was taken.
inline constexpr wchar_t compressedPrefix[] = L"20mb_";

inline bool isCompressedCopy(const std::filesystem::path& path) {
    auto name = path.filename().wstring();
    name = lower(name);
    return name.rfind(compressedPrefix, 0) == 0;
}

// The name shown for a clip: a compressed copy loses its prefix.
inline std::wstring clipDisplayName(const std::filesystem::path& path) {
    auto stem = path.stem().wstring();
    return isCompressedCopy(path) ? stem.substr(std::size(compressedPrefix) - 1) : stem;
}

// The stem of the clip a compressed copy was made from: no prefix, no trailing " (n)".
inline std::wstring compressedSourceStem(const std::filesystem::path& path) {
    auto stem = clipDisplayName(path);
    const auto open = stem.rfind(L" (");
    if (open != std::wstring::npos && stem.size() > open + 3 && stem.back() == L')' &&
        std::all_of(stem.begin() + open + 2, stem.end() - 1, [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
        stem.erase(open);
    return stem;
}

// The game or source a clip was saved from, read from Refarter's own names: "<game> 2026-10-12 21-04-05",
// also with " (2)", " trim" (a trimmed copy, any number of times) and the "20mb_" prefix. Empty when the
// name carries no time stamp: renamed, or not saved by Refarter.
inline std::wstring clipSourceFromName(const std::filesystem::path& path) {
    auto stem = clipDisplayName(path);
    const auto strip = [&](std::wstring_view tail) {
        if (stem.size() <= tail.size() || stem.compare(stem.size() - tail.size(), tail.size(), tail) != 0) return false;
        stem.resize(stem.size() - tail.size());
        return true;
    };
    for (bool more = true; more;) {
        more = strip(L" trim");
        const auto open = stem.rfind(L" (");
        if (open != std::wstring::npos && stem.size() > open + 3 && stem.back() == L')' &&
            std::all_of(stem.begin() + open + 2, stem.end() - 1, [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
            stem.resize(open);
            more = true;
        }
    }
    constexpr std::wstring_view stamp = L" 0000-00-00 00-00-00";
    if (stem.size() <= stamp.size()) return {};
    for (std::size_t i = 0; i < stamp.size(); ++i) {
        const wchar_t c = stem[stem.size() - stamp.size() + i];
        if (stamp[i] == L'0' ? c < L'0' || c > L'9' : c != stamp[i]) return {};
    }
    stem.resize(stem.size() - stamp.size());
    stem.erase(stem.find_last_not_of(L' ') + 1);
    return stem;
}

// The library's search: every word typed, ignoring case, appears in the clip's name as the library shows it or in its
// game ("apex 10-13" finds "apex legends 2026-10-13 20-58-09"). An empty search, or one of spaces, matches every clip.
inline bool matchesSearch(const std::wstring& name, const std::wstring& game, const std::wstring& search) {
    // Windows' lower-casing, not the C library's, so "É" finds "é" too; it never changes the length.
    const auto fold = [](std::wstring text) {
        if (!text.empty()) CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
        return text;
    };
    const auto haystack = fold(name + L'\n' + game);
    const auto words = fold(search);
    for (std::size_t start = 0; start < words.size();) {
        const auto first = words.find_first_not_of(L" \t", start);
        if (first == std::wstring::npos) break;
        const auto end = std::min(words.find_first_of(L" \t", first), words.size());
        if (haystack.find(words.substr(first, end - first)) == std::wstring::npos) return false;
        start = end;
    }
    return true;
}

// Renaming: what a typed name may be. Spaces and dots at the ends are dropped, as Windows would.
inline std::wstring trimClipName(const std::wstring& name) {
    const auto first = name.find_first_not_of(L" .");
    if (first == std::wstring::npos) return {};
    return name.substr(first, name.find_last_not_of(L" .") - first + 1);
}

enum class ClipNameProblem { None, Empty, InvalidCharacter, Reserved };

// What a rename returns when Windows refused the move (access denied, file in use), as opposed to a refused name.
inline constexpr const wchar_t* renameFailedText = L"could not rename this clip (it may be open in another app).";

// For an already trimmed name: no \ / : * ? " < > | or control characters, and not a device name
// (CON, PRN, AUX, NUL, COM1-9, LPT1-9), which Windows also reserves with any extension.
inline ClipNameProblem checkClipName(const std::wstring& name) {
    if (name.empty()) return ClipNameProblem::Empty;
    for (const auto c : name)
        if (c < L' ' || std::wstring_view(L"\\/:*?\"<>|").find(c) != std::wstring_view::npos)
            return ClipNameProblem::InvalidCharacter;
    auto base = name.substr(0, name.find(L'.'));
    base.erase(base.find_last_not_of(L' ') + 1);
    std::transform(base.begin(), base.end(), base.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL") return ClipNameProblem::Reserved;
    if (base.size() == 4 && (base.rfind(L"COM", 0) == 0 || base.rfind(L"LPT", 0) == 0) && base[3] >= L'1' && base[3] <= L'9')
        return ClipNameProblem::Reserved;
    return ClipNameProblem::None;
}

// The file a clip is renamed to: same folder and extension; a compressed copy keeps its prefix
// so it stays under Compressed.
inline std::filesystem::path renamedClipPath(const std::filesystem::path& path, const std::wstring& name) {
    const auto prefix = isCompressedCopy(path) ? path.filename().wstring().substr(0, std::size(compressedPrefix) - 1) : std::wstring{};
    return path.parent_path() / (prefix + name + path.extension().wstring());
}

// Newest first; equal times fall back to the name so the order is stable between scans.
inline void sortNewestFirst(std::vector<ClipFile>& clips) {
    std::sort(clips.begin(), clips.end(), [](const ClipFile& a, const ClipFile& b) {
        const auto order = CompareFileTime(&a.written, &b.written);
        return order != 0 ? order > 0 : a.path.filename().wstring() < b.path.filename().wstring();
    });
}

// Days since 1970-01-01 for a calendar date (proleptic Gregorian).
inline long long civilDays(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const long long era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

// "today · 21:57", "yesterday · 18:03", "12 oct · 21:57" this year, "12 oct 2025 · 21:57" before.
// Both times are local.
inline std::wstring formatClipDate(const SYSTEMTIME& when, const SYSTEMTIME& now) {
    static const wchar_t* months[] = {L"jan", L"feb", L"mar", L"apr", L"may", L"jun",
                                      L"jul", L"aug", L"sep", L"oct", L"nov", L"dec"};
    wchar_t clock[8]{};
    swprintf_s(clock, L"%02u:%02u", when.wHour, when.wMinute);
    const auto age = civilDays(now.wYear, now.wMonth, now.wDay) - civilDays(when.wYear, when.wMonth, when.wDay);
    std::wstring day;
    if (age == 0) day = L"today";
    else if (age == 1) day = L"yesterday";
    else {
        day = std::to_wstring(when.wDay) + L" " + months[(when.wMonth + 11) % 12];
        if (when.wYear != now.wYear) day += L" " + std::to_wstring(when.wYear);
    }
    return day + L" · " + clock;
}

// The same for a file's last write time (UTC); `now` is local.
inline std::wstring formatClipDate(const FILETIME& written, const SYSTEMTIME& now) {
    // The zone's rule for that date (daylight saving then), not today's offset as FileTimeToLocalFileTime applies.
    SYSTEMTIME utc{}, when{};
    if (!FileTimeToSystemTime(&written, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &when)) when = utc;
    return formatClipDate(when, now);
}

} // namespace refarter
