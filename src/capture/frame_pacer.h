#pragma once

#include <cstdint>
#include <optional>

namespace refarter::capture {

// Legacy comparison selector. Normal recording uses FrameRateBudget instead.
// Maps capture times (100 ns) onto a constant-rate frame grid. Frames go to
// the nearest slot so a source running at or near the recording rate is not
// aliased into skipped and doubled frames; the first frame per slot wins.
class FramePacer {
public:
    explicit FramePacer(int fps) : fps_(fps) {}

    // The grid slot for a frame captured at `time`, or nothing if that slot
    // already has a frame. Call commit() once the frame is actually used.
    std::optional<std::int64_t> slotFor(std::int64_t time) {
        if (start_ < 0) start_ = time;
        const std::int64_t quarters = (time - start_) * fps_ * 4 / unit; // Position in quarter slots.
        std::int64_t slot = (quarters + 2) / 4;                           // Nearest slot.
        // A slightly late frame (under 3/4 of a slot) fills the slot it
        // would otherwise leave empty, instead of creating a gap.
        if (last_ >= 0 && slot > last_ + 1 && quarters < last_ * 4 + 7) slot = last_ + 1;
        if (slot <= last_) return std::nullopt;
        return slot;
    }
    void commit(std::int64_t slot) { last_ = slot; }

    std::int64_t time(std::int64_t slot) const { return slot * unit / fps_; }
    // Keep slot selection identical in both modes; only presentation timestamps differ.
    std::int64_t sampleTime(std::int64_t slot, std::int64_t captureTime, bool preserveCaptureTime) const {
        return preserveCaptureTime ? captureTime - start_ : time(slot);
    }
    std::int64_t duration() const { return unit / fps_; }

private:
    static constexpr std::int64_t unit = 10'000'000;
    std::int64_t fps_;
    std::int64_t start_ = -1;
    std::int64_t last_ = -1;
};

} // namespace refarter::capture
