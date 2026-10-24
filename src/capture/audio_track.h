#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace refarter::capture {

// Recent audio as raw 48 kHz stereo 16-bit PCM on the capture clock. Keeping
// PCM (about 11.5 MB a minute) means no audio encoding runs while you play;
// AAC is encoded only when a clip is saved.
//
// Sample n always plays at origin + n / rate, so audio lines up with video
// stamped on the same clock (QPC, 100 ns units). Gaps are filled with silence,
// which also covers loopback delivering nothing while nothing is playing.
// Device clocks drift from QPC; drift beyond `tolerance` is corrected by
// inserting silence or dropping samples.
//
// The ring is cut into one-second chunks that exist only while they hold sound:
// a track that never receives audio (Discord not running) or only silence costs
// no memory, and a new track fills up as it records instead of being committed
// whole at start.
class AudioTrack {
public:
    static constexpr int rate = 48'000;
    static constexpr int channels = 2;
    static constexpr std::int64_t tolerance = rate / 50; // 20 ms.
    static constexpr int bytesPerFrame = channels * static_cast<int>(sizeof(std::int16_t));
    // A few seconds beyond the replay cover the keyframe lead-in.
    static constexpr int leadSeconds = 5;
    static constexpr std::size_t chunkFrames = rate; // One second.
    static constexpr int ringSeconds(int replaySeconds) { return replaySeconds + leadSeconds; }
    // Most memory one ring can hold (it fills as sound arrives); the start guard and the RAM estimate both use it.
    static constexpr std::uint64_t ringBytes(int replaySeconds) {
        return static_cast<std::uint64_t>(ringSeconds(replaySeconds)) * rate * bytesPerFrame;
    }

    explicit AudioTrack(int seconds)
        : capacity_(static_cast<std::size_t>(seconds) * rate), chunks_((capacity_ + chunkFrames - 1) / chunkFrames) {}

    // `data` null means silence. `time` is when the first frame played or was recorded.
    void write(std::int64_t time, const std::int16_t* data, std::size_t frames) {
        std::lock_guard lock(mutex_);
        if (origin_ < 0) origin_ = time;
        const std::int64_t position = frameAt(time);
        if (position > written_ + tolerance) {
            fill(nullptr, static_cast<std::size_t>(position - written_)); // Gap: silence.
        } else if (position < written_ - tolerance) {
            const auto overlap = static_cast<std::size_t>(written_ - position); // Running ahead: drop.
            if (overlap >= frames) return;
            if (data) data += overlap * channels;
            frames -= overlap;
        }
        fill(data, frames);
    }

    // Frames for [from, to) on the capture clock; anything not captured is silent.
    // Copied in pieces of one chunk (about a second), holding the lock for one piece at a time, so the capture
    // thread is not held up for the whole replay right after a save. Each piece is consistent with the ring at
    // the moment it is copied: frames the writer has overwritten by then read as silence (a piece copied earlier
    // keeps the sound it had), never as newer sound from the wrong time.
    std::vector<std::int16_t> read(std::int64_t from, std::int64_t to) const {
        std::int64_t first = 0;
        bool started = false;
        {
            std::lock_guard lock(mutex_);
            started = origin_ >= 0; // Set once by the first write, never moved.
            if (started) first = frameAt(from);
        }
        const std::int64_t count = std::max<std::int64_t>(0, (to - from) * rate / 10'000'000);
        std::vector<std::int16_t> out(static_cast<std::size_t>(count) * channels); // Allocated outside the lock.
        if (!started) return out;
        for (std::int64_t i = first; i < first + count;) {
            const std::int64_t end = std::min(first + count, i + static_cast<std::int64_t>(chunkFrames));
            copyRange(i, end, first, out);
            i = end;
        }
        return out;
    }

    std::int64_t framesWritten() const { std::lock_guard lock(mutex_); return written_; }

private:
    // Copies the part of frames [lo, hi) still in the ring into `out` (whose frame 0 is `base`), under the lock.
    void copyRange(std::int64_t lo, std::int64_t hi, std::int64_t base, std::vector<std::int16_t>& out) const {
        std::lock_guard lock(mutex_);
        const auto cap = static_cast<std::int64_t>(capacity_);
        const std::int64_t oldest = std::max<std::int64_t>(0, written_ - cap);
        hi = std::min(hi, written_);
        for (std::int64_t i = std::max(lo, oldest); i < hi;) {
            const std::int64_t slot = i % cap;
            const std::int64_t run = std::min(hi - i, cap - slot); // Up to the ring's end.
            for (std::int64_t done = 0; done < run;) { // Up to the chunk's end; a missing chunk stays silent in out.
                const auto at = static_cast<std::size_t>(slot + done);
                const std::int64_t part = std::min<std::int64_t>(run - done, chunkSize(at / chunkFrames) - at % chunkFrames);
                if (const auto& chunk = chunks_[at / chunkFrames]) {
                    std::copy_n(chunk.get() + (at % chunkFrames) * channels, part * channels, out.begin() + (i - base + done) * channels);
                }
                done += part;
            }
            i += run;
        }
    }

    std::int64_t frameAt(std::int64_t time) const {
        const std::int64_t delta = time - origin_;
        return (delta * rate + (delta >= 0 ? 5'000'000 : -5'000'000)) / 10'000'000; // Nearest frame.
    }

    void fill(const std::int16_t* data, std::size_t frames) {
        // A gap longer than the ring only needs one ring's worth of silence.
        const std::size_t skipped = frames > capacity_ ? frames - capacity_ : 0;
        written_ += static_cast<std::int64_t>(skipped);
        if (data) data += skipped * channels;
        std::size_t left = frames - skipped;
        while (left > 0) {
            const auto slot = static_cast<std::size_t>(written_ % static_cast<std::int64_t>(capacity_));
            const std::size_t run = std::min(left, capacity_ - slot); // Up to the ring's end.
            for (std::size_t done = 0; done < run;) { // Up to the chunk's end.
                const std::size_t at = slot + done;
                const std::size_t index = at / chunkFrames;
                const std::size_t part = std::min(run - done, chunkSize(index) - at % chunkFrames);
                auto& chunk = chunks_[index];
                const std::size_t offset = (at % chunkFrames) * channels;
                if (data) {
                    if (!chunk) chunk = std::make_unique<std::int16_t[]>(chunkSize(index) * channels); // Zeroed.
                    std::copy_n(data, part * channels, chunk.get() + offset);
                    data += part * channels;
                } else if (chunk) {
                    if (part == chunkSize(index)) chunk.reset(); // A whole chunk of silence: give it back.
                    else std::fill_n(chunk.get() + offset, part * channels, std::int16_t{0});
                }
                done += part;
            }
            written_ += static_cast<std::int64_t>(run);
            left -= run;
        }
    }

    std::size_t chunkSize(std::size_t index) const { return std::min(chunkFrames, capacity_ - index * chunkFrames); }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<std::int16_t[]>> chunks_; // Null: silence.
    std::int64_t origin_ = -1;
    std::int64_t written_ = 0;
};

} // namespace refarter::capture
