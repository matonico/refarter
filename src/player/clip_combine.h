#pragma once
#include "player/clip_export.h"
#include "player/combine_storage.h"
#include <cstdint>
#include <vector>

namespace refarter::player {
// Whether an audio packet crossing a clip's first picture is one ordinary frame (as a keyframe trim leaves), not a
// long packet that would freeze the previous clip's last picture: up to 1.5 frames at the stream's own rate.
inline bool crossingFrameFits(std::int64_t durationMicros, int frameSize, int sampleRate) {
    if (durationMicros <= 0 || sampleRate <= 0) return false;
    const std::int64_t samples = frameSize > 0 ? frameSize : 1024; // AAC-LC.
    // Divide the bounded frame allowance instead of multiplying an untrusted packet duration.
    return durationMicros <= samples * 3 * 1'000'000 / (2LL * sampleRate);
}
// Lossless packet copy, one video and the stored combined audio (or the only audio track).
// Strictly checks the retained streams before writing. Runs only on an export worker.
// An export never bins clips automatically, whatever the storage setting.
ExportResult combineClips(const std::vector<std::filesystem::path>& clips,
                          std::atomic<float>& progress, const std::atomic<bool>& cancel,
                          const CombineStorage& storage = {});
}
