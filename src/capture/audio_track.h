#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace refarter::capture {

// Recent audio as raw 48 kHz 16-bit PCM on the capture clock, stereo (about
// 11.5 MB a minute) or mono for the microphone (half that). Keeping PCM means no
// audio encoding runs while you play; AAC is encoded only when a clip is saved.
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
    // A few seconds beyond the replay cover the keyframe lead-in.
    static constexpr int leadSeconds = 5;
    static constexpr std::size_t chunkFrames = rate; // One second.
    static constexpr int ringSeconds(int replaySeconds) { return replaySeconds + leadSeconds; }
    // Most memory one ring storing `stored` channels can hold (it fills as sound arrives); the start guard and the
    // RAM estimate both use it.
    static constexpr std::uint64_t ringBytes(int replaySeconds, int stored = channels) {
        return static_cast<std::uint64_t>(ringSeconds(replaySeconds)) * rate * static_cast<std::uint64_t>(stored) * sizeof(std::int16_t);
    }

    // Writes and reads are always stereo. `stored` 1 keeps one channel (the microphone, see sourceChannels):
    // write() averages the two, read() repeats it on both.
    explicit AudioTrack(int seconds, int stored = channels)
        : capacity_(static_cast<std::size_t>(seconds) * rate), stored_(static_cast<std::size_t>(stored)),
          chunks_((capacity_ + chunkFrames - 1) / chunkFrames) {}

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
        const auto span = spanOf(from, to);
        std::vector<std::int16_t> out(static_cast<std::size_t>(span.count) * channels); // Allocated outside the lock.
        copy(span, 0, static_cast<std::size_t>(span.count), out.data());
        return out;
    }

    // The frames read() would return for [from, to), to check them for sound or copy them out a piece at a time
    // (a save copies a span and keeps only the channels the ring stores).
    struct Span {
        std::int64_t first = 0, count = 0;
        bool started = false; // Nothing recorded yet: every frame is silent.
    };
    Span spanOf(std::int64_t from, std::int64_t to) const {
        Span span;
        span.count = frameCount(from, to);
        std::lock_guard lock(mutex_);
        span.started = origin_ >= 0; // Set once by the first write, never moved.
        if (span.started) span.first = frameAt(from);
        return span;
    }
    static std::int64_t frameCount(std::int64_t from, std::int64_t to) { return std::max<std::int64_t>(0, (to - from) * rate / 10'000'000); }
    // Frames [offset, offset + frames) of `span` into `out` (frames * channels samples), as read() copies them.
    void copy(const Span& span, std::size_t offset, std::size_t frames, std::int16_t* out) const {
        std::fill_n(out, frames * channels, std::int16_t{0});
        if (!span.started) return;
        const std::int64_t lo = span.first + static_cast<std::int64_t>(offset), hi = lo + static_cast<std::int64_t>(frames);
        for (std::int64_t i = lo; i < hi;) {
            const std::int64_t end = std::min(hi, i + static_cast<std::int64_t>(chunkFrames));
            copyRange(i, end, lo, out);
            i = end;
        }
    }
    // True when any frame of `span` holds sound; read a second at a time, never copied whole.
    bool sounds(const Span& span) const {
        std::vector<std::int16_t> piece(chunkFrames * channels);
        for (std::size_t at = 0; at < static_cast<std::size_t>(span.count); at += chunkFrames) {
            const auto frames = std::min(chunkFrames, static_cast<std::size_t>(span.count) - at);
            copy(span, at, frames, piece.data());
            if (std::any_of(piece.begin(), piece.begin() + static_cast<std::ptrdiff_t>(frames * channels), [](std::int16_t s) { return s != 0; }))
                return true;
        }
        return false;
    }

    int storedChannels() const { return static_cast<int>(stored_); } // 2, or 1 for a mono ring.
    std::int64_t framesWritten() const { std::lock_guard lock(mutex_); return written_; }

private:
    // Copies the part of frames [lo, hi) still in the ring into `out` (whose frame 0 is `base`), under the lock.
    void copyRange(std::int64_t lo, std::int64_t hi, std::int64_t base, std::int16_t* out) const {
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
                    const std::int16_t* from = chunk.get() + (at % chunkFrames) * stored_;
                    std::int16_t* to = out + (i - base + done) * channels;
                    if (stored_ == channels) std::copy_n(from, part * channels, to);
                    else for (std::int64_t f = 0; f < part; ++f) to[2 * f] = to[2 * f + 1] = from[f]; // Mono: both channels.
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
                const std::size_t offset = (at % chunkFrames) * stored_;
                if (data) {
                    if (!chunk) chunk = std::make_unique<std::int16_t[]>(chunkSize(index) * stored_); // Zeroed.
                    if (stored_ == channels) std::copy_n(data, part * channels, chunk.get() + offset);
                    else for (std::size_t f = 0; f < part; ++f) // Mono: the two channels averaged, as RNNoise takes them.
                        chunk[offset + f] = static_cast<std::int16_t>((data[2 * f] + data[2 * f + 1]) / 2);
                    data += part * channels;
                } else if (chunk) {
                    if (part == chunkSize(index)) chunk.reset(); // A whole chunk of silence: give it back.
                    else std::fill_n(chunk.get() + offset, part * stored_, std::int16_t{0});
                }
                done += part;
            }
            written_ += static_cast<std::int64_t>(run);
            left -= run;
        }
    }

    std::size_t chunkSize(std::size_t index) const { return std::min(chunkFrames, capacity_ - index * chunkFrames); }

    const std::size_t capacity_;
    const std::size_t stored_; // Channels held per frame: 2, or 1 for a mono ring.
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<std::int16_t[]>> chunks_; // Null: silence.
    std::int64_t origin_ = -1;
    std::int64_t written_ = 0;
};

} // namespace refarter::capture
