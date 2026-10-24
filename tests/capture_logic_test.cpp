#include "capture/audio_device.h"
#include "capture/audio_track.h"
#include "capture/audio_levels.h"
#include "capture/clip_folder.h"
#include "capture/discord_audio.h"
#include "capture/game_process.h"
#include "capture/frame_pacer.h"
#include "capture/frame_rate_budget.h"
#include "fixtures/jittered_capture_times.h"
#include "capture/clip_timing.h"
#include "capture/mic_filters.h"
#include "capture/annexb.h"
#include "capture/replay_buffer.h"
#include "capture/video_settings.h"
#include "capture/startup_frame.h"
#include "capture/screenshot.h"
#include <wincodec.h>
#include <winrt/base.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <thread>

using namespace refarter::capture;

namespace {
int failures = 0;
void check(bool passes, const char* name) {
    if (!passes) { std::cerr << name << '\n'; ++failures; }
}

PacketPtr packet(std::int64_t time, bool keyframe, std::size_t bytes = 10) {
    auto p = std::make_shared<EncodedPacket>();
    p->time = time;
    p->duration = 10;
    p->keyframe = keyframe;
    p->data.resize(bytes);
    return p;
}
} // namespace

int main() {
    check(!usableStartupFrame(true, {0, 0}, {160, 28}, {160, 28}), "minimised placeholder cannot configure an encoder");
    check(!usableStartupFrame(false, {2560, 1440}, {2560, 1440}, {160, 28}), "restore waits for full capture texture");
    check(!usableStartupFrame(false, {2560, 1440}, {160, 28}, {160, 28}), "restored client rejects stale minimised content");
    check(!usableStartupFrame(false, {0, 0}, {2560, 1440}, {2560, 1440}), "empty client is not ready");
    check(usableStartupFrame(false, {2560, 1440}, {2560, 1440}, {2560, 1440}), "restored full-sized frame is ready");
    // Device menu labels: two identical headsets stay distinguishable, unique names stay plain.
    const auto labels = endpointLabels({{L"a", L"Headset"}, {L"b", L"Speakers"}, {L"c", L"Headset"}, {L"d", L""}});
    check(labels == std::vector<std::wstring>{L"Headset · 1", L"Speakers", L"Headset · 2", L"unnamed device"},
          "duplicate endpoint names are numbered in list order");

    // Gain keeps identity/mute exact, preserves sample count and saturates rather
    // than wrapping when boosting loud signed PCM. Mic filtering precedes gain.
    const std::vector<std::int16_t> gainSamples{-32768, -20000, -1000, 0, 1000, 20000, 32767};
    auto audio = gainSamples;
    applyAudioLevel(audio, 100);
    check(audio == gainSamples, "100% audio stays bit-exact");
    applyAudioLevel(audio, 50);
    check(audio == std::vector<std::int16_t>{-16384, -10000, -500, 0, 500, 10000, 16383}, "50% signed PCM");
    audio = gainSamples;
    applyAudioLevel(audio, 200);
    check(audio == std::vector<std::int16_t>{-32768, -32768, -2000, 0, 2000, 32767, 32767}, "boost saturates both limits");
    applyAudioLevel(audio, 0);
    check(audio == std::vector<std::int16_t>(gainSamples.size(), 0), "mute preserves sample count");
    check(!validate(AudioLevels{}), "default levels valid");
    check(!validate(AudioLevels{0, 200}), "level boundaries valid");
    check(validate(AudioLevels{-1, 100}).has_value(), "negative desktop level rejected");
    check(validate(AudioLevels{100, 201}).has_value(), "excess mic level rejected");

    // Level meters: dBFS after the volume setting, peak hold then steady fall, bar pixels.
    const auto closeTo = [](float a, float b) { return std::abs(a - b) < 0.01f; };
    check(peakToDb(0) == meterFloorDb && peakToDb(0, 200) == meterFloorDb, "silence reads the meter floor");
    check(closeTo(peakToDb(16384), -6.02f), "half scale is -6 dBFS");
    check(closeTo(peakToDb(32768), 0.f), "full scale is 0 dBFS");
    check(closeTo(peakToDb(16384, 50), -12.04f) && closeTo(peakToDb(8192, 200), -6.02f) && closeTo(peakToDb(16384, 200), 0.f),
          "volume scales the meter as it scales the clip");
    check(peakToDb(20000, 0) == meterFloorDb, "muted source reads the floor");
    check(peakToDb(30000, 200) <= 0.f, "boost cannot read above full scale");
    check(peakToDb(1, 100) == meterFloorDb, "below the floor clamps");
    LevelMeter meter;
    const std::int16_t block[] = {100, -3000, 250, 0};
    meter.push(block, 4);
    meter.push(block, 2);
    check(meter.take() == 3000 && meter.take() == 0, "meter keeps the peak since the last look");
    const std::int16_t extreme[] = {-32768};
    meter.push(extreme, 1);
    check(meter.take() == 32768, "most negative sample counts at full scale");
    check(!meter.wanted(), "a meter is not wanted until the interface says so");
    meter.want(true);
    check(meter.wanted(), "the interface can switch a meter on");
    meter.want(false);
    check(!meter.wanted(), "and off again");
    MeterBar bar;
    bar.update(-10.f, 0.05f);
    check(closeTo(bar.db, -10.f), "bar rises at once");
    bar.update(-40.f, 0.05f);
    bar.update(-40.f, 0.05f);
    check(closeTo(bar.db, -10.f), "bar holds a new peak briefly");
    for (int i = 0; i < 4; ++i) bar.update(-40.f, 0.05f);
    check(bar.db < -10.f && bar.db > -40.f, "bar then falls steadily");
    for (int i = 0; i < 200; ++i) bar.update(-40.f, 0.05f);
    check(closeTo(bar.db, -40.f), "bar settles on the measured level");
    bar.update(meterFloorDb, 5.f);
    check(closeTo(bar.db, meterFloorDb) && bar.fraction() == 0.f, "bar empties on silence");
    bar.update(0.f, 0.05f);
    check(bar.fraction() == 1.f && bar.clipping(), "full scale fills the bar and warns");
    bar.reset();
    bar.update(meterClipDb - 0.1f, 0.05f);
    check(!bar.clipping(), "just below the warning level is normal");
    bar.update(meterClipDb, 0.05f);
    check(bar.clipping(), "-3 dBFS warns");
    check(meterLength(0.5f, 101) == 51 && meterLength(0.f, 80) == 0 && meterLength(1.f, 80) == 80 && meterLength(1.5f, 80) == 80,
          "meter pixels round and clamp");
    const auto adjusted = prepareAudioTracks({2000, -2000}, {3000, -3000}, {50, 200});
    check(adjusted.tracks == std::vector<std::vector<std::int16_t>>{{7000, -7000}, {1000, -1000}, {6000, -6000}},
          "mix and separate tracks share adjusted levels");
    const auto muted = prepareAudioTracks({2000, -2000}, {3000, -3000}, {0, 100});
    check(muted.tracks == std::vector<std::vector<std::int16_t>>{{3000, -3000}, {0, 0}, {3000, -3000}},
          "muted source remains silent alternate, mix keeps other source");
    check(prepareAudioTracks({25000, -25000}, {25000, -25000}, {}).tracks.front() ==
          std::vector<std::int16_t>{32767, -32768}, "combined mix saturates");
    check(prepareAudioTracks({}, {2000, -2000}, {100, 50}).tracks ==
          std::vector<std::vector<std::int16_t>>{{1000, -1000}}, "single mic track gain");
    check(prepareAudioTracks({2000, -2000}, {}, {50, 100}).tracks ==
          std::vector<std::vector<std::int16_t>>{{1000, -1000}}, "single desktop track gain");
    check(prepareAudioTracks({}, {}, {}).tracks.empty(), "disabled audio adds no tracks");
    check(adjusted.names == std::vector<std::string>{"mix", "desktop", "microphone"}, "tracks are named in order");
    check(prepareAudioTracks({}, {2000, -2000}, {}).names == std::vector<std::string>{"microphone"}, "single track is named");
    const auto discord = prepareAudioTracks({2000, -2000}, {3000, -3000}, {50, 100}, {4000, -4000});
    check(discord.tracks == std::vector<std::vector<std::int16_t>>{{6000, -6000}, {1000, -1000}, {3000, -3000}, {2000, -2000}},
          "discord joins the mix and keeps the desktop volume");
    check(discord.names == std::vector<std::string>{"mix", "desktop", "microphone", "discord"}, "discord track comes last");
    const auto withDiscord = prepareAudioTracks({2000, -2000}, {}, {}, {1000, 3000});
    check(withDiscord.tracks == std::vector<std::vector<std::int16_t>>{{3000, 1000}, {2000, -2000}, {1000, 3000}} &&
          withDiscord.names == std::vector<std::string>{"mix", "desktop", "discord"}, "desktop and discord without microphone");
    check(prepareAudioTracks({1, 2, 3, 4}, {}, {}, {5, 6}).tracks.front() == std::vector<std::int16_t>{6, 8, 3, 4},
          "mix spans the longest source");
    // A game window's own sound is a last track of its own: the mix does not change and never sums it.
    const auto game = prepareAudioTracks({2000, -2000}, {3000, -3000}, {50, 100}, {}, {1000, -1000});
    check(game.tracks == std::vector<std::vector<std::int16_t>>{{4000, -4000}, {1000, -1000}, {3000, -3000}, {500, -500}} &&
          game.names == std::vector<std::string>{"mix", "desktop", "microphone", "game"}, "game track follows the sources, outside the mix");
    const auto gameAlone = prepareAudioTracks({2000, -2000}, {}, {}, {}, {1000, -1000});
    check(gameAlone.names == std::vector<std::string>{"desktop", "game"} && gameAlone.tracks.size() == 2,
          "desktop and game without a mix: the desktop track stays first");
    const auto everything = prepareAudioTracks({2000, -2000}, {3000, -3000}, {}, {4000, -4000}, {1000, -1000});
    check(everything.names == std::vector<std::string>{"mix", "desktop", "microphone", "discord", "game"} &&
          everything.tracks.front() == std::vector<std::int16_t>{9000, -9000}, "all five tracks in order, the mix without the game");
    check(prepareAudioTracks({2000, -2000}, {}, {}, {}, {}).names == std::vector<std::string>{"desktop"}, "no game sound, no game track");
    // With a fixed desktop output the game may play elsewhere: an independent source, summed into the mix.
    const auto apart = prepareAudioTracks({2000, -2000}, {}, {}, {}, {1000, -1000}, false);
    check(apart.names == std::vector<std::string>{"mix", "desktop", "game output"} &&
          apart.tracks.front() == std::vector<std::int16_t>{3000, -3000}, "a game apart from desktop is mixed and named game output");
    // A fixed desktop output: the game is contained unless it verifiably plays elsewhere; unknown never doubles it.
    check(gameInDesktopAudio(true, GameOutput::Elsewhere), "system default desktop always holds the game");
    check(gameInDesktopAudio(false, GameOutput::Here) && gameInDesktopAudio(false, GameOutput::Unknown), "chosen output with the game, or unknown: contained");
    check(!gameInDesktopAudio(false, GameOutput::Elsewhere), "chosen output the game does not play on: independent source");
    // Routing at save time: a live answer first, else what was seen while recording, else unknown (contained).
    check(knownGameOutput(GameOutput::Here, GameOutput::Elsewhere) == GameOutput::Here &&
          knownGameOutput(GameOutput::Elsewhere, GameOutput::Here) == GameOutput::Elsewhere, "a live known answer wins over the last one seen");
    check(!gameInDesktopAudio(false, knownGameOutput(GameOutput::Unknown, GameOutput::Elsewhere)),
          "game gone (live unknown) but last seen elsewhere: not contained, so the game is mixed in");
    check(gameInDesktopAudio(false, knownGameOutput(GameOutput::Unknown, GameOutput::Here)), "last seen here: contained");
    check(gameInDesktopAudio(false, knownGameOutput(GameOutput::Unknown, GameOutput::Unknown)), "nothing known: contained");
    const std::vector<ProcessEntry> tree{{4, 1, L"explorer.exe"}, {30, 4, L"game.exe"}, {31, 30, L"helper.exe"}, {32, 31, L"deep.exe"}, {40, 4, L"other.exe"}};
    check(inProcessTree(tree, 30, 30) && inProcessTree(tree, 30, 32) && !inProcessTree(tree, 30, 40) && !inProcessTree(tree, 30, 4), "game process tree membership");
    check(!inProcessTree(tree, 0, 30) && !inProcessTree(tree, 30, 0) && !inProcessTree(tree, 30, 99), "unknown processes are not in the tree");
    // Discord's helpers are children of its main process, which owns the voice tree.
    check(isDiscordExecutable(L"Discord.exe") && isDiscordExecutable(L"DISCORDCANARY.EXE") &&
          isDiscordExecutable(L"discordptb.exe") && isDiscordExecutable(L"Vesktop.exe"), "discord clients recognised");
    check(!isDiscordExecutable(L"discord") && !isDiscordExecutable(L"notdiscord.exe") && !isDiscordExecutable(L""),
          "other executables are not discord");
    check(discordRoot({{4, 1, L"explorer.exe"}, {20, 4, L"Discord.exe"}, {21, 20, L"Discord.exe"},
                       {22, 20, L"Discord.exe"}, {30, 4, L"game.exe"}}) == 20, "topmost discord process found");
    check(discordRoot({{21, 20, L"Discord.exe"}, {22, 21, L"Discord.exe"}}) == 21, "orphaned discord child becomes the root");
    check(discordRoot({{4, 1, L"explorer.exe"}, {30, 4, L"game.exe"}}) == 0, "no discord running");
    // Settings and output size.
    check(!validate(VideoSettings{}), "defaults valid");
    check(validate({Resolution::P1080, 9, 50, 180}).has_value(), "fps too low");
    check(validate({Resolution::P1080, 61, 50, 180}).has_value(), "fps above 60");
    check(validate({Resolution::P1080, 60, 251, 180}).has_value(), "bitrate too high");
    check(validate({Resolution::P1080, 60, 50, 4}).has_value(), "replay too short");
    check(outputSize(Resolution::P1080, {2560, 1440}) == Size{1920, 1080}, "downscale 1440p to 1080p");
    check(outputSize(Resolution::P1440, {1920, 1080}) == Size{1920, 1080}, "never upscale");
    check(outputSize(Resolution::Native, {1281, 721}) == Size{1280, 720}, "native rounds to even");
    check(outputSize(Resolution::P1080, {3440, 1440}) == Size{2580, 1080}, "ultrawide keeps aspect");
    check(outputSize(Resolution::Native, {5120, 1440}) == Size{4096, 1152}, "clamped to encoder limit");
    check(outputSize(Resolution::Native, {0, 0}) == Size{}, "empty window");
    check(replayBytes(40'000, 180) == 900'000'000, "replay memory estimate");

    // Bitrates: presets scale with pixels, frame rate and codec; custom is constant.
    VideoSettings balanced;
    check((bitrateFor(balanced, {1920, 1080}) == Bitrate{19'906, 39'812, false}), "balanced 1080p60 h.264");
    balanced.codec = Codec::HEVC;
    check(bitrateFor(balanced, {1920, 1080}).averageKbps == 12'939, "hevc needs less");
    balanced.codec = Codec::H264;
    balanced.fps = 30;
    check(bitrateFor(balanced, {1920, 1080}).averageKbps == 11'836, "30 fps needs more than half");
    check(bitrateFor(balanced, {64, 64}).averageKbps == minBitrateMbps * 1000, "tiny windows clamp to minimum");
    VideoSettings custom;
    custom.quality = Quality::Custom;
    custom.bitrateMbps = 40;
    check((bitrateFor(custom, {1920, 1080}) == Bitrate{40'000, 40'000, true}), "custom is constant");

    // H.264 Annex B parsing.
    const std::vector<std::uint8_t> idr{0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x68, 3, 0, 0, 1, 0x65, 9, 9};
    const std::vector<std::uint8_t> nonIdr{0, 0, 0, 1, 0x41, 9, 9};
    check(annexb::containsKeyframe(Codec::H264, idr), "idr found");
    check(!annexb::containsKeyframe(Codec::H264, nonIdr), "p-frame not idr");
    check((annexb::parameterSets(Codec::H264, idr) == std::vector<std::uint8_t>{0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x68, 3}), "sps+pps extracted");
    check(annexb::parameterSets(Codec::H264, nonIdr).empty(), "no parameter sets");

    // HEVC: two-byte NAL headers, type in bits 1-6; VPS/SPS/PPS 32-34, IDR_W_RADL 19.
    const std::vector<std::uint8_t> hevcIdr{0, 0, 0, 1, 0x40, 1, 7, 0, 0, 1, 0x42, 1, 8, 0, 0, 1, 0x44, 1, 9, 0, 0, 1, 0x26, 1, 5};
    const std::vector<std::uint8_t> hevcTrail{0, 0, 0, 1, 0x02, 1, 5};
    check(annexb::containsKeyframe(Codec::HEVC, hevcIdr), "hevc idr found");
    check(!annexb::containsKeyframe(Codec::HEVC, hevcTrail), "hevc trailing picture not a keyframe");
    check(!annexb::containsKeyframe(Codec::H264, hevcTrail), "codecs parse differently");
    check((annexb::parameterSets(Codec::HEVC, hevcIdr) == std::vector<std::uint8_t>{0, 0, 0, 1, 0x40, 1, 7, 0, 0, 1, 0x42, 1, 8, 0, 0, 1, 0x44, 1, 9}),
          "vps+sps+pps extracted");
    check(annexb::parameterSets(Codec::AV1, hevcIdr).empty(), "av1 has no annex b parameter sets");

    // Replay buffer: keyframe start, whole-GOP eviction, snapshots.
    ReplayBuffer buffer(100, 1'000'000);
    buffer.push(packet(0, false));
    check(buffer.span() == 0, "leading non-keyframe discarded");
    for (int i = 0; i < 30; ++i) buffer.push(packet(i * 10, i % 5 == 0)); // GOP = 50 units.
    // End = 300. Retain 100: the newest GOP still covering 100 units starts at 200.
    const auto all = buffer.snapshot(1'000);
    check(!all.empty() && all.front()->keyframe, "buffer starts on keyframe");
    check(!all.empty() && all.front()->time == 200, "evicts whole GOPs only");
    check(buffer.span() >= 100, "retains the requested span");
    const auto recent = buffer.snapshot(60);
    check(!recent.empty() && recent.front()->time == 200 && recent.front()->keyframe, "snapshot starts at covering keyframe");
    check(recent.size() == 10, "snapshot runs to newest");
    // Static screen: newest packet at 290 but saved at 400, so look back from 400.
    const auto idle = buffer.snapshot(60, 400);
    check(!idle.empty() && idle.front()->time == 250 && idle.size() == 5, "snapshot measured from save time");
    // Saved at 265 but reached later: pictures from 270 on came after the request.
    const auto requested = buffer.snapshot(60, 265);
    check(requested.size() == 7 && requested.front()->time == 200 && requested.back()->time == 260,
          "pictures after the save request left out");
    check(buffer.snapshot(60, 100).empty(), "nothing recorded before the request");
    check(buffer.snapshot(60, 0).size() == buffer.snapshot(60).size() && buffer.snapshot(60, 0).back()->time == 290,
          "an explicit end of 0 still means the newest picture");
    check(buffer.evicted() && !ReplayBuffer(100, 1'000'000).evicted(), "eviction is remembered");
    {
        // A save that starts late: replay 100 units, headroom 50, pictures every 10 and a keyframe every 50.
        // The press was 30 units before the newest picture; the whole replay before it must still be held.
        ReplayBuffer headroom(150, 1'000'000);
        ReplayBuffer tight(100, 1'000'000);
        for (int i = 0; i < 50; ++i) {
            headroom.push(packet(i * 10, i % 5 == 0));
            tight.push(packet(i * 10, i % 5 == 0));
        }
        const auto late = headroom.snapshot(100, 470);
        check(!late.empty() && late.front()->time <= 380 && late.back()->time + late.back()->duration - 380 >= 100 &&
                  late.back()->time <= 470, "a late save still spans the full replay");
        const auto cut = tight.snapshot(100, 470);
        check(!cut.empty() && cut.front()->time > 380, "without headroom the start of a late save is gone");
        check(headroom.snapshot(100, 100).empty() && headroom.evicted(),
              "a press older than the buffer finds nothing, and the buffer says why");
        ReplayBuffer fresh(150, 1'000'000);
        fresh.push(packet(200, true));
        check(fresh.snapshot(100, 100).empty() && !fresh.evicted(), "a press before the first picture is not an eviction");
        headroom.clear();
        check(!headroom.evicted(), "clearing forgets the eviction");
    }
    check(clipEnd(0, 500, 0) == 500, "unset request ends now");
    check(clipEnd(300, 500, 0) == 300, "clip ends at the request, not when the save starts");
    check(clipEnd(300, 500, 250) == 250 && clipEnd(0, 500, 250) == 250, "stopped replay ends at the stop");
    check(clipEnd(300, 500, 400) == 300, "request before a later stop ends at the request");
    // Sparse source: one keyframe picture every 40 units. The picture on screen
    // at the requested start begins the clip there, not 30 units earlier.
    ReplayBuffer sparse(1'000, 1'000'000);
    for (int i = 0; i < 6; ++i) sparse.push(packet(i * 40, true));
    const auto held = sparse.snapshot(60, 230); // from = 170, inside the picture shown since 160.
    check(held.size() == 2 && held.front()->time == 170 && held.front()->keyframe && held[1]->time == 200,
          "lone leading keyframe starts at the requested time");
    check(sparse.snapshot(1'000, 230).front()->time == 0, "short buffer keeps its first picture time");
    check(sparse.snapshot(70, 230).front()->time == 160, "keyframe exactly at the start is unchanged");
    check(buffer.snapshot(60).front()->time == 200, "dense GOP before the start is kept for decoding");
    ReplayBuffer capped(1'000'000, 100);
    for (int i = 0; i < 20; ++i) capped.push(packet(i * 10, i % 5 == 0, 10));
    check(capped.bytes() <= 100, "byte cap evicts GOPs");

    // Actual irregular near-60 Hz delivery: legacy slots reject candidates
    // despite the source fitting the sustained rate. Budget must retain all.
    {
        FrameRateBudget budget(60);
        FramePacer legacy(60);
        int admitted = 0, legacyAdmitted = 0, input = 0;
        for (auto time : test::jitteredCaptureTimes) {
            ++input;
            if (budget.allows(time)) { budget.commit(); ++admitted; }
            if (auto slot = legacy.slotFor(time)) { legacy.commit(*slot); ++legacyAdmitted; }
        }
        check(admitted == input && legacyAdmitted < input, "recorded near-rate jitter retains every delivered picture");
    }
    for (int fps : {30, 60}) {
        for (int hz : {20, 30, 45, 56, 60, 75, 120, 144, 165, 240, 280, 360, 1000}) {
            FrameRateBudget budget(fps);
            std::vector<std::int64_t> accepted;
            std::size_t windowStart = 0;
            bool bounded = true;
            for (int i = 0; i < hz * 30; ++i) {
                const auto time = i * 10'000'000LL / hz;
                if (!budget.allows(time)) continue;
                budget.commit();
                accepted.push_back(time);
                // Every prefix and rolling second must obey the capped budget.
                bounded &= static_cast<std::int64_t>(accepted.size()) <= time * fps / 10'000'000 + FrameRateBudget::burstFrames;
                while (accepted[windowStart] <= time - 10'000'000) ++windowStart;
                bounded &= accepted.size() - windowStart <= static_cast<std::size_t>(fps + FrameRateBudget::burstFrames);
            }
            check(bounded, "high-rate work obeys prefix and rolling-second budgets");
            if (hz <= fps) check(accepted.size() == static_cast<std::size_t>(hz * 30), "slow source retains every picture");
            else check(accepted.size() >= static_cast<std::size_t>(fps * 30 - 1), "fast source fills sustained recording budget");
        }
        for (double hz : {20.0, 29.97, 30.0, 45.0, 56.0, 59.94, 60.0}) {
            if (hz > fps) continue;
            FrameRateBudget budget(fps);
            std::uint32_t random = 1;
            int admitted = 0;
            const int count = static_cast<int>(hz * 30);
            for (int i = 0; i < count; ++i) {
                random = random * 1664525 + 1013904223;
                const auto jitter = static_cast<std::int64_t>(random % 140001) - 70000; // ±7 ms
                const auto time = 10'000'000 + static_cast<std::int64_t>(i * 10'000'000.0 / hz) + jitter;
                if (budget.allows(time)) { budget.commit(); ++admitted; }
            }
            check(admitted == count, "near-rate source with large jitter retains all pictures");
        }
    }
    {
        FrameRateBudget budget(60);
        check(budget.allows(0), "first frame is available"); // Simulate conversion failure: no commit.
        for (std::int64_t time : {1, 2, 3}) {
            const bool allowed = budget.allows(time);
            check(allowed, "failed acquisition/conversion does not spend a credit");
            if (allowed) { budget.commit(); budget.commit(); } // Second commit must not spend twice.
        }
        check(!budget.allows(4), "burst ends after three successful submissions");
        check(!budget.allows(4) && !budget.allows(2) && !budget.allows(-1), "duplicate/backwards timestamps never refill");
        const std::int64_t resumed = 10'000'000;
        for (std::int64_t time : {resumed, resumed + 1, resumed + 2}) {
            const bool allowed = budget.allows(time);
            check(allowed, "pause refills only the bounded allowance");
            if (allowed) budget.commit();
        }
        check(!budget.allows(resumed + 3), "pause cannot accumulate unbounded catch-up work");
        check(budget.allows(std::numeric_limits<std::int64_t>::max()), "huge capture gap saturates without overflow");
    }

    // Legacy comparison pacing: simulate a refresh-rate source, jittered by
    // ±0.4 ms and throttled the way the recorder configures it. Count slots
    // left empty although the source kept presenting.
    const auto gaps = [](int fps, double hz, int seconds) {
        FramePacer pacer(fps);
        const std::int64_t minInterval = 10'000'000LL * 45 / 100 / fps;
        std::int64_t delivered = -minInterval;
        std::int64_t last = -1;
        int empty = 0;
        std::uint32_t random = 1;
        for (int i = 0; i < static_cast<int>(hz * seconds); ++i) {
            random = random * 1664525 + 1013904223;
            const auto t = static_cast<std::int64_t>(i * 10'000'000.0 / hz) + static_cast<std::int64_t>(random >> 22) - 512 * 8;
            if (t - delivered < minInterval) continue;
            delivered = t;
            if (const auto slot = pacer.slotFor(t)) {
                if (last >= 0) empty += static_cast<int>(*slot - last - 1);
                pacer.commit(*slot);
                last = *slot;
            }
        }
        return empty;
    };
    for (const double hz : {60.0, 75.0, 120.0, 143.86, 144.0, 165.0, 239.76, 240.0}) {
        check(gaps(60, hz, 30) == 0, "60 fps from a common refresh rate");
        check(gaps(30, hz, 30) == 0, "30 fps from a common refresh rate");
    }
    // A source slightly slower than the recording rate cannot fill every slot;
    // allow only its unavoidable deficit (0.06 frames a second).
    check(gaps(60, 59.94, 30) <= 2, "60 fps from 59.94 Hz: no gaps beyond the deficit");
    FramePacer stalled(60);
    stalled.commit(*stalled.slotFor(0));
    check(stalled.slotFor(10'000'000) == 60, "a real stall leaves a gap");

    // 56 fps source: quantising to 60 creates longer holds despite no source stall.
    // The experiment preserves every selected picture's original time, including
    // real stalls, while the same pacer still limits high-rate sources.
    FramePacer originalTime(60);
    std::int64_t priorGrid = -1, priorCapture = -1;
    int gridLongIntervals = 0, captureLongIntervals = 0, selected = 0;
    constexpr std::int64_t origin = 49'021'191'055;
    for (int i = 0; i < 560; ++i) {
        const auto raw = origin + i * 10'000'000LL / 56;
        const auto slot = originalTime.slotFor(raw);
        if (!slot) continue;
        const auto grid = originalTime.sampleTime(*slot, raw, false);
        const auto actual = originalTime.sampleTime(*slot, raw, true);
        check(actual == raw - origin, "capture timestamp retains the shared clock origin exactly");
        if (priorGrid >= 0) {
            gridLongIntervals += grid - priorGrid > 250'000;
            captureLongIntervals += actual - priorCapture > 250'000;
        }
        priorGrid = grid;
        priorCapture = actual;
        originalTime.commit(*slot);
        ++selected;
    }
    check(selected == 560 && gridLongIntervals > 0 && captureLongIntervals == 0,
          "timestamp preservation removes manufactured long holds without dropping pictures");
    FramePacer fast(60);
    int fastSelected = 0;
    std::int64_t fastLast = -1;
    for (int i = 0; i < 2800; ++i) {
        const auto raw = origin + i * 10'000'000LL / 280;
        if (auto slot = fast.slotFor(raw)) {
            const auto actual = fast.sampleTime(*slot, raw, true);
            check(actual > fastLast, "high-rate accepted capture times strictly increase");
            fastLast = actual;
            fast.commit(*slot);
            ++fastSelected;
        }
    }
    check(fastSelected >= 599 && fastSelected <= 601, "timestamp experiment keeps 60 fps selection cap");
    FramePacer pause(60);
    pause.commit(*pause.slotFor(origin));
    const auto resumed = pause.slotFor(origin + 20'000'000);
    check(resumed && pause.sampleTime(*resumed, origin + 20'000'000, true) == 20'000'000,
          "actual source pauses retain their full duration");
    EncodedPacket earlierPacket, next;
    earlierPacket.time = 100'000; earlierPacket.duration = 166'666;
    next.time = 242'857;
    check(clipPacketDuration(earlierPacket, &next, 1'000'000, true) == 142'857,
          "short VFR duration ends exactly at next picture without overlap");
    next.time = 314'321;
    check(clipPacketDuration(earlierPacket, &next, 1'000'000, true) == 214'321,
          "long VFR duration ends exactly at next picture without gap");
    check(clipPacketDuration(earlierPacket, nullptr, 20'100'000, true) == 20'000'000,
          "last sparse picture holds to save clock, matching audio span");
    check(clipPacketDuration(earlierPacket, nullptr, 99'999, true) == earlierPacket.duration,
          "future WGC timestamp at save retains a positive final duration");
    check(clipPacketDuration(earlierPacket, &next, 1'000'000, false) == earlierPacket.duration,
          "normal mode retains original packet duration");
    {
        const std::vector<PacketPtr> ordered{packet(0, true), packet(10, false), packet(20, false)};
        const auto same = increasingTimes(ordered);
        check(same[0] == ordered[0] && same[1] == ordered[1] && same[2] == ordered[2],
              "increasing timestamps are kept without copies");
        const std::vector<PacketPtr> repeated{packet(0, true), packet(10, false), packet(10, false),
                                              packet(0, false), packet(30, false)};
        const auto fixed = increasingTimes(repeated);
        check(fixed[2]->time == 11 && fixed[3]->time == 12 && fixed[4] == repeated[4],
              "repeated or earlier timestamps follow the picture before");
        check(repeated[2]->time == 10 && fixed[2]->data == repeated[2]->data, "buffered packets stay unchanged");
        check(increasingTimes({}).empty(), "empty packet list stays empty");
    }

    // Audio timeline. 10 ms = 480 frames = 100'000 clock units.
    const auto tone = [](std::size_t frames, std::int16_t value) { return std::vector<std::int16_t>(frames * 2, value); };
    {
        AudioTrack track(10);
        const auto a = tone(480, 1), b = tone(480, 2);
        track.write(1'000'000, a.data(), 480);
        track.write(1'100'000, b.data(), 480);
        const auto out = track.read(1'000'000, 1'200'000);
        check(out.size() == 960 * 2 && out[0] == 1 && out[479 * 2] == 1 && out[480 * 2] == 2 && out.back() == 2,
              "consecutive packets are contiguous");
        const auto early = track.read(900'000, 1'000'000);
        check(early.size() == 480 * 2 && early[0] == 0 && early.back() == 0, "before capture started is silent");
        const auto late = track.read(1'200'000, 1'300'000);
        check(late.size() == 480 * 2 && late[0] == 0, "not yet captured is silent");
    }
    {
        AudioTrack track(10);
        const auto a = tone(480, 5);
        track.write(0, a.data(), 480);
        track.write(1'000'000, a.data(), 480); // Loopback quiet for 90 ms.
        check(track.framesWritten() == 4800 + 480, "gap filled with silence");
        const auto out = track.read(500'000, 1'100'000);
        check(out[0] == 0 && out.back() == 5, "gap reads silent, later audio in place");
    }
    {
        AudioTrack track(10);
        const auto a = tone(3000, 7);
        track.write(0, a.data(), 3000);
        track.write(100'000, a.data(), 3000); // Device clock ran 52 ms ahead of QPC.
        check(track.framesWritten() == 3480, "overlap beyond tolerance is dropped");
        track.write(726'000, nullptr, 480); // Within 20 ms of the expected time: no correction.
        check(track.framesWritten() == 3960, "small jitter is not corrected");
    }
    {
        AudioTrack track(1); // 48'000 frames.
        const auto a = tone(60'000, 3);
        track.write(0, a.data(), 60'000);
        const auto out = track.read(0, 10'000'000 * 60'000LL / 48'000);
        check(out[0] == 0 && out[11'999 * 2] == 0 && out[12'000 * 2] == 3 && out.back() == 3, "ring keeps only the newest second");
    }
    {
        // Chunks of the ring exist only while they hold sound: reads must match a plain ring across chunk edges.
        AudioTrack track(4);
        const auto a = tone(70'000, 9); // Crosses the one-second chunk edge at 48'000.
        track.write(0, a.data(), 70'000);
        track.write(10'000'000 * 70'000LL / 48'000, nullptr, 50'000); // Silence over a whole chunk and past it.
        const auto out = track.read(10'000'000 * 40'000LL / 48'000, 10'000'000 * 130'000LL / 48'000);
        check(out.size() == 90'000 * 2 && out[0] == 9 && out[29'999 * 2] == 9 && out[30'000 * 2] == 0 && out.back() == 0,
              "silence written over a chunk reads silent");
        const auto edge = track.read(10'000'000 * 47'990LL / 48'000, 10'000'000 * 48'010LL / 48'000);
        check(edge.size() == 20 * 2 && edge.front() == 9 && edge.back() == 9, "read across a chunk edge is contiguous");
        AudioTrack empty(300);
        check(empty.read(0, 10'000'000).size() == 96'000 && empty.read(0, 10'000'000)[5] == 0, "unused track reads silent");
    }
    {
        // A long read is copied in pieces; every sample must still be the one written for its own time.
        const auto value = [](std::int64_t frame) { return static_cast<std::int16_t>(frame % 30'011 + 1); };
        const auto unit = [](std::int64_t frames) { return frames * 10'000'000 / AudioTrack::rate; };
        AudioTrack track(10);
        constexpr std::int64_t ringBlock = 480;
        const auto writeBlocks = [&](std::int64_t from, std::int64_t to) {
            std::vector<std::int16_t> data(ringBlock * 2);
            for (std::int64_t f = from; f < to; f += ringBlock) {
                for (std::int64_t i = 0; i < ringBlock; ++i) data[i * 2] = data[i * 2 + 1] = value(f + i);
                track.write(unit(f), data.data(), ringBlock);
            }
        };
        writeBlocks(0, 48'000 * 12); // Two seconds past the ring: frames 0..95'999 are gone.
        const auto out = track.read(unit(10'000), unit(48'000 * 12 - 20'000));
        const auto expectedFrames = static_cast<std::size_t>(48'000 * 12 - 30'000);
        bool exact = out.size() / 2 <= expectedFrames && out.size() / 2 + 1 >= expectedFrames; // The count truncates to a frame.
        for (std::size_t i = 0; exact && i < out.size() / 2; ++i) {
            const std::int64_t frame = 10'000 + static_cast<std::int64_t>(i);
            const std::int16_t expected = frame < 96'000 ? std::int16_t{0} : value(frame);
            exact = out[i * 2] == expected && out[i * 2 + 1] == expected;
        }
        check(exact, "a read over several pieces keeps overwritten frames silent and the rest in place");

        // The writer running ahead of a reader that copies in pieces: a frame is either silent or its own sound.
        std::atomic<bool> stop = false;
        std::int64_t nextFrame = 48'000 * 12;
        std::thread writer([&] {
            while (!stop) { writeBlocks(nextFrame, nextFrame + ringBlock * 100); nextFrame += ringBlock * 100; }
        });
        bool consistent = true;
        for (int round = 0; round < 200 && consistent; ++round) {
            const std::int64_t end = track.framesWritten();
            const std::int64_t start = std::max<std::int64_t>(0, end - 48'000 * 9);
            const auto part = track.read(unit(start), unit(end));
            for (std::size_t i = 0; i < part.size() / 2; ++i) {
                const auto sample = part[i * 2];
                if (sample != 0 && sample != value(start + static_cast<std::int64_t>(i))) { consistent = false; break; }
            }
        }
        stop = true;
        writer.join();
        check(consistent, "a read racing the writer never returns sound from another time");
    }
    check(AudioTrack::ringBytes(60) == 65ull * 48'000 * 4, "a ring holds replay plus five seconds of 16-bit stereo");
    // Microphone filters.
    {
        MicFilters bad;
        bad.gateCloseDb = -10.f; // Above the open level.
        check(validate(bad).has_value(), "gate close above open rejected");
        check(!validate(MicFilters{}).has_value(), "default filters valid");

        const auto rms = [](const std::vector<std::int16_t>& v, std::size_t from, std::size_t to) {
            double sum = 0;
            for (std::size_t i = from; i < to; ++i) sum += double(v[i]) * v[i];
            return std::sqrt(sum / double(to - from));
        };
        std::uint32_t seed = 7;
        const auto noise = [&](float amplitude) {
            seed = seed * 1664525 + 1013904223;
            return static_cast<std::int16_t>(amplitude * ((seed >> 16) / 32768.f - 1.f));
        };

        // Gate: 0.5 s quiet hiss (-60 dB), 0.5 s loud tone (-10 dB), 1 s hiss.
        std::vector<std::int16_t> gated(48'000 * 4);
        for (std::size_t i = 0; i < gated.size() / 2; ++i) {
            const bool loud = i >= 24'000 && i < 48'000;
            const auto v = loud ? static_cast<std::int16_t>(10'362 * std::sin(i * 2 * 3.14159265 * 220 / 48'000)) : noise(33.f);
            gated[2 * i] = gated[2 * i + 1] = v;
        }
        const auto original = gated;
        applyNoiseGate(gated, MicFilters{});
        check(rms(gated, 0, 48'000) == 0, "gate stays shut on quiet hiss");
        check(std::abs(rms(gated, 2 * 30'000, 2 * 48'000) - rms(original, 2 * 30'000, 2 * 48'000)) < 1, "gate passes speech-level sound");
        check(rms(gated, 2 * 48'000, 2 * 54'000) > 0, "gate holds open briefly after sound stops");
        check(rms(gated, 2 * 90'000, 2 * 96'000) == 0, "gate closes after hold and release");

        // Noise suppression: two seconds of loud white noise should be largely removed.
        std::vector<std::int16_t> hiss(48'000 * 4);
        for (std::size_t i = 0; i < hiss.size() / 2; ++i) hiss[2 * i] = hiss[2 * i + 1] = noise(3'000.f);
        const double before = rms(hiss, 48'000, hiss.size());
        applyNoiseSuppression(hiss);
        check(hiss.size() == 48'000 * 4, "suppression keeps the length");
        check(rms(hiss, 48'000, hiss.size()) < before / 10, "suppression removes steady noise by over 20 dB");

        // Voice-like signal (gliding harmonics in syllables) must come out time-aligned:
        // RNNoise lags one frame internally and the filter compensates.
        std::vector<std::int16_t> voice(48'000 * 2 * 2);
        double phase = 0;
        for (std::size_t i = 0; i < voice.size() / 2; ++i) {
            const double t = i / 48'000.0;
            phase += 2 * 3.14159265 * (140 + 30 * std::sin(2 * 3.14159265 * 1.3 * t)) / 48'000;
            const double syllable = std::max(0.0, std::sin(2 * 3.14159265 * 3 * t));
            double v = 0;
            for (int h = 1; h <= 8; ++h) v += std::sin(h * phase) / h;
            voice[2 * i] = voice[2 * i + 1] = static_cast<std::int16_t>(5'000 * syllable * v);
        }
        const auto clean = voice;
        applyNoiseSuppression(voice);
        int bestLag = 0;
        double best = -1;
        for (int lag = -960; lag <= 960; lag += 4) {
            double sum = 0;
            for (std::size_t i = 24'000; i < 72'000; ++i) sum += double(clean[2 * i]) * voice[2 * (i + lag)];
            if (sum > best) { best = sum; bestLag = lag; }
        }
        if (std::abs(bestLag) > 48) std::cerr << "measured lag " << bestLag << " samples" << std::endl;
        check(std::abs(bestLag) <= 48, "suppressed voice stays within 1 ms of the original timing");
        check(rms(voice, 48'000, voice.size()) > rms(clean, 48'000, clean.size()) / 4, "suppression keeps voice");
    }

    // Clip names.
    SYSTEMTIME t{};
    t.wYear = 2026; t.wMonth = 10; t.wDay = 12; t.wHour = 21; t.wMinute = 4; t.wSecond = 5;
    check(clipFileName(L"Valorant.exe", t) == L"Valorant 2026-10-12 21-04-05.mp4", "clip name");
    check(clipFileName(L"a<b>:c?.EXE", t) == L"a_b__c_ 2026-10-12 21-04-05.mp4", "invalid characters replaced");
    check(clipFileName(L"...", t) == L"clip 2026-10-12 21-04-05.mp4", "empty name fallback");
    check(clipFileName(L"apex legends", t) == L"apex legends 2026-10-12 21-04-05.mp4", "friendly name keeps spaces");
    check(clipFileName(L"CON.demo", t) == L"_CON.demo 2026-10-12 21-04-05.mp4", "reserved device before extension");
    check(clipFileName(L"lpt².game", t) == L"_lpt².game 2026-10-12 21-04-05.mp4", "reserved superscript device");
    check(clipFileName(std::wstring(79, L'a') + L".more", t) == std::wstring(79, L'a') + L" 2026-10-12 21-04-05.mp4", "trim after truncation");
    check(clipFileName(std::wstring(79, L'a') + L"\xd83d\xde00", t) == std::wstring(79, L'a') + L" 2026-10-12 21-04-05.mp4", "truncation keeps valid UTF-16");
    check(prepareClipFolder(L"relative\\path").has_value(), "relative folder rejected");

    // The longest clip file name Refarter can make is the one the folder limit was computed from.
    {
        const auto longest = clipFileName(L"CON." + std::wstring(76, L'a'), t);
        check(longest.size() + std::wstring_view(L" (999)").size() + std::wstring_view(L".partial").size() == longestClipFileName,
              "the longest clip name matches the folder limit");
        check(longestClipFileName == 119 && maxClipFolderLength == 127 && maxOutputFolderLength == 139, "clip roots reserve screenshots and the longest generated filename");
        // Refused before anything is created; a trailing separator does not count.
        check(prepareClipFolder(L"C:\\" + std::wstring(maxClipFolderLength - 2, L'a')).has_value(), "a folder one character too long is refused");
        check(prepareClipFolder(L"C:\\" + std::wstring(maxClipFolderLength - 2, L'a') + L"\\").has_value(), "a trailing separator is not counted, still too long");
        const auto base = normalisedFolder(std::filesystem::temp_directory_path());
        const auto room = maxClipFolderLength - base.native().size() - 1; // The longest last component that still fits.
        if (room > 55 && room < 200) {
            const auto unique = L"refarter-folder-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
            const auto fits = base / (unique + std::wstring(room - unique.size(), L'a'));
            check(!prepareClipFolder(fits).has_value(), "a folder of exactly the limit is accepted");
            const auto screenshots = fits / L"screenshots";
            check(!prepareClipFolder(screenshots, ClipFolderKind::Screenshots), "screenshots at the accepted root limit remain writable");
            check(prepareClipFolder(screenshots / L"a", ClipFolderKind::Screenshots).has_value(), "overlong screenshot destination is refused");
            check(std::filesystem::is_empty(screenshots), "screenshot preflight removes its probe");
            check(prepareClipFolder(base / std::wstring(room + 1, L'a')).has_value(), "one more character is refused");
            std::error_code ignored;
            std::filesystem::remove_all(fits, ignored);
        }
    }

    // Free names: a taken name gets the next " (N)" (an " (N)" it carries is replaced), and normalisedFolder drops a trailing separator.
    {
        const auto folder = std::filesystem::temp_directory_path() / L"refarter free names test";
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        std::filesystem::create_directories(folder);
        const auto first = folder / L"g 2026-10-12 21-04-05.mp4";
        check(nextFreePath(first) == first, "a free name is kept");
        std::ofstream(first) << "x";
        check(nextFreePath(first) == folder / L"g 2026-10-12 21-04-05 (2).mp4", "a taken name gets (2)");
        std::ofstream(folder / L"g 2026-10-12 21-04-05 (2).mp4") << "x";
        check(nextFreePath(first) == folder / L"g 2026-10-12 21-04-05 (3).mp4", "(2) taken gives (3)");
        check(nextFreePath(folder / L"g 2026-10-12 21-04-05 (2).mp4") == folder / L"g 2026-10-12 21-04-05 (3).mp4", "a numbered name does not stack suffixes");
        for (int n = 3; n < 1000; ++n) std::ofstream(folder / (L"g 2026-10-12 21-04-05 (" + std::to_wstring(n) + L").mp4")) << "x";
        check(nextFreePath(first).empty(), "no free name after 999: empty, never a reused name");
        std::filesystem::remove_all(folder, ignored);
        check(normalisedFolder(L"D:\\Clips\\") == std::filesystem::path(L"D:\\Clips"), "trailing separator dropped");
        check(normalisedFolder(L"D:\\Clips") == std::filesystem::path(L"D:\\Clips"), "a plain folder is unchanged");
        check(normalisedFolder(L"D:\\a\\.\\b\\..\\c\\") == std::filesystem::path(L"D:\\a\\c"), "dot parts tidied");
        check(normalisedFolder(L"D:\\") == std::filesystem::path(L"D:\\"), "a drive root keeps its separator");
        check(normalisedFolder(L"\\\\server\\share\\x\\") == std::filesystem::path(L"\\\\server\\share\\x"), "a share folder loses its trailing separator");
        check(normalisedFolder(L"\\\\server\\share\\") == std::filesystem::path(L"\\\\server\\share"), "a share is a folder of its server: its trailing separator goes too");
    }

    // Screenshots: the clip name with ".png", numbered when taken, and a PNG that reads back pixel for pixel.
    {
        const auto folder = std::filesystem::temp_directory_path() / L"refarter screenshot test é";
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        std::filesystem::create_directories(folder);
        const auto first = nextClipPath(folder, L"apex legends", L".png");
        check(first.extension() == L".png" && first.stem().wstring().rfind(L"apex legends 20", 0) == 0, "screenshot name");
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // 3 × 2 BGRA with a padded row (pitch 16) and alpha values that must be ignored.
        const std::uint8_t pixels[] = {
            0, 0, 255, 0,   0, 255, 0, 7,   255, 0, 0, 255,   9, 9, 9, 9,
            10, 20, 30, 0,  40, 50, 60, 0,  70, 80, 90, 0,    9, 9, 9, 9};
        check(!writePng(first, pixels, 3, 2, 16), "screenshot written");
        check(!std::filesystem::exists(first.wstring() + L".partial"), "no partial file left");
        check(nextClipPath(folder, L"apex legends", L".png") != first, "a taken name is never offered again");
        check(writePng(first, pixels, 3, 2, 16).has_value() && std::filesystem::exists(first), "an existing screenshot is never replaced");
        check(writePng(folder / L"empty.png", pixels, 0, 2, 16).has_value() && !std::filesystem::exists(folder / L"empty.png"), "an empty picture is refused");
        std::vector<std::uint8_t> read(3 * 2 * 4);
        if (const auto factory = winrt::try_create_instance<IWICImagingFactory>(CLSID_WICImagingFactory)) {
            winrt::com_ptr<IWICBitmapDecoder> decoder;
            winrt::com_ptr<IWICBitmapFrameDecode> frame;
            winrt::com_ptr<IWICFormatConverter> converter;
            UINT width = 0, height = 0;
            const bool decoded = SUCCEEDED(factory->CreateDecoderFromFilename(first.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put())) &&
                SUCCEEDED(decoder->GetFrame(0, frame.put())) && SUCCEEDED(frame->GetSize(&width, &height)) &&
                SUCCEEDED(factory->CreateFormatConverter(converter.put())) &&
                SUCCEEDED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
                SUCCEEDED(converter->CopyPixels(nullptr, 12, static_cast<UINT>(read.size()), read.data()));
            check(decoded && width == 3 && height == 2, "screenshot reads back at its size");
            bool same = decoded;
            for (int y = 0; y < 2 && same; ++y)
                for (int x = 0; x < 3; ++x)
                    for (int c = 0; c < 3; ++c) same = same && read[y * 12 + x * 4 + c] == pixels[y * 16 + x * 4 + c];
            for (int i = 3; i < 24; i += 4) same = same && read[i] == 255; // Opaque, whatever the capture's alpha said.
            check(same, "screenshot pixels and colours survive, opaque");
        } else {
            check(false, "WIC available for the screenshot check");
        }
        CoUninitialize();
        std::filesystem::remove_all(folder, ignored);
    }

    // A recorded region that extends past the capture texture is clamped to it; one wholly outside is refused.
    {
        const auto inside = clampToTexture(RECT{10, 20, 110, 80}, 1920, 1080);
        check(inside && inside->left == 10 && inside->top == 20 && inside->right == 110 && inside->bottom == 80, "region inside the texture is unchanged");
        const auto partly = clampToTexture(RECT{100, 50, 2100, 1200}, 1920, 1080);
        check(partly && partly->left == 100 && partly->top == 50 && partly->right == 1920 && partly->bottom == 1080, "region past the texture is clamped");
        check(!clampToTexture(RECT{2000, 0, 2200, 100}, 1920, 1080), "region wholly outside the texture is refused");
        check(!clampToTexture(RECT{5, 5, 5, 50}, 1920, 1080), "empty region is refused");
    }

    // Audio ring wrap-around: a one-second ring written in uneven pieces, checked against the source PCM.
    {
        AudioTrack ring(1); // 48000 frames.
        std::vector<std::int16_t> pcm(AudioTrack::rate * 2 * 3);
        for (std::size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<std::int16_t>(i * 7 + 1);
        ring.write(0, pcm.data(), 30'000); // No wrap yet.
        ring.write(30'000LL * 10'000'000 / AudioTrack::rate, pcm.data() + 30'000 * 2, 40'000); // Wraps the ring.
        check(ring.framesWritten() == 70'000, "ring counts frames across wrap");
        const std::int64_t t0 = 30'000LL * 10'000'000 / AudioTrack::rate; // Oldest kept frame is 22000.
        const auto old = ring.read(22'000LL * 10'000'000 / AudioTrack::rate, 70'000LL * 10'000'000 / AudioTrack::rate);
        bool same = old.size() == 48'000u * 2;
        for (std::size_t i = 0; same && i < old.size(); ++i) same = old[i] == pcm[22'000 * 2 + i];
        check(same, "read across the ring end is byte-identical");
        const auto before = ring.read(0, t0); // Overwritten part reads as silence, rest intact.
        same = before.size() == 30'000u * 2;
        for (std::size_t i = 0; same && i < before.size(); ++i) same = before[i] == (i < 22'000u * 2 ? 0 : pcm[i]);
        check(same, "read before the oldest frame is silent");
        ring.write(100'000LL * 10'000'000 / AudioTrack::rate, nullptr, 48'000 * 2); // Gap plus silence longer than the ring.
        const auto silent = ring.read(150'000LL * 10'000'000 / AudioTrack::rate, 170'000LL * 10'000'000 / AudioTrack::rate);
        same = !silent.empty();
        for (auto v : silent) same = same && v == 0;
        check(same, "silence longer than the ring fills with zeros");
    }
    // The game-only track stays bound to the process owning the window at the start (window handles are recycled).
    {
        const auto windowOf = [] {
            return CreateWindowExW(0, L"STATIC", L"bound", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        };
        const auto spawn = [](const wchar_t* command) { // A child that runs until killed, or exits at once.
            std::wstring line = command;
            STARTUPINFOW startup{sizeof(startup)};
            PROCESS_INFORMATION info{};
            const bool made = CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
            if (made) CloseHandle(info.hThread);
            return made ? info : PROCESS_INFORMATION{};
        };
        const auto handleTo = [](DWORD pid) { return OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid); };
        const DWORD self = GetCurrentProcessId();

        HWND window = windowOf();
        check(window != nullptr, "test window");
        auto bound = BoundProcess::ofWindow(window);
        check(bound.running() == self, "original alive and the window matches: its owner");
        DestroyWindow(window);
        check(bound.running() == self, "window destroyed: the original process is still the target");
        check(BoundProcess::ofWindow(window).running() == 0 && BoundProcess::ofWindow(nullptr).running() == 0, "a destroyed or missing window binds nothing");

        // A different process is bound; the window now belongs to this one. It must never be returned.
        auto child = spawn(L"ping.exe -n 30 127.0.0.1");
        check(child.hProcess != nullptr, "child process");
        if (child.hProcess) {
            const DWORD childId = child.dwProcessId;
            window = windowOf();
            auto recorderBound = BoundProcess::ofProcess(child.hProcess);
            check(recorderBound.running() == childId, "recorder binding uses the validated handle, not another window owner");
            auto other = BoundProcess::adopt(childId, handleTo(childId));
            check(other.running() == childId && other.running() != self && IsWindow(window), "window owned by another process: still the bound process");
            // The saver's copy outlives the recorder's: dropping the original keeps the handle open.
            std::function<DWORD()> saverCopy = [other] { return other.running(); };
            other = BoundProcess{};
            check(saverCopy() == childId, "a copy used after the producers stopped still answers");
            TerminateProcess(child.hProcess, 0);
            WaitForSingleObject(child.hProcess, 5000);
            check(recorderBound.running() == 0, "recorder binding stops when its original process exits");
            check(BoundProcess::ofProcess(child.hProcess).running() == 0 && IsWindow(window),
                "original exited during startup: recorder binds nothing despite another live window owner");
            check(saverCopy() == 0, "original exited: 0, not the window's owner");
            check(IsWindow(window), "the window is still there to be mistaken for the game");
            DestroyWindow(window);
            CloseHandle(child.hProcess);
        }
        auto quick = spawn(L"cmd.exe /c exit 0");
        if (quick.hProcess) {
            WaitForSingleObject(quick.hProcess, 5000);
            check(BoundProcess::adopt(quick.dwProcessId, handleTo(quick.dwProcessId)).running() == 0, "a process that had already exited is not running");
            CloseHandle(quick.hProcess);
        }
        check(BoundProcess{}.running() == 0 && BoundProcess::adopt(0, nullptr).running() == 0, "nothing bound: 0");
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        check(event != nullptr, "non-process test handle");
        check(BoundProcess::ofProcess(nullptr).running() == 0 &&
            BoundProcess::ofProcess(event).running() == 0, "missing or non-process recorder handle binds nothing");
        if (event) CloseHandle(event);
    }
    return failures ? 1 : 0;
}
