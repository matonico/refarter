#pragma once

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "text.h"

// Unfinished files that only Refarter creates while it writes in the clip folder. A crash, a power cut or a forced
// shutdown can leave them behind. Found at startup and moved into a recovery subfolder; nothing is deleted or overwritten.
//   <name> YYYY-MM-DD HH-MM-SS[ (N)].mp4.partial   a save in progress (capture/clip_writer.cpp; the name is clipFileName's)
//   <name> YYYY-MM-DD HH-MM-SS[ (N)].png.partial   a screenshot in progress (capture/screenshot.cpp), in <clip folder>\screenshots
//   refarter-export-{GUID}.part      a compression in progress (player/clip_export.cpp)
//   <stem> trim[ (N)].mp4.part       a trim in progress (player/clip_trim.cpp); <stem> is itself a clip stem as above
// The clip folder and its "screenshots" subfolder are both scanned (findAllLeftovers); a screenshot leftover moves to
// the recovery folder beside it, <clip folder>\screenshots\recovery, so recovered pictures stay apart from clips.
namespace refarter {

// Only files untouched for this long are considered; anything younger may belong to a job that is still running.
inline constexpr std::chrono::seconds leftoverMinAge{60};

// Where screenshots are written, beside the clips (named by RecordingController::screenshot).
inline constexpr wchar_t leftoverScreenshotsFolder[] = L"screenshots";

namespace leftover_detail {
inline bool endsWith(std::wstring_view text, std::wstring_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}
inline bool isHex(wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); }
// {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}, as StringFromGUID2 writes it.
inline bool isBracedGuid(std::wstring_view text) {
    if (text.size() != 38 || text.front() != L'{' || text.back() != L'}') return false;
    for (size_t i = 1; i + 1 < text.size(); ++i) {
        const bool dash = i == 9 || i == 14 || i == 19 || i == 24;
        if (dash ? text[i] != L'-' : !isHex(text[i])) return false;
    }
    return true;
}
// Every " (N)" at the end of `text` (the suffix that avoids a name collision, repeated when a copy of a copy collides)
// is removed, with N all digits. True when at least one was there.
inline bool stripCollision(std::wstring_view& text) {
    bool stripped = false;
    while (text.size() >= 4 && text.back() == L')') {
        const size_t open = text.rfind(L" (");
        if (open == std::wstring_view::npos) break;
        const auto digits = text.substr(open + 2, text.size() - open - 3);
        if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) break;
        text = text.substr(0, open);
        stripped = true;
    }
    return stripped;
}
inline bool digitsAt(std::wstring_view text, size_t at, size_t count, int low, int high) {
    int value = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto c = text[at + i];
        if (c < L'0' || c > L'9') return false;
        value = value * 10 + (c - L'0');
    }
    return value >= low && value <= high;
}
// The clip name's stamp, " YYYY-MM-DD HH-MM-SS" (capture/clip_folder.cpp), after a non-empty game or window name.
inline bool endsWithStamp(std::wstring_view text) {
    constexpr size_t length = 20;
    if (text.size() <= length) return false;
    const auto s = text.substr(text.size() - length);
    return s[0] == L' ' && digitsAt(s, 1, 4, 1601, 9999) && s[5] == L'-' && digitsAt(s, 6, 2, 1, 12) && s[8] == L'-' &&
           digitsAt(s, 9, 2, 1, 31) && s[11] == L' ' && digitsAt(s, 12, 2, 0, 23) && s[14] == L'-' &&
           digitsAt(s, 15, 2, 0, 59) && s[17] == L'-' && digitsAt(s, 18, 2, 0, 60);
}
// "<name> YYYY-MM-DD HH-MM-SS" with the optional " (N)": the stem of a clip Refarter saved.
inline bool isClipStem(std::wstring_view stem) {
    stripCollision(stem);
    return endsWithStamp(stem);
}
// A clip stem, a compressed copy's ("20mb_" is part of the name) or either one trimmed, once or again.
inline bool isRefarterStem(std::wstring_view stem) {
    constexpr std::wstring_view trim = L" trim";
    for (;;) {
        std::wstring_view collisionFree = stem;
        stripCollision(collisionFree);
        if (collisionFree.size() > trim.size() && endsWith(collisionFree, trim)) { stem = collisionFree.substr(0, collisionFree.size() - trim.size()); continue; }
        break;
    }
    return isClipStem(stem);
}
// "<refarter stem> trim" or "<refarter stem> trim (N)": what clip_trim.cpp names the result, whose ".part" is the partial.
inline bool isTrimStem(std::wstring_view stem) {
    constexpr std::wstring_view trim = L" trim";
    stripCollision(stem);
    if (stem.size() <= trim.size() || !endsWith(stem, trim)) return false;
    return isRefarterStem(stem.substr(0, stem.size() - trim.size()));
}
} // namespace leftover_detail

