#include "mic_filters.h"
#include "audio_track.h"

#include <rnnoise.h>
#include <algorithm>
#include <cmath>

namespace refarter::capture {

namespace {
// RNNoise works on 10 ms mono frames at 48 kHz, the rate tracks are stored at.
// Its output lags its input by two frames (measured; see capture_logic_test).
constexpr std::size_t rnnoiseFrame = 480;
constexpr std::size_t rnnoiseDelay = 2 * rnnoiseFrame;
static_assert(AudioTrack::rate == 48'000 && AudioTrack::channels == 2);

float amplitude(float db) { return 32768.f * std::pow(10.f, db / 20.f); }
std::int16_t sample(float value) { return static_cast<std::int16_t>(std::clamp(std::lround(value), -32768L, 32767L)); }
} // namespace

std::optional<std::wstring> validate(const MicFilters& f) {
    if (f.gateOpenDb > 0 || f.gateCloseDb < -96 || f.gateCloseDb > f.gateOpenDb) {
        return L"noise gate: close level must be at or below open level, both between -96 and 0 dB";
    }
    if (f.gateAttackMs < 0 || f.gateHoldMs < 0 || f.gateReleaseMs < 0 ||
        f.gateAttackMs > 10'000 || f.gateHoldMs > 10'000 || f.gateReleaseMs > 10'000) {
        return L"noise gate: attack, hold and release must be 0-10000 ms";
    }
    return std::nullopt;
}

void applyNoiseSuppression(std::vector<std::int16_t>& stereo) {
    const std::size_t frames = stereo.size() / 2;
    if (!frames) return;
    // Microphones are mono; the two channels carry the same signal.
    const std::size_t padded = (frames + rnnoiseDelay + rnnoiseFrame - 1) / rnnoiseFrame * rnnoiseFrame;
    std::vector<float> in(padded, 0.f);
    std::vector<float> out(padded, 0.f);
    for (std::size_t i = 0; i < frames; ++i) in[i] = (stereo[2 * i] + stereo[2 * i + 1]) * 0.5f;
    DenoiseState* state = rnnoise_create(nullptr);
    if (!state) return;
    for (std::size_t at = 0; at < padded; at += rnnoiseFrame) rnnoise_process_frame(state, &out[at], &in[at]);
    rnnoise_destroy(state);
    for (std::size_t i = 0; i < frames; ++i) {
        stereo[2 * i] = stereo[2 * i + 1] = sample(out[i + rnnoiseDelay]); // Undo the lag: keeps lip sync.
    }
}

void applyNoiseGate(std::vector<std::int16_t>& stereo, const MicFilters& f) {
    const float open = amplitude(f.gateOpenDb);
    const float close = amplitude(f.gateCloseDb);
    const float attackStep = 1.f / std::max(1, f.gateAttackMs * AudioTrack::rate / 1000);
    const float releaseStep = 1.f / std::max(1, f.gateReleaseMs * AudioTrack::rate / 1000);
    const int holdFrames = f.gateHoldMs * AudioTrack::rate / 1000;
    // Peak envelope with a 20 ms decay: follows speech, bridges zero crossings.
    const float decay = std::exp(-1.f / (0.020f * AudioTrack::rate));
    float envelope = 0.f;
    float gain = 0.f;
    bool isOpen = false;
    int held = 0;
    for (std::size_t i = 0; i + 1 < stereo.size(); i += 2) {
        const float peak = static_cast<float>(std::max(std::abs(stereo[i]), std::abs(stereo[i + 1])));
        envelope = std::max(peak, envelope * decay);
        if (!isOpen && envelope > open) isOpen = true;
        else if (isOpen && envelope < close) { isOpen = false; held = 0; }
        if (isOpen) gain = std::min(1.f, gain + attackStep);
        else if (held < holdFrames) ++held;
        else gain = std::max(0.f, gain - releaseStep);
        stereo[i] = sample(stereo[i] * gain);
        stereo[i + 1] = sample(stereo[i + 1] * gain);
    }
}

void applyMicFilters(std::vector<std::int16_t>& stereo, const MicFilters& f) {
    if (f.noiseSuppression) applyNoiseSuppression(stereo);
    if (f.noiseGate) applyNoiseGate(stereo, f);
}

} // namespace refarter::capture
