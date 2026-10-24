#include "storage_limit.h"

#include <iostream>

using namespace refarter;

namespace {
// A larger `age` is older.
ClipFile clip(const wchar_t* name, std::uint64_t bytes, unsigned long long age, bool favourite = false) {
    ClipFile c;
    c.path = std::filesystem::path(L"C:\\clips") / name;
    c.bytes = bytes;
    const unsigned long long ticks = (200'000ull - age) * 10'000'000ull * 60;
    c.created = {static_cast<DWORD>(ticks & 0xFFFFFFFF), static_cast<DWORD>(ticks >> 32)};
    c.written = {0xFFFFFFFF, 0x01FFFFFF}; // Always the newest: the age must not come from the last write.
    c.favourite = favourite;
    return c;
}
} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    constexpr std::uint64_t mb = 1'000'000;

    // Verdicts.
    check(checkSave({10 * mb, 0, unknownBytes, 5 * mb}) == StorageVerdict::Ok, "no limit and unknown space is fine");
    check(checkSave({90 * mb, 100 * mb, unknownBytes, 10 * mb}) == StorageVerdict::Ok, "exactly at the limit still fits");
    check(checkSave({91 * mb, 100 * mb, unknownBytes, 10 * mb}) == StorageVerdict::OverLimit, "just over the limit");
    check(checkSave({0, 5 * mb, unknownBytes, 10 * mb}) == StorageVerdict::OverLimit, "a clip bigger than the limit never fits");
    check(checkSave({unknownBytes - 1, 100, unknownBytes, 10}) == StorageVerdict::OverLimit, "huge sizes do not overflow");
    check(checkSave({1, 0, 50 * mb, 10 * mb}) == StorageVerdict::LowDisk, "free space below clip plus reserve is low");
    check(checkSave({1, 0, 10 * mb + saveReserve, 10 * mb}) == StorageVerdict::Ok, "free space equal to clip plus reserve is enough");
    check(checkSave({95 * mb, 100 * mb, 1 * mb, 10 * mb}) == StorageVerdict::OverLimit, "the limit is reported before a low drive");
    check(checkSave({1, 0, 0, 10 * mb}) == StorageVerdict::LowDisk, "a full drive is low");

    // Actions per behaviour.
    check(actionFor(StorageVerdict::Ok, StorageFull::Refuse) == StorageAction::Save, "ok always saves");
    check(actionFor(StorageVerdict::OverLimit, StorageFull::Warn) == StorageAction::SaveWithWarning, "warn saves with a warning");
    check(actionFor(StorageVerdict::OverLimit, StorageFull::Refuse) == StorageAction::Refuse, "refuse refuses at the limit");
    check(actionFor(StorageVerdict::LowDisk, StorageFull::Refuse) == StorageAction::Refuse, "refuse refuses on a low drive");
    check(actionFor(StorageVerdict::OverLimit, StorageFull::Bin) == StorageAction::BinThenSave, "bin makes room at the limit");
    check(actionFor(StorageVerdict::LowDisk, StorageFull::Bin) == StorageAction::SaveWithWarning, "bin cannot free a low drive");

    // Status texts fit the status line.
    for (const auto verdict : {StorageVerdict::OverLimit, StorageVerdict::LowDisk})
        for (const auto action : {StorageAction::Refuse, StorageAction::SaveWithWarning})
            check(storageStatus(verdict, action).size() <= 36, "status stays short");
    check(storageStatus(StorageVerdict::OverLimit, StorageAction::Refuse) == L"not saved \u00b7 storage limit reached", "refuse wording");
    check(binningStatus().size() <= 36 && binUnavailableStatus().size() <= 36, "bin wording fits");
    check(binningStatus().find(L"binned") == std::wstring::npos, "binning is not claimed done before the result");

    // The one-time question.
    using Q = Question;
    check(questionTiming(StorageVerdict::OverLimit, false, false, false, true, false) == Q::Now, "asks at once only over Refarter's own window");
    check(questionTiming(StorageVerdict::OverLimit, false, false, false, false, false) == Q::Later, "over a game the question waits");
    check(questionTiming(StorageVerdict::OverLimit, false, false, true, false, false) == Q::No, "a queued question is not queued twice");
    check(questionTiming(StorageVerdict::OverLimit, false, false, false, true, true) == Q::Later, "never during a bounded wait");
    check(questionTiming(StorageVerdict::OverLimit, true, false, false, true, false) == Q::No, "never asks once a choice was made");
    check(questionTiming(StorageVerdict::OverLimit, false, true, false, true, false) == Q::No, "asks once per session");
    check(questionTiming(StorageVerdict::LowDisk, false, false, false, true, false) == Q::No, "a low drive does not ask");
    check(actionFor(StorageVerdict::OverLimit, StorageFull::Bin, false) == StorageAction::SaveWithWarning, "bin without a safe recycle bin warns");
    check(actionFor(StorageVerdict::OverLimit, StorageFull::Refuse, false) == StorageAction::Refuse, "refuse ignores the bin");
    check(canBinAutomatically({true, true, true}) && !canBinAutomatically({false, true, true}) &&
          !canBinAutomatically({true, false, true}) && !canBinAutomatically({true, true, false}), "bin needs a fixed, readable, real volume");
    check(estimateSeconds(120, 30.2) == 31 && estimateSeconds(120, 500) == 120 && estimateSeconds(120, 0) == 120 && estimateSeconds(120, 0.1) == 1,
          "the estimate is capped by what is buffered");
    check(resolveAnswer(0).behaviour == StorageFull::Warn && resolveAnswer(0).persist, "warn is kept");
    check(resolveAnswer(1).behaviour == StorageFull::Refuse && resolveAnswer(1).persist, "refuse is kept");
    check(resolveAnswer(2).behaviour == StorageFull::Bin && resolveAnswer(2).persist, "bin is kept");
    check(resolveAnswer(-1).behaviour == StorageFull::Warn && !resolveAnswer(-1).persist, "dismiss acts as warn and is not kept");
    check(resolveAnswer(3).behaviour == StorageFull::Warn && !resolveAnswer(3).persist, "a fourth button is a dismiss too");

