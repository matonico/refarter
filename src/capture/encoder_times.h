#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace refarter::capture {

// Capture times of the pictures given to the encoder and not yet back. Media Foundation encoders should stamp each
// output with its input's time, and the replay buffer relies on it: one picture's time decides what a clip spans.
// An output without a time, or with one that was never given, takes the oldest time still waiting instead: without
// B-frames pictures come back in the order they went in. Not thread-safe; the encoder calls it under its lock.
class EncoderTimes {
public:
    // Pictures the encoder dropped never come back; their times are forgotten after this many newer ones.
    static constexpr std::size_t limit = 64;

    void submitted(std::int64_t time) {
        waiting_.push_back(time);
        if (waiting_.size() > limit) waiting_.pop_front();
    }

    // The time for the next output, given the one the encoder stamped on it (none when it stamped nothing).
    std::int64_t output(std::optional<std::int64_t> stamped) {
        if (stamped) {
            if (const auto it = std::find(waiting_.begin(), waiting_.end(), *stamped); it != waiting_.end()) {
                // Outputs come back in order, so every older picture still waiting was dropped: forget those too,
                // or the next unstamped output would take a dropped picture's (possibly much older) time.
                waiting_.erase(waiting_.begin(), it + 1);
                return last_ = *stamped;
            }
            // A picture whose time an earlier, wrongly stamped output already took: keep the encoder's own stamp
            // rather than shifting this and every later picture by one.
            if (const auto it = std::find(guessed_.begin(), guessed_.end(), *stamped); it != guessed_.end()) {
                guessed_.erase(guessed_.begin(), it + 1);
                return last_ = *stamped;
            }
        }
        if (waiting_.empty()) return last_ = stamped.value_or(last_); // Nothing to go by.
        ++repaired_;
        last_ = waiting_.front();
        waiting_.pop_front();
        guessed_.push_back(last_);
        if (guessed_.size() > 4) guessed_.pop_front();
        return last_;
    }

    // Outputs whose time was replaced.
    std::size_t repaired() const { return repaired_; }

private:
    std::deque<std::int64_t> waiting_;
    std::deque<std::int64_t> guessed_; // The last few times handed to outputs without their own (recent repairs).
    std::int64_t last_ = 0;
    std::size_t repaired_ = 0;
};

} // namespace refarter::capture
