// codec_choice.h and splitDetail: codec states, recommendations and muted menu detail.
#include "codec_choice.h"
#include "popup_menu.h"
#include <iostream>

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    using namespace refarter;
    using capture::Codec;

    // ---- Rates in the codec help ----
    capture::VideoSettings settings; // Balanced, 60 fps, h.264.
    const capture::Size p1080{1920, 1080};
    const auto rate = [&](Codec codec, int average, int peak, int perMinute) {
        const auto r = codecRate(settings, codec, p1080);
        return r.averageMbps == average && r.peakMbps == peak && r.mbPerMinute == perMinute;
    };
    check(rate(Codec::H264, 20, 40, 150), "balanced 1080p60 h.264: 19.9 average, 39.8 peak, about 150 mb a minute");
    check(rate(Codec::HEVC, 18, 36, 130), "hevc gets about 10% less");
    check(rate(Codec::AV1, 15, 30, 110), "av1 gets about 25% less");
    settings.codec = Codec::AV1;
    check(rate(Codec::H264, 20, 40, 150), "the codec asked for counts, not the saved one");
    settings.quality = capture::Quality::Custom;
    settings.bitrateMbps = 50;
    check(rate(Codec::AV1, 50, 50, 380), "custom is the same fixed rate for every codec");
    settings.quality = capture::Quality::Small;
    check(codecRate(settings, Codec::AV1, {64, 64}).averageMbps == capture::minBitrateMbps, "tiny windows get the minimum");
    check(codecRate(settings, Codec::AV1, {64, 64}).mbPerMinute == 20, "2 mb/s is 15 mb a minute, shown as 20");

    // ---- Codec rows ----
    capture::CodecSupport none{Codec::AV1, false, false, L"", L"no hardware encoder"};
    capture::CodecSupport failed{Codec::AV1, false, false, L"NVIDIA NVENC AV1", L"windows could not write it as mp4"};
    capture::CodecSupport works{Codec::AV1, true, false, L"NVIDIA NVENC AV1", L""};
    check(codecState(Codec::H264, nullptr, true) == CodecState::Records, "h.264 always records");
    check(codecState(Codec::AV1, nullptr, true) == CodecState::Checking, "unknown while the check runs");
    check(codecState(Codec::AV1, nullptr, false) == CodecState::NotChecked, "unknown without a check");
    check(codecState(Codec::AV1, &none, false) == CodecState::CheckFailed, "a missing encoder name is not proof of unsupported hardware");
    for (const auto* problem : {L"media foundation is unavailable", L"no usable gpu: device removed", L"the check ran out of memory"}) {
        capture::CodecSupport earlyFailure{Codec::AV1, false, false, {}, problem};
        check(codecState(Codec::AV1, &earlyFailure, false) == CodecState::CheckFailed,
              "initialisation and exceptional failures remain failed checks, not hardware capability verdicts");
    }
    check(codecState(Codec::AV1, &failed, false) == CodecState::TestFailed, "an encoder opened: the test failed");
    check(codecState(Codec::AV1, &works, false) == CodecState::Records, "a passed check records");

    // ---- Recommendation ----
    check(!recommendedCodec(nullptr, true), "nothing recommended while checking");
    check(!recommendedCodec(nullptr, false), "nothing recommended before a check");
    check(!recommendedCodec(&works, true), "nothing recommended while a new check runs");
    check(recommendedCodec(&works, false) == Codec::AV1, "av1 once it records");
    check(recommendedCodec(&failed, false) == Codec::H264, "h.264 once av1 is known not to record");

    // ---- Muted detail in menus ----
    check(splitDetail(L"av1\t(recommended)") == std::pair<std::wstring, std::wstring>{L"av1", L"(recommended)"}, "a tab splits");
    check(splitDetail(L"custom") == std::pair<std::wstring, std::wstring>{L"custom", L""}, "no tab, no detail");
    check(splitDetail(L"a\tb\tc") == std::pair<std::wstring, std::wstring>{L"a", L"b\tc"}, "only the first tab splits");

    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
