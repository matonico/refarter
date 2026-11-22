#pragma once

#include "capture/codec_probe.h"
#include "capture/video_settings.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace refarter {

// A game's own recording values. Anything left empty follows the all-games
// settings, so a later change there reaches every value the game hasn't set.
// Audio is never per game: it can be tuned afterwards in the clip editor.
struct GameVideo {
    std::optional<capture::Resolution> resolution;
    std::optional<int> fps;            // 30 or 60, as the page offers.
    std::optional<capture::Quality> quality;
    std::optional<int> bitrateMbps;    // Used only when the resolved quality is custom.
    std::optional<capture::Codec> codec;
    std::optional<int> replaySeconds;  // 5-3600.

    bool empty() const {
        return !resolution && !fps && !quality && !bitrateMbps && !codec && !replaySeconds;
    }
    friend bool operator==(const GameVideo&, const GameVideo&) = default;
};

// One game with its own values, identified like auto-capture by its full
// executable path (compared case-insensitively; a moved install loses them).
struct GameSettingsEntry {
    std::wstring path;
    std::wstring label; // Shown in the "settings for" menu.
    GameVideo video;
};

struct GameSettingsList {
    std::vector<GameSettingsEntry> games;

    const GameSettingsEntry* find(const std::wstring& path) const;
    // Stores the game's values and label; values that are all empty remove the game.
    void set(std::wstring path, std::wstring label, GameVideo video);
    void remove(const std::wstring& path);
    // After all games' quality changes: drops the bitrate of every game that no longer records at custom quality (and
    // a game left with nothing). True when anything changed, so the caller writes the file.
    bool dropUnusedBitrates(const capture::VideoSettings& allGames);
};

// What a replay of this game starts with: the game's own values over the
// all-games ones. `own` may be null (monitor source, or a game without its own
// values). A custom quality without the game's own bitrate takes the all-games
// bitrate. The result's replaySeconds is the game's own length or `replaySeconds`.
capture::VideoSettings resolveVideo(const capture::VideoSettings& defaults, int replaySeconds, const GameVideo* own);

// One value a game can set, in the Recording page's order; each reset arrow names one.
enum class GameValue { ReplaySeconds, Resolution, Fps, Quality, Bitrate, Codec };
// The value as a number (an enum's index, fps, mbps or seconds), so the page handles all six alike.
std::optional<int> ownValue(const GameVideo& own, GameValue value);
void setOwnValue(GameVideo& own, GameValue value, std::optional<int> number);
int videoValue(const capture::VideoSettings& video, GameValue value);
// Sets one of all games' values (the length too) from its number, the inverse of videoValue.
void setVideoValue(capture::VideoSettings& video, GameValue value, int number);
// A value chosen for a game. All games' value gives the game nothing of its own (and clears what it had), so it
// keeps following all games instead of holding a copy that would look changed. A quality that leaves the game off
// custom also clears its own bitrate, which only custom quality uses.
GameVideo chooseValue(GameVideo own, GameValue value, int number, const capture::VideoSettings& allGames);
// A reset arrow: the game follows all games for that value; clearing its quality applies the same bitrate rule.
GameVideo clearValue(GameVideo own, GameValue value, const capture::VideoSettings& allGames);
// Whether a game's own values change what `resolved` records (an own bitrate only does at custom quality): what
// "· own settings" and the diagnostic line report.
bool ownValuesApply(const GameVideo& own, const capture::VideoSettings& resolved);

// What a start records with: the game's own values (null for a monitor or a game without them) over all games'. A
// game's codec this PC's check refused (once `codecs` is known) falls back to h.264, as a saved all-games choice does,
// and is named in `refused`; before the check nothing is known, for either.
struct StartVideo {
    capture::VideoSettings video;
    bool own = false; // A game's own values changed what is recorded (see ownValuesApply).
    std::optional<capture::Codec> refused;
};
StartVideo startVideo(const capture::VideoSettings& allGames, int replaySeconds, const GameVideo* own,
                      const std::vector<capture::CodecSupport>& codecs);
// The codec a game with its own values (`own`) would record that this PC's check refused, or nothing (a monitor, a
// game without its own values, h.264, or before the check). startVideo's fallback rule, for values already resolved.
std::optional<capture::Codec> refusedCodec(bool own, capture::Codec codec, const std::vector<capture::CodecSupport>& codecs);

// Whether the page's number fields (replay length, custom bitrate) allow a start: an invalid one blocks only what it
// edits. `editsSource`: the page edits all games or the source's own game; `allGames`: it edits all games, whose
// length or bitrate a source game with its own (`sourceOwn`) does not use.
bool fieldsAllowStart(bool editsSource, bool allGames, const GameVideo* sourceOwn, capture::Quality quality,
                      bool lengthValid, bool bitrateValid);

// %LOCALAPPDATA%\Refarter\game_settings.dat: UTF-16, written atomically like
// auto_capture.dat. The first line is "refarter game-settings 1"; then one line
// per game:
//   game "<path>" "<label>" resolution=1080p fps=60 quality=high bitrate_mbps=40 codec=hevc replay_seconds=60
// Only the game's own values are written, in that order, using refarter.ini's
// value names (resolutionNames, qualityNames, h264/hevc/av1). Path and label are
// std::quoted, so a backslash is written "\\"; a hand-written single "\" makes
// the path relative and that line is skipped.
std::filesystem::path gameSettingsPath();
std::wstring formatGameSettings(const GameSettingsList& list);
// Lenient, unlike auto-capture: a malformed line or value is skipped, so a bad
// hand edit costs only that line or value. A wrong header gives nothing (a
// newer or foreign file), and the caller then never overwrites the file.
std::optional<GameSettingsList> parseGameSettings(const std::wstring& text);
// A missing file is an empty list; an unreadable file or a wrong header is nothing.
std::optional<GameSettingsList> readGameSettings(const std::filesystem::path& file);
bool writeGameSettings(const std::filesystem::path& file, const GameSettingsList& list);

} // namespace refarter