    check(!isNetworkPathText(L"C:\\clips") && !isNetworkPathText(L"\\\\?\\C:\\clips") && !isNetworkPathText(L""), "local paths are not network paths");
    check(isNetworkPathText(L"\\\\server\\share") && isNetworkPathText(L"\\\\?\\UNC\\server\\share") && isNetworkPathText(L"\\\\?\\unc\\server\\share"),
          "shares are network paths, in either form");

    // Which clips to bin.
    {
        const std::vector<ClipFile> clips = {clip(L"new.mp4", 40 * mb, 1), clip(L"mid.mp4", 40 * mb, 5), clip(L"old.mp4", 40 * mb, 9),
                                             clip(L"oldest.mp4", 40 * mb, 12), clip(L"fav.mp4", 40 * mb, 20, true)};
        const std::filesystem::path folder = L"C:\\clips";
        const auto plan = planBin(clips, {}, folder, 200 * mb, 200 * mb, 70 * mb);
        check(plan.reachable && plan.bin.size() == 2, "two oldest non-favourites free enough room");
        check(plan.bin.size() == 2 && plan.bin[0].path.filename() == L"oldest.mp4" && plan.bin[1].path.filename() == L"old.mp4",
              "oldest first, favourite skipped");
        const auto none = planBin(clips, {}, folder, 100 * mb, 200 * mb, 70 * mb);
        check(none.reachable && none.bin.empty(), "nothing to bin when it already fits");
        const auto keep = planBin(clips, {std::filesystem::path(L"C:\\clips\\oldest.mp4")}, folder, 200 * mb, 200 * mb, 70 * mb);
        check(keep.reachable && keep.bin.size() == 2 && keep.bin[0].path.filename() == L"old.mp4" && keep.bin[1].path.filename() == L"mid.mp4",
              "a protected clip is never binned");
        const auto hopeless = planBin(clips, {}, folder, 200 * mb, 50 * mb, 70 * mb);
        check(!hopeless.reachable && hopeless.bin.empty(), "a clip bigger than the limit bins nothing");
        const auto shortOf = planBin(clips, {}, folder, 200 * mb, 100 * mb, 90 * mb);
        check(!shortOf.reachable && shortOf.bin.empty(), "binning that cannot reach the limit bins nothing");
        check(planBin(clips, {}, folder, 200 * mb, 0, 10 * mb).bin.empty(), "no limit bins nothing");
        const std::vector<ClipFile> onlyFavourites = {clip(L"a.mp4", 90 * mb, 3, true)};
        check(planBin(onlyFavourites, {}, folder, 90 * mb, 100 * mb, 20 * mb).bin.empty(), "favourites are never binned");

        // Compressed copies count towards the limit but are never binned, even when they are the oldest.
        {
            const std::filesystem::path kept = L"C:\\clips\\kept.mp4";
            const std::vector<ClipFile> mixed = {clip(L"20mb_ancient.mp4", 40 * mb, 99), clip(L"old.mp4", 40 * mb, 9), clip(L"fav.mp4", 40 * mb, 50, true),
                                                 clip(L"kept.mp4", 40 * mb, 30), clip(L"new.mp4", 40 * mb, 1)};
            const auto ordinary = planBin(mixed, {kept}, folder, 200 * mb, 200 * mb, 70 * mb);
            check(ordinary.reachable && ordinary.bin.size() == 2 && ordinary.bin[0].path.filename() == L"old.mp4" &&
                  ordinary.bin[1].path.filename() == L"new.mp4",
                  "a compressed copy, a favourite and a protected clip are skipped; ordinary clips go oldest first");
            const std::vector<ClipFile> protectedMostly = {clip(L"20mb_a.mp4", 40 * mb, 9), clip(L"fav.mp4", 40 * mb, 8, true),
                                                           clip(L"kept.mp4", 40 * mb, 7), clip(L"small.mp4", 10 * mb, 6)};
            const auto unreachable = planBin(protectedMostly, {kept}, folder, 130 * mb, 100 * mb, 20 * mb);
            check(!unreachable.reachable && unreachable.bin.empty(), "without the protected files the limit is unreachable: nothing is binned");
            check(planBin({clip(L"20MB_A.MP4", 90 * mb, 3)}, {}, folder, 90 * mb, 100 * mb, 20 * mb).bin.empty(), "the prefix is matched without case");
        }

        // Age comes from the name's time stamp, then the creation time; never the last write.
        const auto stamped = [&](const wchar_t* name, unsigned long long createdAge) {
            auto c = clip(name, 40 * mb, createdAge);
            return c;
        };
        const std::vector<ClipFile> restored = {stamped(L"game 2026-01-05 10-00-00.mp4", 1), // Old by name, created just now (restored).
                                                stamped(L"game 2026-03-01 10-00-00.mp4", 50)};
        const auto first = planBin(restored, {}, folder, 80 * mb, 80 * mb, 40 * mb);
        check(first.reachable && first.bin.size() == 1 && first.bin[0].path.filename() == L"game 2026-01-05 10-00-00.mp4",
              "a restored clip is binned by its name's time, not first by its new creation time");
        check(clipAgeKey(stamped(L"a 2026-01-05 10-00-00 (2).mp4", 1)) == clipAgeKey(stamped(L"a 2026-01-05 10-00-00.mp4", 99)),
              "a collision suffix does not change the stamp");
        check(clipAgeKey(stamped(L"x 2026-01-05 10-00-00.mp4", 1)) < clipAgeKey(stamped(L"x 2026-01-05 10-00-01.mp4", 1)), "stamps order by the second");
        check(clipAgeKey(stamped(L"old.mp4", 100)) < clipAgeKey(stamped(L"new.mp4", 1)), "without a stamp the creation time orders");
        // Only files directly in the clip folder.
        auto elsewhere = clip(L"old.mp4", 40 * mb, 50);
        elsewhere.path = std::filesystem::path(L"C:\\clips\\sub") / L"old.mp4";
        check(planBin({elsewhere}, {}, folder, 120 * mb, 100 * mb, 10 * mb).bin.empty(), "a clip outside the folder is never binned");
    }

