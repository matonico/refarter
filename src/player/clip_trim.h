#pragma once

#include "player/timeline.h"
#include "temporary_clip.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace refarter::player {

// "<name> trim.mp4" beside the source, then "<name> trim (2).mp4" and so on.
std::filesystem::path trimmedPath(const std::filesystem::path& source);

struct TrimResult {
    bool ok = false;
    std::filesystem::path path;
    std::wstring error;
    TemporaryClipIdentity identity; // Captured before publication, for late cancellation cleanup.
};

// The audio a trimmed clip keeps, as the player's source switches left it.
struct TrimAudio {
    // Audio track indices (file order) copied as they are; empty keeps every track.
    std::vector<int> keep;
    // Encode a new "mix" of the kept tracks, placed first as the default track:
    // the stored mix also holds the sources left out.
    bool mix = false;
    // Names written for the kept tracks, in `keep` order (older clips carry none); may be empty.
    std::vector<std::string> names;
    // Kept tracks the new mix leaves out (the game while desktop audio, which holds it, is kept).
    std::vector<int> apart;
    // Keep no audio track at all (video only); wins over `keep`, which cannot say "none" because empty means all.
    bool none = false;
    // The trim changes the audio, so even the whole clip is worth saving again.
    bool changesAudio() const { return none || !keep.empty(); }
};

// Copies [range.start, range.end) of `source` into a new MP4 without
// re-encoding: every picture and sound keeps its original quality, and all
// audio tracks (mix, desktop, microphone, discord) are kept with the mix as
// default, or only those in `audio` (then only a new mix is encoded, AAC).
// range.start must be a keyframe (see snapTrim). Runs on the caller's thread;
// `progress` goes 0 → 1. The file only appears under its name once complete.
TrimResult trimClip(const std::filesystem::path& source, TrimRange range, std::atomic<float>& progress,
                    const std::atomic<bool>& cancel, const TrimAudio& audio = {});

} // namespace refarter::player
