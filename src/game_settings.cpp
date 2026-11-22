#include "game_settings.h"
#include "local_file.h"
#include "settings.h"
#include "path_list.h"
#include "text.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace refarter {
namespace {
constexpr const wchar_t* header = L"refarter game-settings 1";
constexpr std::uintmax_t maxBytes = 1024 * 1024;

// The page offers 30 and 60 fps only, as refarter.ini does.
bool validFps(int fps) { return fps == 30 || fps == 60; }
bool validBitrate(int mbps) { return mbps >= capture::minBitrateMbps && mbps <= capture::maxBitrateMbps; }
bool validReplay(int seconds) { return seconds >= capture::minReplaySeconds && seconds <= capture::maxReplaySeconds; }

template <size_t N>
bool inRange(auto value, const wchar_t* const (&)[N]) { return static_cast<unsigned>(value) < N; }

// Applies one key=value pair; unknown keys and bad values are ignored.
void apply(GameVideo& video, const std::wstring& key, const std::wstring& value) {
    if (key == L"resolution") {
        if (const int i = detail::pick(value, resolutionNames, -1); i >= 0) video.resolution = static_cast<capture::Resolution>(i);
    } else if (key == L"fps") {
        if (const auto fps = parseNumber(value, 30, 60); fps && validFps(*fps)) video.fps = *fps;
    } else if (key == L"quality") {
        if (const int i = detail::pick(value, qualityNames, -1); i >= 0) video.quality = static_cast<capture::Quality>(i);
    } else if (key == L"bitrate_mbps") {
        if (const auto mbps = parseNumber(value, capture::minBitrateMbps, capture::maxBitrateMbps)) video.bitrateMbps = *mbps;
    } else if (key == L"codec") {
        if (const int i = detail::pick(value, detail::codecNames, -1); i >= 0) video.codec = static_cast<capture::Codec>(i);
    } else if (key == L"replay_seconds") {
        if (const auto seconds = parseNumber(value, capture::minReplaySeconds, capture::maxReplaySeconds)) video.replaySeconds = *seconds;
    }
}
} // namespace

const GameSettingsEntry* GameSettingsList::find(const std::wstring& path) const { return findByPath(games, path); }

void GameSettingsList::set(std::wstring path, std::wstring label, GameVideo video) {
    if (video.empty()) {
        remove(path);
        return;
    }
    if (auto* game = findByPath(games, path)) {
        game->label = oneLineLabel(std::move(label));
        game->video = video;
        return;
    }
    games.push_back({std::move(path), oneLineLabel(std::move(label)), video});
}

void GameSettingsList::remove(const std::wstring& path) { eraseByPath(games, path); }

namespace {
// A game's own bitrate belongs to custom quality: once the game no longer records at custom, its bitrate row and
// reset arrow are gone, so a kept bitrate could only show as "own settings" that change nothing.
void dropUnusedBitrate(GameVideo& own, const capture::VideoSettings& allGames) {
    if (own.quality.value_or(allGames.quality) != capture::Quality::Custom) own.bitrateMbps.reset();
}
} // namespace

bool GameSettingsList::dropUnusedBitrates(const capture::VideoSettings& allGames) {
    bool changed = false;
    for (auto& game : games) {
        const auto before = game.video;
        dropUnusedBitrate(game.video, allGames);
        changed = changed || game.video != before;
    }
    std::erase_if(games, [](const auto& game) { return game.video.empty(); });
    return changed;
}

capture::VideoSettings resolveVideo(const capture::VideoSettings& defaults, int replaySeconds, const GameVideo* own) {
    auto video = defaults;
    // A value out of range (never written by the page) falls back rather than reaching the encoder.
    video.replaySeconds = std::clamp(replaySeconds, capture::minReplaySeconds, capture::maxReplaySeconds);
    if (!own) return video;
    if (own->resolution && inRange(*own->resolution, resolutionNames)) video.resolution = *own->resolution;
    if (own->fps && validFps(*own->fps)) video.fps = *own->fps;
    if (own->quality && inRange(*own->quality, qualityNames)) video.quality = *own->quality;
    // Without its own bitrate, a custom quality takes the all-games one already in `video`.
    if (own->bitrateMbps && validBitrate(*own->bitrateMbps)) video.bitrateMbps = *own->bitrateMbps;
    if (own->codec && inRange(*own->codec, detail::codecNames)) video.codec = *own->codec;
    if (own->replaySeconds && validReplay(*own->replaySeconds)) video.replaySeconds = *own->replaySeconds;
    return video;
}

