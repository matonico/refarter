#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct DenoiseState; // RNNoise's, declared in rnnoise.h.

namespace refarter::capture {

// Microphone clean-up, applied when a clip is saved (never while playing), to
// the raw 48 kHz stereo track: noise suppression first, then the gate.
// Both are off until the user turns them on; gate values default to OBS's
// own. Quiet headset microphones may need lower levels (e.g. -45/-50 dB).
struct MicFilters {
    bool noiseSuppression = false; // RNNoise: removes breathing, fans, keyboard hiss.
    bool noiseGate = false;        // Silences the mic between words.
    float gateOpenDb = -26.f;      // Opens above this level (dBFS)...
    float gateCloseDb = -32.f;     // ...and closes below this one.
    int gateAttackMs = 25;         // Fade-in when opening.
    int gateHoldMs = 200;          // Stays open this long after the level drops.
    int gateReleaseMs = 150;       // Fade-out when closing.
};

std::optional<std::wstring> validate(const MicFilters& filters);

void applyNoiseSuppression(std::vector<std::int16_t>& stereo);
void applyNoiseGate(std::vector<std::int16_t>& stereo, const MicFilters& filters);
void applyMicFilters(std::vector<std::int16_t>& stereo, const MicFilters& filters);

// The same filters a piece at a time, so a save never holds the whole microphone track: push() the recording in
// order, finish() after the last piece, and take() frames once ready() has them. The result is the same as
// applyMicFilters on the whole recording; RNNoise's lag means its frames come out about 20 ms after they go in.
class MicFilterStream {
public:
    explicit MicFilterStream(const MicFilters& filters);
    ~MicFilterStream();
    MicFilterStream(const MicFilterStream&) = delete;
    MicFilterStream& operator=(const MicFilterStream&) = delete;

    void push(const std::int16_t* stereo, std::size_t frames);
    void finish(); // No more input: the frames still inside the denoiser come out.
    std::size_t ready() const { return (ready_.size() - head_) / 2; }
    void take(std::int16_t* stereo, std::size_t frames); // At most ready() frames.

private:
    void denoise(); // Runs RNNoise on one whole frame of input_.

    MicFilters filters_;
    ::DenoiseState* denoiser_ = nullptr;
    std::vector<float> input_, output_;       // Mono: waiting for a whole RNNoise frame; one frame's result.
    std::size_t pushed_ = 0, emitted_ = 0, lag_ = 0; // Frames in; denoised frames out; leading ones dropped.
    std::vector<std::int16_t> ready_;         // Stereo frames from ready_[head_] on.
    std::size_t head_ = 0;
    float envelope_ = 0.f, gain_ = 0.f;       // The gate, carried from one piece to the next.
    bool open_ = false;
    int held_ = 0;
};

} // namespace refarter::capture
