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

MicFilterStream::MicFilterStream(const MicFilters& filters) : filters_(filters) {
    if (filters_.noiseSuppression) denoiser_ = rnnoise_create(nullptr); // Null: no suppression, as before.
    if (denoiser_) {
        input_.reserve(rnnoiseFrame);
        output_.resize(rnnoiseFrame);
    }
}

MicFilterStream::~MicFilterStream() {
    if (denoiser_) rnnoise_destroy(denoiser_);
}

void MicFilterStream::denoise() {
    rnnoise_process_frame(denoiser_, output_.data(), input_.data());
    input_.clear();
    for (const float value : output_) {
        if (lag_ < rnnoiseDelay) { ++lag_; continue; } // Undo the lag: keeps lip sync.
        if (emitted_ == pushed_) break;                 // Padding after the last frame.
        const auto denoised = sample(value);
        ready_.push_back(denoised);
        ready_.push_back(denoised);
        ++emitted_;
    }
}

void MicFilterStream::push(const std::int16_t* stereo, std::size_t frames) {
    pushed_ += frames;
    if (!denoiser_) {
        ready_.insert(ready_.end(), stereo, stereo + frames * 2);
        return;
    }
    // Microphones are mono; the two channels carry the same signal.
    for (std::size_t i = 0; i < frames; ++i) {
        input_.push_back((stereo[2 * i] + stereo[2 * i + 1]) * 0.5f);
        if (input_.size() == rnnoiseFrame) denoise();
    }
}

void MicFilterStream::finish() {
    if (!denoiser_) return;
    // Silence after the end, a whole frame at a time, until every frame pushed has come out.
    while (emitted_ < pushed_) {
        input_.resize(rnnoiseFrame, 0.f);
        denoise();
    }
}

void MicFilterStream::take(std::int16_t* stereo, std::size_t frames) {
    frames = std::min(frames, ready());
    const std::int16_t* from = ready_.data() + head_;
    if (!filters_.noiseGate) {
        std::copy_n(from, frames * 2, stereo);
    } else {
        const float open = amplitude(filters_.gateOpenDb);
        const float close = amplitude(filters_.gateCloseDb);
        const float attackStep = 1.f / std::max(1, filters_.gateAttackMs * AudioTrack::rate / 1000);
        const float releaseStep = 1.f / std::max(1, filters_.gateReleaseMs * AudioTrack::rate / 1000);
        const int holdFrames = filters_.gateHoldMs * AudioTrack::rate / 1000;
        // Peak envelope with a 20 ms decay: follows speech, bridges zero crossings.
        const float decay = std::exp(-1.f / (0.020f * AudioTrack::rate));
        for (std::size_t i = 0; i < frames * 2; i += 2) {
            const float peak = static_cast<float>(std::max(std::abs(from[i]), std::abs(from[i + 1])));
            envelope_ = std::max(peak, envelope_ * decay);
            if (!open_ && envelope_ > open) open_ = true;
            else if (open_ && envelope_ < close) { open_ = false; held_ = 0; }
            if (open_) gain_ = std::min(1.f, gain_ + attackStep);
            else if (held_ < holdFrames) ++held_;
            else gain_ = std::max(0.f, gain_ - releaseStep);
            stereo[i] = sample(from[i] * gain_);
            stereo[i + 1] = sample(from[i + 1] * gain_);
        }
    }
    head_ += frames * 2;
    // Taken as it comes, so this stays about one piece long.
    if (head_ * 2 >= ready_.size()) {
        ready_.erase(ready_.begin(), ready_.begin() + static_cast<std::ptrdiff_t>(head_));
        head_ = 0;
    }
}

void applyMicFilters(std::vector<std::int16_t>& stereo, const MicFilters& f) {
    const std::size_t frames = stereo.size() / 2;
    if (!frames || (!f.noiseSuppression && !f.noiseGate)) return;
    MicFilterStream stream(f);
    stream.push(stereo.data(), frames);
    stream.finish();
    stream.take(stereo.data(), frames);
}

void applyNoiseSuppression(std::vector<std::int16_t>& stereo) {
    MicFilters f;
    f.noiseSuppression = true;
    f.noiseGate = false;
    applyMicFilters(stereo, f);
}

void applyNoiseGate(std::vector<std::int16_t>& stereo, const MicFilters& f) {
    auto gate = f;
    gate.noiseSuppression = false;
    gate.noiseGate = true;
    applyMicFilters(stereo, gate);
}

} // namespace refarter::capture
