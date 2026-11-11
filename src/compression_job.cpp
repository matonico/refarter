#include "compression_job.h"
#include "temporary_clip.h"

#include <windows.h>

namespace refarter {

bool CompressionJob::launch(std::filesystem::path identity, std::function<player::ExportResult()> work) {
    if (active() || identity.empty()) return false;
    clip_ = std::move(identity);
    progress_.store(0); cancel_.store(false); done_.store(false);
    try {
        thread_ = std::thread([this, work = std::move(work)] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            result_ = work();
            done_.store(true); // Release the result to the owner.
        });
    } catch (const std::exception&) {
        clip_.clear();
        return false;
    }
    return true;
}

bool CompressionJob::start(const std::filesystem::path& clip, bool discardIntermediate) {
    return launch(clip, [this, clip, discardIntermediate] {
        const auto identity = discardIntermediate ? temporaryClipIdentity(clip) : TemporaryClipIdentity{};
        auto result = player::compressClip(clip, progress_, cancel_);
        if (discardIntermediate && !cancel_.load() && !result.cancelled && result.error.empty() &&
            !result.alreadyFits && !result.path.empty() && result.path != clip &&
            !discardExportIntermediate(clip, identity, result.path))
            result.cleanupWarning = L"the temporary trim could not be removed";
        return result;
    });
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
