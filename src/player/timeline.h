#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

// Pure timing rules shared by the player, the trim timeline and the cutter.
namespace refarter::player {

// "0:07", "12:30", "1:02:03"; with tenths "0:07.4". Negative times show as 0.
inline std::wstring formatTime(double seconds, bool tenths = false) {
    if (!(seconds > 0)) seconds = 0;
    // Round once, in the unit shown, so 59.96 s reads 1:00.0 rather than 0:60.0.
    const long long units = std::llround(seconds * (tenths ? 10 : 1));
    const long long whole = tenths ? units / 10 : units;
    const long long h = whole / 3600, m = whole / 60 % 60, s = whole % 60;
    wchar_t text[32];
    if (h) std::swprintf(text, std::size(text), L"%lld:%02lld:%02lld", h, m, s);
    else std::swprintf(text, std::size(text), L"%lld:%02lld", m, s);
    std::wstring result = text;
    if (tenths) result += L"." + std::to_wstring(units % 10);
    return result;
}

// The latest keyframe at or before `time` (keyframes sorted ascending). Before
// the first keyframe, or without an index, a cut can only start at 0.
inline double keyframeAtOrBefore(const std::vector<double>& keyframes, double time) {
    // A millisecond of slack: index and picture timestamps come from different time bases.
    const auto after = std::upper_bound(keyframes.begin(), keyframes.end(), time + 1e-3);
    return after == keyframes.begin() ? 0.0 : *(after - 1);
}

// A trimmed copy starts at a keyframe, so it is written without re-encoding;
// the end can be any frame. Keeps at least `minimum` seconds where possible.
struct TrimRange {
    double start = 0, end = 0;
    double length() const { return end - start; }
};
inline TrimRange snapTrim(const std::vector<double>& keyframes, double in, double out, double duration,
                          double minimum = 0.5) {
    TrimRange range;
    range.end = std::clamp(out, 0.0, duration);
    range.start = keyframeAtOrBefore(keyframes, std::clamp(in, 0.0, range.end));
    if (range.end - range.start < minimum) range.end = std::min(duration, range.start + minimum);
    return range;
}

// Where the trim's end handle goes: at least `minimum` after `in` where the clip is long enough, never past the
// clip's end (a clip shorter than `minimum` would otherwise give clamp an empty range).
inline double trimEndAt(double time, double in, double duration, double minimum = 0.5) {
    return std::clamp(time, std::min(in + minimum, duration), duration);
}

// Decides, packet by packet in decode order, which video packets a stream copy
// keeps for [start, end). The first kept packet is the keyframe at `start`;
// earlier pictures are dropped. A picture at or after `end` is held back: it
// is written only if a later packet still shows inside the range (a reference
// for reordered frames), and a keyframe at or after `end` finishes the cut.
class VideoCut {
public:
    VideoCut(double start, double end) : start_(start), end_(end) {}

    // Sequence numbers of packets to write now, in order.
    std::vector<std::size_t> push(std::size_t id, double pts, bool keyframe) {
        std::vector<std::size_t> write;
        if (done_) return write;
        constexpr double slack = 1e-4; // Timestamps arrive in stream time bases.
        if (!started_) {
            if (!keyframe || pts < start_ - slack) return write;
            started_ = true;
        }
        if (pts < start_ - slack) return write; // Leading pictures of the previous group.
        if (pts < end_ - slack) {
            write.swap(held_);
            write.push_back(id);
        } else if (keyframe) {
            done_ = true;
            held_.clear();
        } else {
            held_.push_back(id);
        }
        return write;
    }
    bool done() const { return done_; }
    // Packets waiting for a later decision; anything neither written nor held is dropped.
    const std::vector<std::size_t>& held() const { return held_; }

private:
    double start_, end_;
    bool started_ = false, done_ = false;
    std::vector<std::size_t> held_;
};

// Geometry of a horizontal timeline: times to x positions and back.
struct TimelineScale {
    double left = 0, width = 1, duration = 0;
    double x(double time) const { return duration > 0 ? left + width * std::clamp(time / duration, 0.0, 1.0) : left; }
    double time(double x) const {
        return width > 0 && duration > 0 ? std::clamp((x - left) / width, 0.0, 1.0) * duration : 0.0;
    }
};

} // namespace refarter::player
