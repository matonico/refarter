#include "clip_list.h"
#include <iostream>

using namespace refarter;

namespace {
int failures = 0;
void check(bool passes, const char* name) {
    if (!passes) { std::cerr << name << '\n'; ++failures; }
}

SYSTEMTIME day(WORD year, WORD month, WORD dayOfMonth, WORD hour = 21, WORD minute = 57) {
    SYSTEMTIME t{};
    t.wYear = year; t.wMonth = month; t.wDay = dayOfMonth; t.wHour = hour; t.wMinute = minute;
    return t;
}

ClipFile clip(const wchar_t* name, ULONGLONG time) {
    return {name, {static_cast<DWORD>(time), static_cast<DWORD>(time >> 32)}, 0};
}
} // namespace

int main() {
    check(isClipFile(L"C:\\clips\\game 2026-10-12.mp4"), "mp4 listed");
    check(isClipFile(L"D:\\Stream\\Replay.MKV"), "obs mkv listed, any case");
    check(!isClipFile(L"C:\\clips\\game.mp4.partial"), "unfinished save skipped");
    check(!isClipFile(L"C:\\clips\\notes.txt"), "other files skipped");
    check(!isClipFile(L"C:\\clips\\mp4"), "no extension skipped");

    std::vector<ClipFile> clips{clip(L"b.mp4", 100), clip(L"new.mp4", 300), clip(L"a.mp4", 100), clip(L"old.mp4", 50)};
    sortNewestFirst(clips);
    check(clips[0].path == L"new.mp4" && clips[3].path == L"old.mp4", "newest first");
    check(clips[1].path == L"a.mp4" && clips[2].path == L"b.mp4", "ties ordered by name");
    const auto original = clips[0];
    check(sameClip(original, original), "same file identity");
    clips[0].bytes = 1000;
    check(!sameClip(original, clips[0]), "replacement size changes identity");
    clips[1].bytes = 2500;
    check(clipBytes(clips) == 3500, "total includes all clips");
    check(clipBytes({}) == 0, "empty folder size");
    clips[2].bytes = UINT64_MAX;
    check(clipBytes(clips) == UINT64_MAX, "total cannot wrap");
    check(formatClipBytes(0) == L"0b", "zero bytes");
    check(formatClipBytes(999) == L"999b", "small file size");
    check(formatClipBytes(1500) == L"1.5kb", "decimal kilobytes");
    check(formatClipBytes(999999) == L"1.0mb", "rounded size promotes unit");
    check(formatClipBytes(1234567890) == L"1.2gb", "gigabytes");
    check(isCompressedCopy(L"d:\\clips\\20MB_cs2 (2).mp4") && !isCompressedCopy(L"x\\cs2 20mb_.mp4"), "compressed prefix");
    check(clipDisplayName(L"20mb_cs2 (2).mp4") == L"cs2 (2)" && clipDisplayName(L"cs2.mp4") == L"cs2", "display name");
    check(compressedSourceStem(L"20mb_cs2 (2).mp4") == L"cs2" && compressedSourceStem(L"20mb_cs2 ().mp4") == L"cs2 ()" &&
          compressedSourceStem(L"20mb_cs2.mp4") == L"cs2", "source stem");
    check(trimClipName(L" . my clip. ") == L"my clip" && trimClipName(L" .. ").empty() && trimClipName(L"a.b") == L"a.b", "name trimmed at the ends");
    check(checkClipName(L"") == ClipNameProblem::Empty && checkClipName(L"round 2 (cs2)") == ClipNameProblem::None, "empty and plain names");
    for (const auto* bad : {L"a\\b", L"a/b", L"a:b", L"a*b", L"a?b", L"a\"b", L"a<b", L"a>b", L"a|b", L"a\tb"})
        check(checkClipName(bad) == ClipNameProblem::InvalidCharacter, "invalid character rejected");
    check(checkClipName(L"con") == ClipNameProblem::Reserved && checkClipName(L"NUL.old") == ClipNameProblem::Reserved &&
          checkClipName(L"Com1") == ClipNameProblem::Reserved && checkClipName(L"lpt9") == ClipNameProblem::Reserved, "device names rejected");
    check(checkClipName(L"com0") == ClipNameProblem::None && checkClipName(L"console") == ClipNameProblem::None &&
          checkClipName(L"com10") == ClipNameProblem::None, "near device names allowed");
    check(renamedClipPath(L"d:\\clips\\old.mp4", L"ace round") == std::filesystem::path(L"d:\\clips\\ace round.mp4"), "rename keeps folder and extension");
    check(renamedClipPath(L"d:\\clips\\20MB_old (2).MKV", L"ace") == std::filesystem::path(L"d:\\clips\\20MB_ace.MKV") &&
          isCompressedCopy(renamedClipPath(L"d:\\clips\\20mb_old.mp4", L"ace")), "compressed copy keeps its prefix");
    // The library's folders: the game read from Refarter's names.
    check(clipSourceFromName(L"d:\\clips\\Apex Legends 2026-10-12 21-04-05.mp4") == L"Apex Legends", "game before the stamp");
    check(clipSourceFromName(L"Apex Legends 2026-10-12 21-04-05 (2).mp4") == L"Apex Legends", "taken name");
    check(clipSourceFromName(L"Apex Legends 2026-10-12 21-04-05 trim (3) trim.mp4") == L"Apex Legends", "trims of trims");
    check(clipSourceFromName(L"20mb_cs2 2026-10-12 21-04-05 trim (2).mp4") == L"cs2", "compressed copy");
    check(clipSourceFromName(L"display 1 2026-01-02 03-04-05.mp4") == L"display 1", "monitor capture");
    check(clipSourceFromName(L"sick clutch.mp4").empty() && clipSourceFromName(L"2026-10-12 21-04-05.mp4").empty() &&
          clipSourceFromName(L"cs2 2026-10-12 21-04.mp4").empty() && clipSourceFromName(L"cs2 2026-1x-12 21-04-05.mp4").empty(),
          "no stamp, no game");
    check(formatClipDuration(0) == L"0:00", "zero duration");
    check(formatClipDuration(594999999) == L"0:59", "duration below rounding boundary");
    check(formatClipDuration(595000000) == L"1:00", "rounded minute boundary");
    check(formatClipDuration(36610000000ULL) == L"1:01:01", "hour duration");

    const auto now = day(2026, 10, 12, 22, 5);
    check(formatClipDate(day(2026, 10, 12), now) == L"today · 21:57", "today");
    check(formatClipDate(day(2026, 10, 11, 8, 3), now) == L"yesterday · 08:03", "yesterday, zero padded");
    check(formatClipDate(day(2026, 3, 1), now) == L"1 mar · 21:57", "this year omits year");
    check(formatClipDate(day(2025, 12, 31), now) == L"31 dec 2025 · 21:57", "earlier year shown");
    check(formatClipDate(day(2026, 12, 31), day(2027, 1, 1)) == L"yesterday · 21:57", "yesterday across new year");
    check(formatClipDate(day(2024, 2, 29), day(2024, 3, 1)) == L"yesterday · 21:57", "leap day");

    // Library search: every word, any case, in the name or the game.
    const std::wstring apex = L"apex legends 2026-10-13 20-58-09";
    check(matchesSearch(apex, L"apex legends", L"") && matchesSearch(apex, L"", L"   "), "empty search matches everything");
    check(matchesSearch(apex, L"apex legends", L"APEX"), "ignores case");
    check(matchesSearch(apex, L"apex legends", L"  apex   10-13 "), "several words, extra spaces");
    check(!matchesSearch(apex, L"apex legends", L"apex league"), "every word must match");
    check(matchesSearch(L"best clutch ever", L"League of Legends", L"league clutch"), "the remembered game counts");
    check(!matchesSearch(L"best", L"apex", L"best apex zz"), "a missing word fails");
    check(!matchesSearch(L"abc", L"def", L"cd"), "no match across the name and the game");
    check(matchesSearch(L"cafÉ \U0001F430", L"", L"café \U0001F430"), "unicode names");
    return failures ? 1 : 0;
}
