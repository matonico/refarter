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
        // The last picture before `from` is still on screen at `from`: the next one comes later. A lone keyframe
        // starts there instead of adding the static lead-in. A longer lead-in is needed to decode that picture; it
        // moves up with it only after a stale hold (a screen that stood still), so a normal clip keeps its sound in
        // step and starts at most a keyframe interval early instead of hours before the replay asked for.
        std::size_t lead = 0;
        while (lead < packets.size() && packets[lead]->time < from) ++lead;
        if (lead > 0 && (lead == packets.size() || packets[lead]->time > from)) {
            const auto hold = from - packets[lead - 1]->time;
            if (lead == 1 || hold > staleHold) {
                for (std::size_t i = 0; i < lead; ++i) {
                    auto moved = std::make_shared<EncodedPacket>(*packets[i]);
                    if (i + 1 == lead) moved->duration = std::max<std::int64_t>(0, moved->duration - hold);
                    moved->time += hold;
                    packets[i] = std::move(moved);
                }
            }
            // A still stretch inside the lead-in itself (an encoder that ignores the forced keyframe, so one GOP
            // spans the stillness) would survive that move and start the clip hours early. Those pictures only
            // decode the one shown at the start: pack them just before it, a millisecond apart.
            const auto shown = packets[lead - 1]->time;
            if (lead > 1 && shown - packets[0]->time > staleHold) {
                for (std::size_t i = 0; i + 1 < lead; ++i) {
                    auto packed = std::make_shared<EncodedPacket>(*packets[i]);
                    packed->time = shown - static_cast<std::int64_t>(lead - 1 - i) * packedTick;
                    packed->duration = packedTick;
                    packets[i] = std::move(packed);
                }
            }
        }
        return packets;
    }

    // Longer than the encoder's one-second keyframe interval: a hold beyond it before a clip's start means the
    // source stopped updating.
    static constexpr std::int64_t staleHold = 10'000'000;
    // The spacing of lead-in pictures packed before the clip's first shown picture: 1 ms.
    static constexpr std::int64_t packedTick = 10'000;

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
    // Up to the next keyframe, found by its flag: an encoder that repeats one time on every picture must still
    // free memory.
    void dropFirstGop() {
        evicted_ = true;
        keyframes_.pop_front();
        do {
            bytes_ -= packets_.front()->data.size();
            packets_.pop_front();
        } while (!packets_.front()->keyframe);
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
