#include "compression_job.h"
#include "player/clip_combine.h"

namespace refarter {
// Kept apart from compression_job.cpp so tests that only compress do not link the combine code.
bool CompressionJob::startCombine(std::vector<std::filesystem::path> clips, const player::CombineStorage& storage) {
    if (clips.size() < 2) return false;
    for (const auto& clip : clips) if (clip.empty()) return false; // active() uses the first input as the slot's identity.
    auto identity = clips.front();
    return launch(std::move(identity), [this, clips = std::move(clips), storage] {
        return player::combineClips(clips, progress_, cancel_, storage);
    });
}
}
