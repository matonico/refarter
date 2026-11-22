#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <thread>
#include <vector>
#include "player/clip_export.h"
#include "player/combine_storage.h"

namespace refarter {

// One export (compression or lossless combination) on its own worker thread, never the recorder queue. The owner polls it
// from a timer that exists only while a job runs; nothing exists at idle.
class CompressionJob {
public:
    ~CompressionJob() { stop(); }
    // False: a job is running, the path is empty or the thread could not start.
    // `discardIntermediate` is only for a trim created by this app: remove it after publishing a distinct
    // compressed result, never on failure/cancellation. A trim that already fits is renamed to the compressed
    // name instead (`ExportResult::path`; empty, with a `cleanupWarning`, when the rename failed).
    bool start(const std::filesystem::path& clip, bool discardIntermediate = false);
    // Shares the single export slot, progress, cancellation and shutdown with compression.
    bool startCombine(std::vector<std::filesystem::path> clips, const player::CombineStorage& storage);
    bool active() const { return !clip_.empty(); }
    const std::filesystem::path& clip() const { return clip_; } // Empty when idle.
    float progress() const { return progress_.load(); }
    bool cancelling() const { return cancel_.load(); }
    void cancel() { cancel_.store(true); }
    // Once the worker has finished: joins it (never waits for encoding), goes idle and returns the result.
    std::optional<player::ExportResult> poll();
    void stop(); // Cancels and waits; for shutdown.

private:
    // The one place a worker starts: below-normal priority, result released through done_.
    bool launch(std::filesystem::path identity, std::function<player::ExportResult()> work);
    std::thread thread_;
    std::atomic<bool> cancel_ = false, done_ = false;
    std::atomic<float> progress_ = 0;
    player::ExportResult result_; // Read only after done_'s acquire, then join.
    std::filesystem::path clip_;
};

} // namespace refarter