std::optional<int> ownValue(const GameVideo& own, GameValue value) {
    const auto number = [](const auto& v) { return v ? std::optional<int>(static_cast<int>(*v)) : std::nullopt; };
    switch (value) {
    case GameValue::ReplaySeconds: return own.replaySeconds;
    case GameValue::Resolution: return number(own.resolution);
    case GameValue::Fps: return own.fps;
    case GameValue::Quality: return number(own.quality);
    case GameValue::Bitrate: return own.bitrateMbps;
    case GameValue::Codec: return number(own.codec);
    }
    return std::nullopt;
}

void setOwnValue(GameVideo& own, GameValue value, std::optional<int> number) {
    const auto as = [&]<typename T>(std::optional<T>& field) { field = number ? std::optional(static_cast<T>(*number)) : std::nullopt; };
    switch (value) {
    case GameValue::ReplaySeconds: own.replaySeconds = number; break;
    case GameValue::Resolution: as(own.resolution); break;
    case GameValue::Fps: own.fps = number; break;
    case GameValue::Quality: as(own.quality); break;
    case GameValue::Bitrate: own.bitrateMbps = number; break;
    case GameValue::Codec: as(own.codec); break;
    }
}

int videoValue(const capture::VideoSettings& video, GameValue value) {
    switch (value) {
    case GameValue::ReplaySeconds: return video.replaySeconds;
    case GameValue::Resolution: return static_cast<int>(video.resolution);
    case GameValue::Fps: return video.fps;
    case GameValue::Quality: return static_cast<int>(video.quality);
    case GameValue::Bitrate: return video.bitrateMbps;
    case GameValue::Codec: return static_cast<int>(video.codec);
    }
    return 0;
}

void setVideoValue(capture::VideoSettings& video, GameValue value, int number) {
    switch (value) {
    case GameValue::ReplaySeconds: video.replaySeconds = number; break;
    case GameValue::Resolution: video.resolution = static_cast<capture::Resolution>(number); break;
    case GameValue::Fps: video.fps = number; break;
    case GameValue::Quality: video.quality = static_cast<capture::Quality>(number); break;
    case GameValue::Bitrate: video.bitrateMbps = number; break;
    case GameValue::Codec: video.codec = static_cast<capture::Codec>(number); break;
    }
}

GameVideo chooseValue(GameVideo own, GameValue value, int number, const capture::VideoSettings& allGames) {
    setOwnValue(own, value, videoValue(allGames, value) == number ? std::nullopt : std::optional<int>(number));
    if (value == GameValue::Quality) dropUnusedBitrate(own, allGames);
    return own;
}

GameVideo clearValue(GameVideo own, GameValue value, const capture::VideoSettings& allGames) {
    setOwnValue(own, value, std::nullopt);
    if (value == GameValue::Quality) dropUnusedBitrate(own, allGames);
    return own;
}

bool ownValuesApply(const GameVideo& own, const capture::VideoSettings& resolved) {
    // A bitrate counts only at custom quality, a codec only if it is the one recorded (not refused by the check);
    // every other own value always changes what is recorded.
    return own.resolution || own.fps || own.quality || own.replaySeconds || (own.codec && *own.codec == resolved.codec) ||
        (own.bitrateMbps && resolved.quality == capture::Quality::Custom);
}

std::optional<capture::Codec> refusedCodec(bool own, capture::Codec codec, const std::vector<capture::CodecSupport>& codecs) {
    if (!own || codec == capture::Codec::H264 || codecs.empty()) return std::nullopt;
    const auto support = std::find_if(codecs.begin(), codecs.end(), [&](const auto& s) { return s.codec == codec; });
    return support == codecs.end() || !support->records ? std::optional(codec) : std::nullopt;
}

