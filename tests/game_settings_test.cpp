#include "game_settings.h"
#include <windows.h>
#include <iostream>

using namespace refarter;
using capture::Codec;
using capture::Quality;
using capture::Resolution;

int wmain() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    const std::wstring head = L"refarter game-settings 1\n";

    GameSettingsList list;
    GameVideo apex;
    apex.resolution = Resolution::P1080;
    apex.fps = 60;
    apex.quality = Quality::High;
    apex.bitrateMbps = 40;
    apex.codec = Codec::HEVC;
    apex.replaySeconds = 60;
    list.set(L"C:\\Games\\Apex\\r5apex.exe", L"apex legends", apex);
    GameVideo quoted;
    quoted.fps = 30;
    list.set(L"D:\\Steam\\Weird \"Quoted\" Game\\game.exe", L"label with \"quotes\"\nand a newline", quoted);
    check(list.games.back().label.find(L'\n') == std::wstring::npos, "labels stay on one line");

    const auto text = formatGameSettings(list);
    check(text.starts_with(head + L"game \"C:\\\\Games\\\\Apex\\\\r5apex.exe\" \"apex legends\" resolution=1080p fps=60 quality=high "
                                  L"bitrate_mbps=40 codec=hevc replay_seconds=60\n"),
          "values are written in order with refarter.ini's names");
    const auto parsed = parseGameSettings(text);
    check(parsed && parsed->games.size() == 2 && parsed->games[0].path == list.games[0].path &&
          parsed->games[0].video == apex && parsed->games[1].path == list.games[1].path &&
          parsed->games[1].label == list.games[1].label && parsed->games[1].video == quoted,
          "round trip keeps paths, labels and values");

    // Case-insensitive find and replacing set, keeping the entry's position.
    check(list.find(L"c:\\games\\apex\\R5APEX.EXE") == &list.games[0], "find ignores case");
    GameVideo only720;
    only720.resolution = Resolution::P720;
    list.set(L"C:\\GAMES\\apex\\r5apex.exe", L"apex", only720);
    check(list.games.size() == 2 && list.games[0].label == L"apex" && list.games[0].video == only720 &&
          list.games[0].path == L"C:\\Games\\Apex\\r5apex.exe", "set replaces label and values in place");
    list.set(L"c:\\games\\apex\\r5apex.exe", L"apex", {});
    check(list.games.size() == 1 && !list.find(L"C:\\Games\\Apex\\r5apex.exe"), "empty values remove the game");
    list.remove(L"d:\\steam\\weird \"quoted\" game\\GAME.exe");
    check(list.games.empty(), "remove ignores case");

    // Lenient parsing.
    check(!parseGameSettings(L"something else\n"), "wrong header gives nothing");
    check(!parseGameSettings(L""), "missing header gives nothing");
    check(!parseGameSettings(L"refarter game-settings 2\ngame \"C:\\a.exe\" \"a\" fps=60\n"), "newer version gives nothing");
    const auto empty = parseGameSettings(head);
    check(empty && empty->games.empty(), "header only is an empty list");
    const auto crlf = parseGameSettings(L"refarter game-settings 1\r\ngame \"C:/a.exe\" \"a\" fps=60\r\n");
    check(crlf && crlf->games.size() == 1 && crlf->games[0].video.fps == 60, "a CRLF hand edit still reads");
    GameSettingsList tabbed;
    tabbed.set(L"C:/t.exe", L"My Game\tDX12", quoted);
    check(tabbed.games[0].label == L"My Game DX12", "a tab in a label becomes a space"); // PR 166 review: menus split at tabs.
    capture::VideoSettings changed;
    setVideoValue(changed, GameValue::Resolution, static_cast<int>(Resolution::P720));
    setVideoValue(changed, GameValue::Bitrate, 40);
    check(changed.resolution == Resolution::P720 && changed.bitrateMbps == 40 && videoValue(changed, GameValue::Bitrate) == 40,
          "setVideoValue is videoValue's inverse");

    const auto badValue = parseGameSettings(head + L"game \"C:/a.exe\" \"a\" resolution=999p fps=30 quality=ultra codec=hevc\n");
    check(badValue && badValue->games.size() == 1 && !badValue->games[0].video.resolution &&
          badValue->games[0].video.fps == 30 && !badValue->games[0].video.quality && badValue->games[0].video.codec == Codec::HEVC,
          "a bad value is skipped, the rest of the line kept");
    const auto badFps = parseGameSettings(head + L"game \"C:/a.exe\" \"a\" fps=45 codec=av1\n");
    check(badFps && badFps->games.size() == 1 && !badFps->games[0].video.fps, "fps 45 is rejected");
    const auto ranges = parseGameSettings(head + L"game \"C:/a.exe\" \"a\" bitrate_mbps=1 replay_seconds=4 fps=60\n"
                                                L"game \"C:/b.exe\" \"b\" bitrate_mbps=250 replay_seconds=3600\n");
    check(ranges && ranges->games.size() == 2 && !ranges->games[0].video.bitrateMbps && !ranges->games[0].video.replaySeconds &&
          ranges->games[1].video.bitrateMbps == 250 && ranges->games[1].video.replaySeconds == 3600,
          "bitrate and replay length stay within their limits");
    const auto unknown = parseGameSettings(head + L"game \"C:/a.exe\" \"a\" volume=50 junk fps=60\n");
    check(unknown && unknown->games.size() == 1 && unknown->games[0].video.fps == 60, "unknown keys are ignored");

    const auto badLines = parseGameSettings(head +
        L"game \"relative/game.exe\" \"x\" fps=60\n"
        L"game \"C:\\unescaped.exe\" \"x\" fps=60\n" // std::quoted drops the lone backslash: C:unescaped.exe is relative.
        L"game C:/unquoted.exe \"x\" fps=60\n"
        L"game \"C:/nolabel.exe\" fps=60\n"
        L"play \"C:/other.exe\" \"x\" fps=60\n"
        L"\n"
        L"game \"C:/allbad.exe\" \"x\" fps=45 codec=vp9\n"
        L"game \"C:/good.exe\" \"good\" codec=av1\n");
    check(badLines && badLines->games.size() == 1 && badLines->games[0].path == L"C:/good.exe" &&
          badLines->games[0].video.codec == Codec::AV1, "bad lines are skipped, others kept");
    const auto duplicate = parseGameSettings(head + L"game \"C:/a.exe\" \"first\" fps=30\n"
                                                   L"game \"c:/A.exe\" \"second\" fps=60\n"
                                                   L"game \"C:/a.exe\" \"bad\" fps=45\n");
    check(duplicate && duplicate->games.size() == 1 && duplicate->games[0].label == L"second" &&
          duplicate->games[0].video.fps == 60, "a later line wins; an all-bad one changes nothing");

    // Resolving against the all-games values.
    capture::VideoSettings defaults;
    defaults.resolution = Resolution::P1440;
    defaults.fps = 60;
    defaults.quality = Quality::Balanced;
    defaults.bitrateMbps = 80;
    defaults.codec = Codec::H264;
    defaults.replaySeconds = 999; // Replaced by the replaySeconds argument.
    auto expected = defaults;
    expected.replaySeconds = 120;
    const auto none = resolveVideo(defaults, 120, nullptr);
    check(!capture::validate(none) && none.resolution == expected.resolution && none.fps == expected.fps &&
          none.quality == expected.quality && none.bitrateMbps == 80 && none.codec == expected.codec && none.replaySeconds == 120,
          "without own values the all-games ones apply");
    GameVideo partial;
    partial.fps = 30;
    partial.codec = Codec::AV1;
    const auto mixed = resolveVideo(defaults, 120, &partial);
    check(!capture::validate(mixed) && mixed.resolution == Resolution::P1440 && mixed.fps == 30 && mixed.codec == Codec::AV1 &&
          mixed.quality == Quality::Balanced && mixed.replaySeconds == 120, "own values override only what they set");
    GameVideo custom;
    custom.quality = Quality::Custom;
    const auto customDefault = resolveVideo(defaults, 120, &custom);
    check(!capture::validate(customDefault) && customDefault.quality == Quality::Custom && customDefault.bitrateMbps == 80,
          "custom quality without its own bitrate takes the all-games bitrate");
    GameVideo bitrateOnly;
    bitrateOnly.bitrateMbps = 30;
    const auto carried = resolveVideo(defaults, 120, &bitrateOnly);
    check(!capture::validate(carried) && carried.quality == Quality::Balanced && carried.bitrateMbps == 30 &&
          !capture::bitrateFor(carried, {2560, 1440}).constant, "an own bitrate with a preset quality is only carried");
    GameVideo replay;
    replay.replaySeconds = 30;
    const auto shorter = resolveVideo(defaults, 120, &replay);
    check(!capture::validate(shorter) && shorter.replaySeconds == 30, "own replay length overrides");
    GameVideo invalid;
    invalid.fps = 500;
    invalid.bitrateMbps = 9999;
    invalid.replaySeconds = 1;
    const auto guarded = resolveVideo(defaults, 120, &invalid);
    check(!capture::validate(guarded) && guarded.fps == 60 && guarded.bitrateMbps == 80 && guarded.replaySeconds == 120,
          "out-of-range own values never reach the encoder");

    // Choosing a value for a game: all games' value clears the game's own rather than saving a copy.
    GameVideo own1080;
    own1080.resolution = Resolution::P1080;
    own1080.replaySeconds = 60;
    const auto toAllGames = chooseValue(own1080, GameValue::Resolution, static_cast<int>(Resolution::P1440), defaults);
    check(!toAllGames.resolution && toAllGames.replaySeconds == 60, "all games' value clears only that own value");
    const auto to720 = chooseValue(own1080, GameValue::Resolution, static_cast<int>(Resolution::P720), defaults);
    check(to720.resolution == Resolution::P720, "another value is the game's own");
    const auto lengthBack = chooseValue(own1080, GameValue::ReplaySeconds, 120, resolveVideo(defaults, 120, nullptr));
    check(!lengthBack.replaySeconds && lengthBack.resolution == Resolution::P1080, "all games' length clears the game's length");
    check(chooseValue({}, GameValue::Fps, 60, defaults).empty(), "all games' value on a game without its own adds nothing");
    for (const auto value : {GameValue::ReplaySeconds, GameValue::Resolution, GameValue::Fps, GameValue::Quality, GameValue::Bitrate, GameValue::Codec}) {
        GameVideo set;
        setOwnValue(set, value, 30);
        check(ownValue(set, value) == 30 && !set.empty(), "each value reads back what was set");
        setOwnValue(set, value, std::nullopt);
        check(set.empty(), "each value clears");
    }

    // A game's own bitrate goes with its own custom quality.
    GameVideo customOwn;
    customOwn.quality = Quality::Custom;
    customOwn.bitrateMbps = 40;
    const auto toBalanced = chooseValue(customOwn, GameValue::Quality, static_cast<int>(Quality::Balanced), defaults);
    check(!toBalanced.quality && !toBalanced.bitrateMbps && toBalanced.empty(), "leaving custom quality drops the own bitrate");
    const auto toHigh = chooseValue(customOwn, GameValue::Quality, static_cast<int>(Quality::High), defaults);
    check(toHigh.quality == Quality::High && !toHigh.bitrateMbps, "a preset quality drops the own bitrate too");
    auto customDefaults = defaults;
    customDefaults.quality = Quality::Custom;
    GameVideo bitrateUnderCustom;
    bitrateUnderCustom.bitrateMbps = 40;
    bitrateUnderCustom.quality = Quality::High;
    const auto backToCustom = chooseValue(bitrateUnderCustom, GameValue::Quality, static_cast<int>(Quality::Custom), customDefaults);
    check(!backToCustom.quality && backToCustom.bitrateMbps == 40, "inheriting all games' custom quality keeps the own bitrate");
    check(!ownValuesApply(bitrateOnly, resolveVideo(defaults, 120, &bitrateOnly)), "a bitrate at a preset quality changes nothing");
    check(ownValuesApply(bitrateOnly, resolveVideo(customDefaults, 120, &bitrateOnly)), "a bitrate at custom quality does");
    check(!startVideo(defaults, 120, &bitrateOnly, {}).own, "an inactive own bitrate is not 'own settings'");

    // What a start records with, and the codec fallback.
    std::vector<capture::CodecSupport> codecs{{Codec::H264, true}, {Codec::HEVC, false}, {Codec::AV1, true}};
    GameVideo hevc;
    hevc.codec = Codec::HEVC;
    const auto refusedStart = startVideo(defaults, 120, &hevc, codecs);
    check(refusedStart.video.codec == Codec::H264 && refusedStart.refused == Codec::HEVC,
          "a game's codec the check refused falls back to h.264 and is named");
    check(!refusedStart.own, "a refused codec alone is not own settings"); // Fresh review 5 of PR 166.
    GameVideo hevc1080 = hevc;
    hevc1080.resolution = Resolution::P1080;
    check(startVideo(defaults, 120, &hevc1080, codecs).own, "...but another own value still is");
    GameVideo inheritedCustom;
    inheritedCustom.bitrateMbps = 40;
    inheritedCustom.quality = Quality::Custom;
    const auto cleared = clearValue(inheritedCustom, GameValue::Quality, defaults);
    check(cleared.empty(), "clearing a custom quality to all games' preset clears the bitrate too"); // Fresh review 1.
    GameSettingsList inherited;
    GameVideo bitrate40;
    bitrate40.bitrateMbps = 40;
    inherited.set(L"C:/x.exe", L"x", bitrate40);
    inherited.set(L"C:/y.exe", L"y", inheritedCustom);
    check(inherited.dropUnusedBitrates(defaults) && inherited.games.size() == 1 && inherited.games[0].video.bitrateMbps == 40,
          "all games off custom: only games without their own custom quality lose their bitrate");
    const auto unchecked = startVideo(defaults, 120, &hevc, {});
    check(unchecked.video.codec == Codec::HEVC && !unchecked.refused, "before the check the game's codec is used");
    GameVideo av1;
    av1.codec = Codec::AV1;
    check(startVideo(defaults, 120, &av1, codecs).video.codec == Codec::AV1, "a codec that records is kept");
    auto hevcDefaults = defaults;
    hevcDefaults.codec = Codec::HEVC;
    const auto monitor = startVideo(hevcDefaults, 120, nullptr, codecs);
    check(!monitor.own && monitor.video.codec == Codec::HEVC && !monitor.refused,
          "without own values nothing falls back here (all games' choice is handled by the codec check)");

    // When an invalid number field blocks a start.
    GameVideo ownLength;
    ownLength.replaySeconds = 60;
    check(fieldsAllowStart(false, false, nullptr, Quality::Custom, false, false), "fields editing another game never block");
    check(!fieldsAllowStart(true, true, nullptr, Quality::Balanced, false, true), "an invalid all-games length blocks all games");
    check(fieldsAllowStart(true, true, &ownLength, Quality::Balanced, false, true), "...but not a game with its own length");
    check(!fieldsAllowStart(true, false, &ownLength, Quality::Balanced, false, true), "an invalid field for the source game blocks it");
    check(fieldsAllowStart(true, true, nullptr, Quality::Balanced, true, false), "an invalid bitrate matters only at custom quality");
    check(!fieldsAllowStart(true, true, nullptr, Quality::Custom, true, false), "an invalid bitrate blocks a custom start");
    GameVideo ownBitrate;
    ownBitrate.bitrateMbps = 40;
    check(fieldsAllowStart(true, true, &ownBitrate, Quality::Custom, true, false), "...unless the game has its own bitrate");

    // File round trip.
    GameSettingsList saved;
    saved.set(L"C:\\Games\\Apex\\r5apex.exe", L"apex legends", apex);
    saved.set(L"D:\\Other\\game.exe", L"other", quoted);
    wchar_t folder[MAX_PATH]{}, temporary[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, folder) || !GetTempFileNameW(folder, L"rgs", 0, temporary)) return 1;
    std::filesystem::remove(temporary);
    check(readGameSettings(temporary) && readGameSettings(temporary)->games.empty(), "missing file is an empty list");
    check(writeGameSettings(temporary, saved), "list is written");
    const auto read = readGameSettings(temporary);
    check(read && formatGameSettings(*read) == formatGameSettings(saved), "file round trip");
    std::filesystem::remove(temporary);
    check(!gameSettingsPath().empty() && gameSettingsPath().filename() == L"game_settings.dat", "file lives beside auto_capture.dat");
    return failures ? 1 : 0;
}
