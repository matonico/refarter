#pragma once

#include "clip_list.h"
#include "leftover_files.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The clip folder's storage limit and low-disk rules. Pure decisions, so they can be tested without a disk:
// whether a save fits, what to do when it does not, which old clips to move to the Recycle Bin, and what the
// library strip says. The App measures the folder and the drive and acts on the answers.
namespace refarter {

enum class StorageFull { Warn, Refuse, Bin }; // What a save does once the limit would be passed.
inline constexpr const wchar_t* storageFullNames[] = {L"warn", L"refuse", L"bin"}; // As written to refarter.ini.

inline constexpr unsigned maxStorageGb = 99'999; // The number field's largest value (five digits).
inline constexpr std::uint64_t gigabyte = 1'000'000'000; // Decimal, like every size Refarter shows.
inline constexpr std::uint64_t unknownBytes = (std::numeric_limits<std::uint64_t>::max)(); // Free space that could not be read.
// Free space a save leaves alone beyond the clip itself, and the strip's alert threshold without a limit.
inline constexpr std::uint64_t saveReserve = 100'000'000;
inline constexpr std::uint64_t lowDiskFloor = 2 * gigabyte;

inline std::uint64_t limitBytes(unsigned gb) { return static_cast<std::uint64_t>(gb) * gigabyte; }

enum class StorageVerdict { Ok, OverLimit, LowDisk };

struct StorageInput {
    std::uint64_t used = 0;       // Bytes of clips in the folder now.
    std::uint64_t limit = 0;      // 0: no limit.
    std::uint64_t free = unknownBytes; // Free bytes on the clip drive.
    std::uint64_t estimate = 0;   // The clip about to be saved.
};

inline bool sumExceeds(std::uint64_t a, std::uint64_t b, std::uint64_t limit) {
    return a > limit || b > limit - a; // Overflow-safe a + b > limit.
}

// The limit is checked first: it is the user's rule. A drive that cannot hold the clip and a reserve comes second.
inline StorageVerdict checkSave(const StorageInput& in) {
    if (in.limit && sumExceeds(in.used, in.estimate, in.limit)) return StorageVerdict::OverLimit;
    if (in.free != unknownBytes && in.free < in.estimate + saveReserve) return StorageVerdict::LowDisk;
    return StorageVerdict::Ok;
}

enum class StorageAction { Save, SaveWithWarning, Refuse, BinThenSave };

// Moving clips to the Recycle Bin keeps them on the same drive, so it cannot make room on a nearly full drive:
// "bin oldest clips" only acts on the limit, and a low drive is a warning that still saves.
// "Bin oldest" is also only automatic where it cannot delete for good (see canBinAutomatically); elsewhere it warns.
inline StorageAction actionFor(StorageVerdict verdict, StorageFull behaviour, bool binAllowed = true) {
    if (verdict == StorageVerdict::Ok) return StorageAction::Save;
    if (behaviour == StorageFull::Refuse) return StorageAction::Refuse;
    if (behaviour == StorageFull::Bin && verdict == StorageVerdict::OverLimit && binAllowed) return StorageAction::BinThenSave;
    return StorageAction::SaveWithWarning;
}

// Short status lines (at most 36 characters) for a save that was stopped or went ahead with a problem.
inline std::wstring storageStatus(StorageVerdict verdict, StorageAction action) {
    if (action == StorageAction::Refuse)
        return verdict == StorageVerdict::LowDisk ? L"not saved · drive almost full" : L"not saved · storage limit reached";
    return verdict == StorageVerdict::LowDisk ? L"saving · drive almost full" : L"saving · over storage limit";
}

// While the worker plans and bins (how many is only known once it has read the folder), and when binning is not
// possible here and the save goes ahead as a warning.
inline std::wstring binningStatus() { return L"saving · binning old clips"; }
inline std::wstring binUnavailableStatus() { return L"saving · over limit · no bin here"; }

// Whether "bin oldest" may run on its own. It must never delete for good or prompt, so it only acts where Windows
// really recycles: a fixed local drive whose Recycle Bin answers, on a real volume (not a network share, a removable
// drive, a subst/junction alias). Anything else behaves as "warn".
struct BinFacts {
    bool fixedDrive = false;        // GetDriveTypeW is DRIVE_FIXED.
    bool recycleBinReadable = false; // SHQueryRecycleBinW succeeded for the drive.
    bool realVolume = false;        // The folder's volume has a volume GUID path.
};
inline bool canBinAutomatically(const BinFacts& facts) { return facts.fixedDrive && facts.recycleBinReadable && facts.realVolume; }

// The first time the limit is reached with no choice made, the user is asked once per session. A save never waits
// for a game's window to be answered: a question opens at once only while Refarter's own window is in front (the
// save came from its button); otherwise the save goes ahead as "warn" and the question waits for the next time
// the window is activated. Never while a bounded wait (shutdown, sleep) is running.
enum class Question { No, Now, Later };
inline Question questionTiming(StorageVerdict verdict, bool chosen, bool askedThisSession, bool alreadyQueued,
                               bool ownWindowForeground, bool quiet) {
    if (verdict != StorageVerdict::OverLimit || chosen || askedThisSession) return Question::No;
    if (quiet) return alreadyQueued ? Question::No : Question::Later;
    if (ownWindowForeground) return Question::Now;
    return alreadyQueued ? Question::No : Question::Later;
}

// The coming clip holds at most what is buffered: just after a start the replay is shorter than the setting.
inline int estimateSeconds(int replaySeconds, double bufferedSeconds) {
    if (!(bufferedSeconds > 0)) return std::max(replaySeconds, 0);
    const int buffered = static_cast<int>(std::min(bufferedSeconds, 1.0e6)) + 1;
    return std::clamp(buffered, 1, std::max(replaySeconds, 1));
}

struct StorageAnswer {
    StorageFull behaviour = StorageFull::Warn;
    bool persist = false; // Only a real choice is remembered.
};
// The dialog's buttons are warn, refuse, bin; anything else (the dismiss button, Esc) acts as warn and is not kept.
inline StorageAnswer resolveAnswer(int choice) {
    if (choice >= 0 && choice <= 2) return {static_cast<StorageFull>(choice), true};
    return {StorageFull::Warn, false};
}

// A path on a network share (\server\share or \?\UNC\server\share), not a local extended path (\?\C:\).
inline bool isNetworkPathText(const std::wstring& text) {
    if (text.rfind(L"\\\\?\\", 0) == 0 || text.rfind(L"\\\\.\\", 0) == 0) {
        constexpr std::wstring_view unc = L"UNC\\";
        return text.size() >= 4 + unc.size() && _wcsnicmp(text.c_str() + 4, unc.data(), unc.size()) == 0;
    }
    return text.rfind(L"\\\\", 0) == 0;
}

struct BinPlan {
    std::vector<ClipFile> bin; // Oldest first; empty when the limit cannot be met this way.
    bool reachable = false;
};

// How old a clip is, as a sortable number of local-time seconds: the time stamp in its name when it has one (the
// moment it was saved, so a clip restored from the Recycle Bin or copied back keeps its place), otherwise the file's
// creation time, otherwise its last write. Last write alone would make a restored or copied clip look new.
inline long long clipAgeKey(const ClipFile& clip) {
    namespace detail = leftover_detail;
    const std::wstring stemText = clip.path.stem().native();
    std::wstring_view stem = stemText;
    detail::stripCollision(stem);
    if (detail::endsWithStamp(stem)) {
        const auto s = stem.substr(stem.size() - 19); // "YYYY-MM-DD HH-MM-SS"
        const auto number = [&](std::size_t at, std::size_t count) {
            int value = 0;
            for (std::size_t i = 0; i < count; ++i) value = value * 10 + (s[at + i] - L'0');
            return value;
        };
        return civilDays(number(0, 4), static_cast<unsigned>(number(5, 2)), static_cast<unsigned>(number(8, 2))) * 86400 +
               number(11, 2) * 3600 + number(14, 2) * 60 + number(17, 2);
    }
    FILETIME source = clip.created.dwLowDateTime || clip.created.dwHighDateTime ? clip.created : clip.written;
    FILETIME local{};
    SYSTEMTIME time{};
    if (!FileTimeToLocalFileTime(&source, &local) || !FileTimeToSystemTime(&local, &time)) return 0;
    return civilDays(time.wYear, time.wMonth, time.wDay) * 86400 + time.wHour * 3600 + time.wMinute * 60 + time.wSecond;
}

// Which clips to move to the Recycle Bin so that the folder plus the coming clip fits the limit: the oldest first
// (clipAgeKey), only files directly in `folder`, never a favourite, a compressed copy ("20mb_") or one of `keep` (a clip
// being compressed, open, trimmed). Compressed copies still count in `used`. When all eligible clips together would still not be enough, nothing is binned: binning without
// reaching the limit helps no one.
inline BinPlan planBin(std::vector<ClipFile> clips, const std::vector<std::filesystem::path>& keep,
                       const std::filesystem::path& folder, std::uint64_t used, std::uint64_t limit, std::uint64_t estimate) {
    BinPlan plan;
    if (!limit) return plan;
    if (!sumExceeds(used, estimate, limit)) { plan.reachable = true; return plan; }
    std::vector<std::pair<long long, const ClipFile*>> oldestFirst;
    for (const auto& clip : clips) oldestFirst.emplace_back(clipAgeKey(clip), &clip);
    std::sort(oldestFirst.begin(), oldestFirst.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second->path.filename().wstring() < b.second->path.filename().wstring();
    });
    std::uint64_t remaining = used;
    for (const auto& [key, clip] : oldestFirst) {
        if (!sumExceeds(remaining, estimate, limit)) break;
        if (clip->favourite || isCompressedCopy(clip->path) || clip->path.parent_path() != folder ||
            std::find(keep.begin(), keep.end(), clip->path) != keep.end()) continue;
        plan.bin.push_back(*clip);
        remaining -= std::min(remaining, clip->bytes);
    }
    plan.reachable = !sumExceeds(remaining, estimate, limit);
    if (!plan.reachable) plan.bin.clear();
    return plan;
}

