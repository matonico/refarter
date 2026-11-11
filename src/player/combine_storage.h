#pragma once
#include <cstdint>

namespace refarter::player {
// What an export may use. Light on purpose: compression_job.h includes it.
struct CombineStorage {
    std::uint64_t folderLimit = 0;   // Zero: no limit.
    std::uint64_t reserve = 0;       // Free space left for replay saves (the app and the probe pass saveReserve, plus a clip in the app).
    bool refuseOverLimit = true;     // Otherwise the export goes ahead past the limit and reports overLimit.
};
inline bool combineOverLimit(std::uint64_t estimate, std::uint64_t used, std::uint64_t limit) {
    return limit && (used > limit || estimate > limit - used); // Overflow-safe used + estimate > limit.
}
// Conservative preflight, not a reservation: recording and other applications may write afterwards.
// A full drive always refuses; the folder limit refuses only when the storage setting says so.
inline const wchar_t* combineStorageProblem(std::uint64_t estimate, std::uint64_t free,
                                           std::uint64_t used, std::uint64_t limit, std::uint64_t reserve,
                                           bool refuseOverLimit = true) {
    if (estimate > free || reserve > free - estimate)
        return L"not enough free disk space to combine these clips and leave room for replay saves";
    if (refuseOverLimit && combineOverLimit(estimate, used, limit))
        return L"combining would exceed the clip folder's storage limit; make room, increase the limit or choose \"warn and save\" in settings";
    return nullptr;
}
}
