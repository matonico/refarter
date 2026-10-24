#pragma once

#include <algorithm>
#include <cstdint>

namespace refarter::capture {

// Admit near-rate timestamp jitter without assigning pictures to rigid slots.
// Three numerical credits bound bursts; no pictures, timers or queues live here.
// fps must be positive (Recorder validates VideoSettings before construction).
class FrameRateBudget {
public:
    static constexpr int burstFrames = 3;
    explicit FrameRateBudget(int fps) : fps_(fps) {}

    // Update refill on every new timestamp. Failed conversion/acquisition spends
    // no credit: commit() is called only once the new sample will be submitted.
    bool allows(std::int64_t time) {
        pending_ = false;
        if (time < 0 || (seen_ && time <= lastTime_)) return false;
        if (seen_) {
            const auto elapsed = time - lastTime_;
            // Saturate before multiplication: even an INT64_MAX pause is safe.
            const auto refillTime = (capacity + fps_ - 1) / fps_;
            credit_ = elapsed >= refillTime ? capacity :
                std::min(capacity, credit_ + elapsed * fps_);
        }
        seen_ = true;
        lastTime_ = time;
        pending_ = credit_ >= unit;
        return pending_;
    }

    void commit() {
        if (pending_) {
            credit_ -= unit;
            pending_ = false;
        }
    }

private:
    static constexpr std::int64_t unit = 10'000'000; // QPC time in 100 ns units.
    static constexpr std::int64_t capacity = burstFrames * unit;
    std::int64_t fps_;
    std::int64_t credit_ = capacity;
    std::int64_t lastTime_ = 0;
    bool seen_ = false;
    bool pending_ = false;
};

} // namespace refarter::capture
