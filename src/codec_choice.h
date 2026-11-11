#pragma once

#include "capture/codec_probe.h"
#include "capture/video_settings.h"

#include <algorithm>
#include <optional>

namespace refarter {

// The codec picker's rules, pure so they are unit-tested (codec_choice_test).

// What a codec costs at these settings, as "which codec should i pick?" shows it: the average and peak in whole Mb/s,
// and the video's megabytes a minute at the average, to the nearest 10 (calm scenes come out smaller).
struct CodecRate { int averageMbps = 0, peakMbps = 0, mbPerMinute = 0; };

inline CodecRate codecRate(capture::VideoSettings settings, capture::Codec codec, capture::Size output) {
    settings.codec = codec;
    const auto rate = capture::bitrateFor(settings, output);
    const auto mbps = [](int kbps) { return (kbps + 500) / 1000; };
    const int perMinute = (rate.averageKbps * 60 / 8 + 5000) / 10000 * 10; // kb/s → MB a minute, nearest 10.
    return {mbps(rate.averageKbps), mbps(rate.peakKbps), std::max(10, perMinute)};
}

// An empty encoder name does not establish unsupported hardware: initialisation can fail before enumeration,
// or an exception can discard the probe's local result after an encoder opened. Preserve the actual problem.
enum class CodecState { Records, Checking, NotChecked, CheckFailed, TestFailed };

inline CodecState codecState(capture::Codec codec, const capture::CodecSupport* support, bool probing) {
    if (codec == capture::Codec::H264 || (support && support->records)) return CodecState::Records; // h.264: every supported GPU.
    if (probing) return CodecState::Checking;
    if (!support) return CodecState::NotChecked;
    return support->encoder.empty() ? CodecState::CheckFailed : CodecState::TestFailed;
}

// Clips are shared as recorded (nothing converts them), mostly on Discord and YouTube, which both play AV1: AV1 once
// this GPU is known to record it, h.264 once it is known not to. Nothing while unknown, so the tag never moves. HEVC
// never: browsers and chat apps often refuse it.
inline std::optional<capture::Codec> recommendedCodec(const capture::CodecSupport* av1, bool probing) {
    if (probing || !av1) return std::nullopt;
    return av1->records ? capture::Codec::AV1 : capture::Codec::H264;
}

} // namespace refarter