// "45.2/100.0gb": both numbers in the limit's unit, one decimal, no space.
inline std::wstring formatUsage(std::uint64_t used, std::uint64_t limit) {
    const wchar_t* units[] = {L"b", L"kb", L"mb", L"gb", L"tb", L"pb"};
    std::size_t unit = 0;
    double scale = 1;
    while (static_cast<double>(limit) / scale >= 999.95 && unit + 1 < std::size(units)) { scale *= 1000; ++unit; }
    wchar_t text[64]{};
    swprintf_s(text, L"%.1f/%.1f%s", static_cast<double>(used) / scale, static_cast<double>(limit) / scale, units[unit]);
    return text;
}

struct StripState {
    std::wstring text;
    bool alert = false;
};

inline bool lowDisk(std::uint64_t free, std::uint64_t estimate) {
    return free != unknownBytes && free < (std::max)(lowDiskFloor, estimate);
}

// The text beside the library's folder button. With a limit: how much of it is used, alert from 90%. Without
// one: the folder size, alert while the drive has less than 2 GB (or the next clip) free.
inline StripState stripState(std::uint64_t used, std::uint64_t limit, std::uint64_t free, std::uint64_t estimate) {
    StripState state;
    if (limit) {
        state.text = formatUsage(used, limit) + L" used for storage";
        state.alert = static_cast<double>(used) >= static_cast<double>(limit) * 0.9;
        if (lowDisk(free, estimate)) { state.alert = true; state.text += L" · low disk"; } // A limit does not make a full drive safe.
        return state;
    }
    state.text = formatClipBytes(used) + L" of clips in folder";
    state.alert = lowDisk(free, estimate);
    if (state.alert) state.text += L" · low disk";
    return state;
}

// The note after "bin oldest" ran: empty when nothing was needed (no note), the count when clips were moved.
inline std::wstring binNote(const std::wstring& problem, std::size_t binned) {
    if (!problem.empty()) return L"could not bin old clips · saved anyway";
    if (!binned) return {};
    return L"moved " + std::to_wstring(binned) + (binned == 1 ? L" old clip" : L" old clips") + L" to the recycle bin";
}

} // namespace refarter