StartVideo startVideo(const capture::VideoSettings& allGames, int replaySeconds, const GameVideo* own,
                      const std::vector<capture::CodecSupport>& codecs) {
    StartVideo start{resolveVideo(allGames, replaySeconds, own), false, std::nullopt};
    start.refused = refusedCodec(own != nullptr, start.video.codec, codecs);
    if (start.refused) start.video.codec = capture::Codec::H264;
    // After the fallback: a refused codec alone is not "own settings" (it records all games' values and h.264).
    start.own = own && ownValuesApply(*own, start.video);
    return start;
}

bool fieldsAllowStart(bool editsSource, bool allGames, const GameVideo* sourceOwn, capture::Quality quality,
                      bool lengthValid, bool bitrateValid) {
    if (!editsSource) return true;
    // All games' field is not what a game with its own length (or bitrate) records with.
    const bool length = !(allGames && sourceOwn && sourceOwn->replaySeconds);
    const bool bitrate = quality == capture::Quality::Custom && !(allGames && sourceOwn && sourceOwn->bitrateMbps);
    return (!length || lengthValid) && (!bitrate || bitrateValid);
}

std::filesystem::path gameSettingsPath() { return localAppDataFile(L"game_settings.dat"); }

std::wstring formatGameSettings(const GameSettingsList& list) {
    std::wostringstream text;
    text << header << L'\n';
    for (const auto& game : list.games) {
        const auto& v = game.video;
        text << L"game " << std::quoted(game.path) << L' ' << std::quoted(game.label);
        if (v.resolution && inRange(*v.resolution, resolutionNames)) text << L" resolution=" << resolutionNames[static_cast<int>(*v.resolution)];
        if (v.fps) text << L" fps=" << *v.fps;
        if (v.quality && inRange(*v.quality, qualityNames)) text << L" quality=" << qualityNames[static_cast<int>(*v.quality)];
        if (v.bitrateMbps) text << L" bitrate_mbps=" << *v.bitrateMbps;
        if (v.codec && inRange(*v.codec, detail::codecNames)) text << L" codec=" << detail::codecNames[static_cast<int>(*v.codec)];
        if (v.replaySeconds) text << L" replay_seconds=" << *v.replaySeconds;
        text << L'\n';
    }
    return text.str();
}

std::optional<GameSettingsList> parseGameSettings(const std::wstring& text) {
    std::wistringstream lines(text);
    std::wstring line;
    if (!std::getline(lines, line)) return std::nullopt;
    if (line.ends_with(L'\r')) line.pop_back(); // A hand edit may have saved CRLF.
    if (line != header) return std::nullopt;
    GameSettingsList list;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        std::wistringstream record(line);
        std::wstring kind, path, label;
        if (!(record >> kind) || kind != L"game") continue;
        // std::quoted reads an unquoted word too; only a quoted path and label make a game line.
        if (!(record >> std::ws) || record.peek() != L'"' || !(record >> std::quoted(path))) continue;
        if (!(record >> std::ws) || record.peek() != L'"' || !(record >> std::quoted(label))) continue;
        if (!singleLine(path) || !singleLine(label) ||!std::filesystem::path(path).is_absolute()) continue;
        GameVideo video;
        for (std::wstring pair; record >> pair;) {
            const auto equals = pair.find(L'=');
            if (equals != std::wstring::npos) apply(video, pair.substr(0, equals), pair.substr(equals + 1));
        }
        // Nothing usable: drop the line rather than let it remove an earlier one for the same game.
        if (!video.empty()) list.set(std::move(path), std::move(label), video);
    }
    return list;
}

std::optional<GameSettingsList> readGameSettings(const std::filesystem::path& file) {
    std::wstring text;
    switch (readUtf16File(file, maxBytes, text)) {
    case Utf16Read::Missing: return GameSettingsList{};
    case Utf16Read::Ok: return parseGameSettings(text);
    default: return std::nullopt;
    }
}

bool writeGameSettings(const std::filesystem::path& file, const GameSettingsList& list) {
    return writeUtf16FileAtomic(file, formatGameSettings(list), maxBytes, L".game-settings-");
}

} // namespace refarter
