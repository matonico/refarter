#include "compression_job.h"
#include "temporary_clip.h"

#include <windows.h>

namespace refarter {

bool CompressionJob::start(const std::filesystem::path& clip, bool discardIntermediate) {
    if (active() || clip.empty()) return false;
    clip_ = clip;
    progress_.store(0); cancel_.store(false); done_.store(false);
    try {
        thread_ = std::thread([this, clip, discardIntermediate] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            const auto identity = discardIntermediate ? temporaryClipIdentity(clip) : TemporaryClipIdentity{};
            result_ = player::compressClip(clip, progress_, cancel_);
            if (discardIntermediate && !cancel_.load() && !result_.cancelled && result_.error.empty() &&
                !result_.alreadyFits && !result_.path.empty() && result_.path != clip &&
                !discardExportIntermediate(clip, identity, result_.path))
                result_.cleanupWarning = L"the temporary trim could not be removed";
            done_.store(true); // Release the result to the owner.
        });
    } catch (const std::exception&) {
        clip_.clear();
        return false;
    }
    return true;
}

std::optional<player::ExportResult> CompressionJob::poll() {
    if (!active() || !done_.load()) return std::nullopt;
    thread_.join(); // The thread has finished.
    clip_.clear();
    return std::move(result_);
}

void CompressionJob::stop() {
    cancel_.store(true);
    if (thread_.joinable()) thread_.join();
}

} // namespace refarter
