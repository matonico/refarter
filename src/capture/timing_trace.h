#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace refarter::capture {

enum class TimingKind { Submitted, PacerSkip, SurfaceBusy, ConversionFailed,
                        QueueDisplaced, CaptureError, Encoded };

struct TimingEvent {
    TimingKind kind;
    std::int64_t captureTime = 0; // Raw WGC timestamp; zero for encoder output.
    std::int64_t arrivalTime = 0; // QPC, same 100 ns clock as WGC.
    std::int64_t sampleTime = -1; // Recorder grid timestamp, relative to capture start.
    std::int64_t workTime = 0; // Callback conversion/submission wall time, not CPU time.
    std::uint64_t bytes = 0; // Compressed packet size, for encoder output only.
    int foreground = -1; // Selected window is foreground; -1 for monitor/output events.
};

struct TimingSnapshot {
    std::vector<TimingEvent> events;
    std::uint64_t overwritten = 0;
};

// Diagnostic only: allocated at explicit opt-in, never grows on the frame path.
// Capture and encoder callbacks have different threads; snapshots are written
// by the existing save worker. Snapshot order is insertion order, not timestamp order.
class TimingTrace {
public:
    explicit TimingTrace(std::size_t capacity = 65'536) : ring_(capacity) {}

    void record(TimingEvent event) {
        std::lock_guard lock(mutex_);
        if (ring_.empty()) { ++overwritten_; return; }
        ring_[next_] = event;
        next_ = (next_ + 1) % ring_.size();
        if (count_ < ring_.size()) ++count_;
        else ++overwritten_;
    }

    TimingSnapshot snapshot() const {
        std::lock_guard lock(mutex_);
        TimingSnapshot result;
        result.overwritten = overwritten_;
        result.events.reserve(count_);
        const auto first = count_ == ring_.size() ? next_ : 0;
        for (std::size_t i = 0; i < count_; ++i)
            result.events.push_back(ring_[(first + i) % ring_.size()]);
        return result;
    }

    // A failed diagnostic export must never change the clip's save result.
    bool write(const std::filesystem::path& path, std::int64_t clockStart,
               std::int64_t clipFrom, std::int64_t clipTo, int fps, int width,
               int height, int codec, bool sourceThrottled, bool preserveCaptureTime = false,
               std::int64_t lastSample = -1, std::size_t clipPackets = 0, int frameBudget = 0) const noexcept {
        try {
            const auto data = snapshot();
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << "# refarter timing v1; all times in 100 ns; diagnostic overhead included\n"
                << "# clock_start=" << clockStart << "; clip_from=" << clipFrom
                << "; clip_to=" << clipTo << "; fps=" << fps
                << "; width=" << width << "; height=" << height << "; codec=" << codec
                << "; source_throttled=" << sourceThrottled
                << "; capture_timestamps=" << preserveCaptureTime
                << "; frame_budget=" << frameBudget
                << "; clip_last_sample=" << lastSample << "; clip_packets=" << clipPackets
                << "; overwritten_events=" << data.overwritten << '\n'
                << "kind,capture_time,arrival_time,sample_time,work_time,bytes,foreground\n";
            constexpr const char* names[]{"submitted", "pacer_skip", "surface_busy",
                "conversion_failed", "queue_displaced", "capture_error", "encoded"};
            for (const auto& e : data.events)
                out << names[static_cast<int>(e.kind)] << ',' << e.captureTime << ','
                    << e.arrivalTime << ',' << e.sampleTime << ',' << e.workTime << ','
                    << e.bytes << ',' << e.foreground << '\n';
            out.flush();
            return out.good();
        } catch (...) { return false; }
    }

private:
    mutable std::mutex mutex_;
    std::vector<TimingEvent> ring_;
    std::size_t next_ = 0, count_ = 0;
    std::uint64_t overwritten_ = 0;
};

} // namespace refarter::capture
