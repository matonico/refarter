#include "player/audio_sources.h"
#include "player/clip_trim.h"
#include "player/timeline.h"
#include <iostream>

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    using namespace refarter::player;

    check(formatTime(0) == L"0:00", "zero");
    check(formatTime(-3) == L"0:00", "negative shows as zero");
    check(formatTime(7.4) == L"0:07", "seconds");
    check(formatTime(59.6) == L"1:00", "rounds into the next minute");
    check(formatTime(3723) == L"1:02:03", "hours");
    check(formatTime(7.44, true) == L"0:07.4", "tenths");
    check(formatTime(59.96, true) == L"1:00.0", "tenths round into the next minute");

    const std::vector<double> keys{0, 1, 2.0005, 3};
    check(keyframeAtOrBefore(keys, 1.5) == 1, "keyframe before");
    check(keyframeAtOrBefore(keys, 2) == 2.0005, "keyframe within timestamp slack");
    check(keyframeAtOrBefore(keys, 9) == 3, "last keyframe");
    check(keyframeAtOrBefore({}, 4) == 0, "no index starts at zero");
    check(keyframeAtOrBefore({0.5}, 0.2) == 0, "before the first keyframe");

    const auto range = snapTrim(keys, 2.7, 2.9, 10);
    check(range.start == 2.0005 && range.end == 2.9, "start snaps, end stays");
    const auto tiny = snapTrim(keys, 3.1, 3.2, 10);
    check(tiny.start == 3 && tiny.end > 3.49, "minimum length");
    check(snapTrim(keys, 4, 20, 10).end == 10, "end clamps to the clip");

    check(trimEndAt(3, 1, 10) == 3 && trimEndAt(1.2, 1, 10) == 1.5 && trimEndAt(20, 1, 10) == 10, "end handle: free, minimum length, clip end");
    check(trimEndAt(0.1, 0, 0.3) == 0.3 && trimEndAt(5, 0, 0.3) == 0.3 && trimEndAt(0, 0.2, 0.3) == 0.3,
          "end handle on a clip shorter than the minimum stays inside it (no empty clamp range)");

    {   // Refarter clips: no reordering. Exact range from the start keyframe.
        VideoCut cut(1, 2);
        std::vector<std::size_t> kept;
        const double pts[] = {1, 1.25, 1.5, 1.75, 2, 2.25};
        const bool key[] = {true, false, false, false, true, false};
        for (std::size_t i = 0; i < 6; ++i) for (auto id : cut.push(i, pts[i], key[i])) kept.push_back(id);
        check((kept == std::vector<std::size_t>{0, 1, 2, 3}), "cut keeps [start, end)");
        check(cut.done(), "keyframe at the end finishes");
    }
    {   // Packets before the start keyframe are never written.
        VideoCut cut(1, 2);
        check(cut.push(0, 0.9, false).empty(), "before the keyframe");
        check(cut.push(1, 1, true) == std::vector<std::size_t>{1}, "the start keyframe");
    }
    {   // Reordered pictures: a reference after the end is kept when a later B-frame needs it.
        VideoCut cut(0, 0.3);
        std::vector<std::size_t> kept;
        // Decode order: I0, P0.4, B0.1, B0.2, P0.8, B0.5 ...
        const double pts[] = {0, 0.4, 0.1, 0.2, 0.8, 0.5};
        const bool key[] = {true, false, false, false, false, false};
        for (std::size_t i = 0; i < 6; ++i) for (auto id : cut.push(i, pts[i], key[i])) kept.push_back(id);
        check((kept == std::vector<std::size_t>{0, 1, 2, 3}), "reference for reordered frames kept");
        check(cut.held().size() == 2, "later pictures wait for a decision");
    }

    TimelineScale scale{100, 200, 10};
    check(scale.x(5) == 200, "time to x");
    check(scale.time(150) == 2.5, "x to time");
    check(scale.time(0) == 0 && scale.time(1000) == 10, "x clamps");

    // Audio sources: the stored mix while all are on, the chosen ones summed otherwise.
    const auto four = audioSources({L"mix", L"desktop", L"microphone", L"discord"});
    check(four.switchable() && four.mix == 0 && four.tracks == std::vector<int>{1, 2, 3}, "refarter tracks become sources");
    check(tracksFor(four, {true, true, true}) == std::vector<int>{0}, "all sources play the stored mix");
    check(tracksFor(four, {true, true, false}) == std::vector<int>{1, 2}, "without discord sums desktop and microphone");
    check(tracksFor(four, {false, false, true}) == std::vector<int>{3}, "discord alone plays its track");
    check(audioSources({L"mix", L"desktop", L"game"}).names == std::vector<std::wstring>{L"desktop", L"game"}, "game is a source");
    const auto two = audioSources({L"mix", L"desktop", L"discord"});
    check(tracksFor(two, {false, true}) == std::vector<int>{2}, "two sources, discord only");
    // The game track is part of desktop audio: a source, but never summed with desktop.
    const auto five = audioSources({L"mix", L"desktop", L"microphone", L"discord", L"game"});
    check(five.switchable() && five.tracks == std::vector<int>{1, 2, 3, 4} && five.names.back() == L"game", "game is a source");
    check(tracksFor(five, {true, true, true, true}) == std::vector<int>{0}, "all five on plays the stored mix");
    check(tracksFor(five, {true, true, true, false}) == std::vector<int>{0}, "game follows desktop: still the stored mix");
    check(effectiveOn(five, {true, false, false, false}) == std::vector<bool>{true, false, false, true}, "game is on while desktop is");
    check(tracksFor(five, {false, true, true, true}) == std::vector<int>{2, 3, 4}, "desktop off: mic, discord and the game track");
    check(tracksFor(five, {false, false, false, true}) == std::vector<int>{4}, "the game alone plays its track");
    check(tracksFor(five, {true, true, false, false}) == std::vector<int>{1, 2}, "desktop and mic without the game track");
    check(trimKeepFor(five, {true, true, false, false}) == std::vector<int>{1, 2, 4} && trimApartFor(five, {true, true, false, false}) == std::vector<int>{4},
          "a trim keeps the game track beside desktop, out of the new mix");
    check(trimKeepFor(five, {false, true, true, true}) == std::vector<int>{2, 3, 4} && trimApartFor(five, {false, true, true, true}).empty(),
          "desktop off: the game is a mixed source");
    check(sourcesLabel(five, {false, false, false, true}) == L"game", "label game alone");
    check(sourcesLabel(five, {true, true, true, false}) == L"mix", "label: game follows desktop");
    check(sourcesLabel(five, {false, true, true, true}) == L"no desktop", "label desktop off");
    const auto gameOnly = audioSources({L"desktop", L"game"});
    check(gameOnly.switchable() && gameOnly.mix < 0 && gameOnly.tracks == std::vector<int>{0, 1}, "desktop and game without a mix still get switches");
    check(tracksFor(gameOnly, {true, true}) == std::vector<int>{0} && tracksFor(gameOnly, {true, false}) == std::vector<int>{0},
          "no mix, game inside desktop: the desktop track alone plays");
    check(tracksFor(gameOnly, {false, true}) == std::vector<int>{1} && tracksFor(gameOnly, {false, false}).empty(), "no mix: the game alone, or silence");
    check(trimKeepFor(gameOnly, {true, true}) == std::vector<int>{0, 1} && trimApartFor(gameOnly, {true, true}) == std::vector<int>{1},
          "no mix: a trim with all on keeps both tracks, the game out of any new mix");
    check(includedIn(gameOnly, {true, true}) == std::vector<bool>{false, true} && includedIn(gameOnly, {false, true}) == std::vector<bool>{false, false},
          "no mix: the game is included only while desktop is on");
    const auto summed = audioSources({L"desktop", L"microphone", L"discord"});
    check(summed.switchable() && tracksFor(summed, {true, true, true}) == std::vector<int>{0, 1, 2} && tracksFor(summed, {true, false, true}) == std::vector<int>{0, 2},
          "no mix: the sources are summed");
    const auto apartSources = audioSources({L"mix", L"desktop", L"game output"});
    check(apartSources.switchable() && apartSources.names.back() == L"game output", "game output is a source");
    check(effectiveOn(apartSources, {true, false}) == std::vector<bool>{true, false}, "game output is not contained in desktop");
    check(tracksFor(apartSources, {true, true}) == std::vector<int>{0} && tracksFor(apartSources, {true, false}) == std::vector<int>{1},
          "desktop alone does not play the independent game");
    check(sourcesLabel(apartSources, {true, false}) == L"desktop", "label: desktop alone, the independent game is off");
    check(allOff(five, {false, false, false, false}) && !allOff(five, {false, false, false, true}) && !allOff(five, {true, false, false, false}),
          "all off is every source off (desktop brings the game)");
    check(tracksFor(five, {false, false, false, false}).empty() && trimKeepFor(five, {false, false, false, false}).empty(),
          "all off decodes and keeps no track (the trim then needs an explicit no-audio flag)");
    check(sourcesLabel(five, {false, false, false, false}) == L"off" && !allOff(AudioSources{}, {}), "label off; no sources is not all off");
    check(includedIn(five, {true, true, true, false}) == std::vector<bool>{false, false, false, true} &&
          includedIn(five, {false, true, true, true}) == std::vector<bool>{false, false, false, false} &&
          includedIn(apartSources, {true, true}) == std::vector<bool>{false, false},
          "the game is included only while desktop is on (not game output)");
    check(!TrimAudio{}.changesAudio() && TrimAudio{{1}}.changesAudio() && TrimAudio{{}, false, {}, {}, true}.changesAudio(),
          "an empty keep list is every track; none is explicit");
    check(!audioSources({L"audio"}).switchable(), "single track is not switchable");
    check(!audioSources({L"track 1", L"track 2"}).switchable(), "other files pick one track");
    return failures ? 1 : 0;
}
