#pragma once

#include "replay_buffer.h"
#include "video_settings.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace refarter::capture {

struct ClipFormat {
    Size size;
    int fps = 60;
    int averageKbps = 50'000;
    Codec codec = Codec::H264;
    std::vector<std::uint8_t> sequenceHeader; // Annex B parameter sets, or the encoder's AV1 codec data.
    std::int64_t end = 0; // Packet-clock time the clip ends; the last frame is held until then.
    // Audio tracks, each 48 kHz stereo 16-bit PCM starting at the first packet.
    // Encoded to AAC while saving, so nothing encodes audio during gameplay.
    std::vector<std::vector<std::int16_t>> audio;
    // Per audio track, written as its handler name (at most 12 characters); may be empty.
    std::vector<std::string> audioNames;
    bool preserveCaptureTime = false; // Recorder enables this; direct probe callers retain their supplied durations.
};

// Writes already-encoded packets into an MP4 without re-encoding: the cost of
// a save is disk I/O, not GPU or CPU encoding. The file appears under its final
// name only once complete, and never replaces an existing file: if `path` has been taken meanwhile the next free
// name is used, and `placed` (when given) receives the name the clip ended up with. Returns an error description on failure.
std::optional<std::wstring> writeClip(const std::filesystem::path& path,
                                      const std::vector<PacketPtr>& packets, const ClipFormat& format,
                                      std::filesystem::path* placed = nullptr);

} // namespace refarter::capture
