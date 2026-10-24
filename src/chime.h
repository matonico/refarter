#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace refarter {

// Scales the samples of a 16-bit PCM WAV in place, 0-100%. PlaySound has no
// volume of its own, and the process-wide wave volume would also change other
// sounds, so the save chime is played from a scaled copy. Returns false if the
// bytes are not a 16-bit PCM WAV; the data is then left untouched.
inline bool scaleWavVolume(std::span<std::uint8_t> wav, int percent) {
    const auto read32 = [&](std::size_t at) {
        std::uint32_t value = 0;
        std::memcpy(&value, wav.data() + at, 4);
        return value;
    };
    if (wav.size() < 12 || std::memcmp(wav.data(), "RIFF", 4) || std::memcmp(wav.data() + 8, "WAVE", 4)) return false;
    bool pcm16 = false;
    for (std::size_t at = 12; at + 8 <= wav.size();) {
        const auto size = static_cast<std::size_t>(read32(at + 4));
        const auto body = at + 8;
        if (body + size > wav.size()) return false;
        if (!std::memcmp(wav.data() + at, "fmt ", 4) && size >= 16) {
            std::uint16_t format = 0, bits = 0;
            std::memcpy(&format, wav.data() + body, 2);
            std::memcpy(&bits, wav.data() + body + 14, 2);
            pcm16 = format == 1 && bits == 16;
        } else if (!std::memcmp(wav.data() + at, "data", 4)) {
            if (!pcm16) return false;
            percent = std::clamp(percent, 0, 100);
            for (std::size_t i = body; i + 2 <= body + size; i += 2) {
                std::int16_t sample = 0;
                std::memcpy(&sample, wav.data() + i, 2);
                sample = static_cast<std::int16_t>(sample * percent / 100);
                std::memcpy(wav.data() + i, &sample, 2);
            }
            return true;
        }
        at = body + size + (size & 1); // Chunks are padded to even sizes.
    }
    return false;
}

// The save chime and the failed-save sound, played from embedded WAV resources at one volume.
class Chimes {
public:
    enum class Kind { Saved, Failed };
    int percent() const { return percent_; }
    // Follows the volume slider while it moves; prepare() then rebuilds the scaled copies.
    void setPercent(int percent) { percent_ = percent; }
    // Sets the volume and rebuilds the scaled copies; 0 and 100 need none.
    void prepare(int percent);
    // Embedded PCM, no file access or synthesis; playback must never delay saving.
    // A preview or a failure replaces a sound that is still playing; a save request does not, except the
    // failed-save sound, so an accepted save right after a refused one is still heard.
    void play(Kind kind, bool preview);

private:
    int percent_ = 100;
    Kind last_ = Kind::Saved; // The sound played last.
    std::array<std::vector<std::uint8_t>, 2> wav_; // Scaled sounds by Kind; empty plays the resource as is.
};

} // namespace refarter
