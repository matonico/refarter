#pragma once

#include "replay_buffer.h"
#include "video_settings.h"

#include <cstdint>
#include <filesystem>
#include <functional>
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
    // Audio tracks, one per name (written as its handler name, at most 12 characters; may be empty), each
    // `audioFrames` of 48 kHz 16-bit PCM starting at the first packet, interleaved with the track's `audioChannels`
    // (2 when missing; 1 for the microphone). `audio` gives the next frames of every track, in order, while the clip
    // is written, so a save never holds whole tracks. Encoded to AAC while saving, so nothing encodes audio during
    // gameplay.
    std::vector<std::string> audioNames;
    std::vector<int> audioChannels;
    std::size_t audioFrames = 0;
    std::function<void(std::size_t frames, std::vector<std::vector<std::int16_t>>& tracks)> audio;
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
