#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <iterator>
#include <mutex>
#include <vector>

namespace refarter::capture {

// Times are in 100 ns units, matching Media Foundation.
struct EncodedPacket {
    std::int64_t time = 0;
    std::int64_t duration = 0;
    bool keyframe = false;
    std::vector<std::uint8_t> data;
};
using PacketPtr = std::shared_ptr<const EncodedPacket>;

// Rolling window of compressed video kept in memory. It always begins on a
// keyframe and evicts whole GOPs, so any snapshot decodes without the encoder.
// Packets are shared, so a save in progress never blocks or copies the live buffer.
class ReplayBuffer {
public:
    ReplayBuffer(std::int64_t retain, std::size_t maxBytes) : retain_(retain), maxBytes_(maxBytes) {}

    void push(PacketPtr packet) {
        std::lock_guard lock(mutex_);
        if (packets_.empty() && !packet->keyframe) return; // Undecodable without a prior keyframe.
        if (packet->keyframe) keyframes_.push_back(packet->time);
        bytes_ += packet->data.size();
        packets_.push_back(std::move(packet));
        const auto end = packets_.back()->time + packets_.back()->duration;
        while (keyframes_.size() >= 2 && (end - keyframes_[1] >= retain_ || bytes_ > maxBytes_)) dropFirstGop();
    }

    // Packets covering at least `duration` before `end` (when available),
    // starting on a keyframe. An `end` of 0 (the default) means the newest packet;
    // pass the save time so a static screen or paused game still counts. Pictures
    // stamped after a positive `end` were captured after the save was asked for and are left out.
    // Empty when `end` is before the oldest picture still held (see evicted()).
    std::vector<PacketPtr> snapshot(std::int64_t duration, std::int64_t end = 0) const {
        std::vector<PacketPtr> packets;
        std::int64_t from = 0;
        {
            std::lock_guard lock(mutex_);
            auto stop = packets_.end();
            // From the back: only the pictures since the save request are passed over. Times rise in push
            // order (the encoder stamps them from the capture clock); the rare picture that does not is
            // repaired only when the clip is written, and here it merely moves the cut by one picture.
            while (end > 0 && stop != packets_.begin() && (*std::prev(stop))->time > end) --stop;
            if (stop == packets_.begin()) return {};
            const auto& newest = *std::prev(stop);
            from = std::max(end, newest->time + newest->duration) - duration;
            auto start = packets_.begin();
            for (auto it = packets_.begin(); it != stop && (*it)->time <= from; ++it) {
                if ((*it)->keyframe) start = it;
            }
            packets.assign(start, stop);
        }
        // A lone keyframe before `from` is still on screen at `from`: the next
        // picture comes later. Start it there instead of adding the static lead-in.
        if (packets.front()->time < from && (packets.size() == 1 || packets[1]->time > from)) {
            auto moved = std::make_shared<EncodedPacket>(*packets.front());
            moved->duration = std::max<std::int64_t>(0, moved->duration - (from - moved->time));
            moved->time = from;
            packets.front() = std::move(moved);
        }
        return packets;
    }

    std::size_t bytes() const { std::lock_guard lock(mutex_); return bytes_; }
    std::int64_t span() const {
        std::lock_guard lock(mutex_);
        return packets_.empty() ? 0 : packets_.back()->time + packets_.back()->duration - packets_.front()->time;
    }
    // True once a whole GOP has been dropped since the last clear(): pictures older than the
    // oldest one held are gone, as opposed to never having been recorded.
    bool evicted() const { std::lock_guard lock(mutex_); return evicted_; }
    void clear() {
        std::lock_guard lock(mutex_);
        packets_.clear();
        keyframes_.clear();
        bytes_ = 0;
        evicted_ = false;
    }

private:
    void dropFirstGop() {
        evicted_ = true;
        keyframes_.pop_front();
        while (packets_.front()->time != keyframes_.front() || !packets_.front()->keyframe) {
            bytes_ -= packets_.front()->data.size();
            packets_.pop_front();
        }
    }

    const std::int64_t retain_;
    const std::size_t maxBytes_;
    mutable std::mutex mutex_;
    std::deque<PacketPtr> packets_;
    std::deque<std::int64_t> keyframes_;
    std::size_t bytes_ = 0;
    bool evicted_ = false;
};

} // namespace refarter::capture