    // Strip text and colour.
    {
        const auto s = stripState(45'200'000'000, 100 * gigabyte, unknownBytes, 0);
        check(s.text == L"45.2/100.0gb used for storage" && !s.alert, "strip with a limit");
        check(!stripState(89 * gigabyte, 100 * gigabyte, unknownBytes, 0).alert, "89% is calm");
        check(stripState(90 * gigabyte, 100 * gigabyte, unknownBytes, 0).alert, "90% alerts");
        check(stripState(100 * gigabyte, 100 * gigabyte, unknownBytes, 0).alert, "100% alerts");
        check(stripState(120 * gigabyte, 100 * gigabyte, unknownBytes, 0).alert, "over 100% alerts");
        check(stripState(450 * mb, 500 * mb, unknownBytes, 0).text == L"450.0/500.0mb used for storage", "small limits use mb");
        check(stripState(0, 5 * gigabyte, unknownBytes, 0).text == L"0.0/5.0gb used for storage", "empty folder");
        const auto plain = stripState(3'500'000'000, 0, 50 * gigabyte, 100 * mb);
        check(plain.text == L"3.5gb of clips in folder" && !plain.alert, "no limit shows the folder size");
        check(stripState(3'500'000'000, 0, 1 * gigabyte, 100 * mb).alert, "under 2 gb free alerts");
        check(!stripState(3'500'000'000, 0, 2 * gigabyte, 100 * mb).alert, "exactly 2 gb free is calm");
        check(stripState(3'500'000'000, 0, 3 * gigabyte, 5 * gigabyte).alert, "free below the next clip alerts");
        check(!stripState(3'500'000'000, 0, unknownBytes, 5 * gigabyte).alert, "unknown free space does not alert");
        check(stripState(3'500'000'000, 0, 1 * gigabyte, 0).text.find(L"low disk") != std::wstring::npos, "alert says why");
        const auto limited = stripState(1 * gigabyte, 100 * gigabyte, 1 * gigabyte, 100 * mb);
        check(limited.alert && limited.text.find(L"low disk") != std::wstring::npos, "a low drive alerts even well under the limit");
        check(stripState(1 * gigabyte, 100 * gigabyte, 3 * gigabyte, 5 * gigabyte).alert, "limit: free below the next clip alerts");
        check(!stripState(1 * gigabyte, 100 * gigabyte, 50 * gigabyte, 100 * mb).alert, "limit: plenty of room is calm");
        check(!stripState(1 * gigabyte, 100 * gigabyte, unknownBytes, 100 * mb).alert, "limit: unknown free space does not alert");
        check(binNote({}, 0).empty(), "nothing needed binning: no note");
        check(binNote({}, 1) == L"moved 1 old clip to the recycle bin" && binNote({}, 3) == L"moved 3 old clips to the recycle bin", "bin note counts clips");
        check(!binNote(L"problem", 0).empty(), "a failure is noted");
    }

    return failures ? 1 : 0;
}
