#pragma once

#include "replay_buffer.h"

namespace refarter::capture {

// Each recorded picture lasts until the next actual capture timestamp.
// Extending only the final picture preserves sparse-source holds and audio clock alignment.
inline std::int64_t clipPacketDuration(const EncodedPacket& packet, const EncodedPacket* next,
                                     std::int64_t end, bool preserveCaptureTime) {
    if (preserveCaptureTime) {
        if (next) return next->time - packet.time; // Positive after increasingTimes().
        return std::max<std::int64_t>(1, end > packet.time ? end - packet.time : packet.duration);
    }
    return next ? packet.duration : std::max(packet.duration, end - packet.time);
}

// Where a saved clip ends on the capture clock: when the save was asked for (`requested`; 0 means `now`),
// not when the save worker reached it, which a busy queue or a waking disk can delay by seconds.
// A replay that stopped before that ends at the stop.
inline std::int64_t clipEnd(std::int64_t requested, std::int64_t now, std::int64_t stoppedAt) {
    const auto latest = stoppedAt ? stoppedAt : now;
    return requested ? std::min(requested, latest) : latest;
}

// Each picture must come after the one before. One that does not would fail
// every save until it left the replay buffer, so it takes the next possible
// time instead; only such rare packets are copied.
inline std::vector<PacketPtr> increasingTimes(std::vector<PacketPtr> packets) {
    for (std::size_t i = 1; i < packets.size(); ++i) {
        if (packets[i]->time > packets[i - 1]->time) continue;
        auto moved = std::make_shared<EncodedPacket>(*packets[i]);
        moved->time = packets[i - 1]->time + 1;
        packets[i] = std::move(moved);
    }
    return packets;
}

} // namespace refarter::capture