// Whether a file name (not a path) is one of the four unfinished-job names above, exactly as Refarter writes them.
// Look-alikes (a browser's "video.mp4.part", an old Edge "x.mp4.partial", "movie trim.mp4.part") are left alone.
inline bool isLeftoverName(std::wstring_view original) {
    using namespace leftover_detail;
    const auto name = lower(original);
    constexpr std::wstring_view partialSuffix = L".mp4.partial", pngPartialSuffix = L".png.partial", partSuffix = L".mp4.part",
                                exportPrefix = L"refarter-export-", exportSuffix = L".part";
    if (endsWith(name, partialSuffix)) return isClipStem(std::wstring_view(name).substr(0, name.size() - partialSuffix.size()));
    if (endsWith(name, pngPartialSuffix)) return isClipStem(std::wstring_view(name).substr(0, name.size() - pngPartialSuffix.size()));
    if (name.size() == exportPrefix.size() + 38 + exportSuffix.size() && name.rfind(exportPrefix, 0) == 0 && endsWith(name, exportSuffix))
        return isBracedGuid(std::wstring_view(name).substr(exportPrefix.size(), 38));
    if (endsWith(name, partSuffix)) return isTrimStem(std::wstring_view(name).substr(0, name.size() - partSuffix.size()));
    return false;
}

// The leftover files directly inside `folder` (not its subfolders) last written at least `minAge` before `now`.
inline std::vector<std::filesystem::path> findLeftovers(const std::filesystem::path& folder,
                                                        std::chrono::seconds minAge = leftoverMinAge,
                                                        std::filesystem::file_time_type now = std::filesystem::file_time_type::clock::now()) {
    std::vector<std::filesystem::path> found;
    std::error_code error;
    std::filesystem::directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, error), end;
    for (; !error && it != end; it.increment(error)) {
        std::error_code ignored;
        if (!it->is_regular_file(ignored) || it->is_symlink(ignored) || !isLeftoverName(it->path().filename().wstring())) continue;
        const auto written = it->last_write_time(ignored);
        if (!ignored && now - written >= minAge) found.push_back(it->path());
    }
    return found;
}

// The leftovers of the clip folder and of its "screenshots" subfolder (the only subfolder Refarter writes to).
inline std::vector<std::filesystem::path> findAllLeftovers(const std::filesystem::path& folder,
                                                           std::chrono::seconds minAge = leftoverMinAge,
                                                           std::filesystem::file_time_type now = std::filesystem::file_time_type::clock::now()) {
    auto found = findLeftovers(folder, minAge, now);
    const auto shots = findLeftovers(folder / leftoverScreenshotsFolder, minAge, now);
    found.insert(found.end(), shots.begin(), shots.end());
    return found;
}

// Same-volume, no-overwrite moves into a "recovery" folder beside each file (<clip folder>/recovery, or
// <clip folder>/screenshots/recovery for a screenshot), with the original names intact.
// Locked, changed, fresh, reparse or colliding files stay in place. No shell delete API is used.
inline constexpr wchar_t leftoverRecoveryFolder[] = L"recovery";
int recoverLeftovers(const std::vector<std::filesystem::path>& files);

} // namespace refarter
