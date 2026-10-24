#pragma once

#include "capture/audio_device.h"
#include "capture/audio_levels.h"
#include "capture/mic_filters.h"
#include "capture/video_settings.h"
#include "replay_duration.h"
#include "storage_limit.h"
#include "text.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace refarter {

// Whether Refarter may ask GitHub for the latest release (see update_check.h). Ask: not decided yet; the question is
// asked once and the answer is treated as off until then.
enum class UpdateChoice { Ask, On, Off };
inline constexpr const wchar_t* updateChoiceNames[] = {L"ask", L"on", L"off"};

// Every choice from the Recording and Settings pages that survives a restart.
// The save shortcut is stored beside these, in [shortcuts] (see hotkey.h).
struct UserSettings {
    std::wstring sourceMonitor; // Device name, e.g. \\.\DISPLAY1; empty when a game or nothing was chosen.
    int replaySeconds = 180;
    bool autoCapture = true;        // Listed games start replay when they come to the front.
    bool autoCapturePrompt = true;  // Offer new fullscreen apps for the list (a card that never takes focus).
    capture::VideoSettings video;
    capture::AudioDevice desktopAudio{capture::AudioDevice::Mode::SystemDefault};
    capture::AudioDevice microphone;
    capture::MicFilters micFilters;
    capture::AudioLevels levels;      // Save-time volumes, 0-200%.
    bool hideDiscord = false;         // Discord is left out instead of getting its own track.
    int chimePercent = 100;           // Save-request chime, 0-100%; 0 is silent.
    std::filesystem::path clipFolder; // Empty: the default Videos\Refarter.
    unsigned maxStorageGb = 0;        // Limit for the clip folder in gb; 0 is no limit.
    StorageFull storageFull = StorageFull::Warn; // What a save does at the limit.
    bool storageFullChosen = false;   // The user picked it (the first-time question is not asked again).
    bool smallTiles = false;          // Library tile size: large (default) or small.
    bool groupFolders = false;        // Library: clips shown in a folder per game.
    bool askBeforeBinning = true;     // Library: ask before moving clips to the recycle bin (the question's box turns it off).
    int playerVolume = 100;           // The built-in player's volume, 0-100%.
    bool compressCopyAfterTrim = false; // The trim bar's "compress & copy" box starts ticked.
    bool closeToTray = false;         // Closing the window hides it in the tray.
    bool startWithWindows = false;    // Parsed and written for the file only; the Run registry key (refreshStartup) is the source, never this value.
    bool startMinimised = true;       // Sign-in starts in the tray; ordinary launches still open the window.
    bool sidebarCollapsed = false;    // The sidebar shows icons only.
    bool diagnosticLogging = false;  // Opt-in local session events, never media or personal paths.
    UpdateChoice updateCheck = UpdateChoice::Ask; // [updates] check: the daily release check is opt-in.
    std::wstring availableUpdateTag, availableUpdateUrl; // Last known newer release, retained across restarts.
    long long lastUpdateCheck = 0;   // [updates] last_check: unix seconds of the last check that got an answer; 0 for none.
};

struct SettingEntry {
    const wchar_t* section;
    const wchar_t* key;
    std::wstring value;
};

// Names of the video choices, indexed by capture::Resolution and capture::Quality: written to
// refarter.ini and shown on the Recording page.
inline constexpr const wchar_t* resolutionNames[] = {L"native", L"2160p", L"1440p", L"1080p", L"720p"};
inline constexpr const wchar_t* qualityNames[] = {L"small", L"balanced", L"high", L"custom"};

// Noise gate limits shared by the file reader and the gate's number fields.
inline constexpr int gateLevelMinDb = -96, gateLevelMaxDb = 0, gateTimeMaxMs = 10'000;

// A whole decimal number within [low, high]; empty text or trailing characters give nothing.
inline std::optional<int> parseNumber(const std::wstring& text, int low, int high) {
    if (text.empty()) return std::nullopt;
    wchar_t* end = nullptr;
    const long value = std::wcstol(text.c_str(), &end, 10);
    if (*end || value < low || value > high) return std::nullopt;
    return static_cast<int>(value);
}

// Unix seconds as stored in the file: digits only, up to the year 9999; anything else is "never checked".
inline long long parseUnixSeconds(const std::wstring& text) {
    if (text.empty() || text.size() > 12 || !std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) return 0;
    const long long value = std::wcstoll(text.c_str(), nullptr, 10);
    return value <= 253'402'300'799LL ? value : 0;
}

// A storage limit in gb: a number above the field's largest value is that largest value (silently dropping the
// limit would let the folder grow unchecked); text that is not a number is no limit.
inline unsigned clampStorageGb(const std::wstring& text) {
    const auto value = parseNumber(text, 0, (std::numeric_limits<int>::max)());
    if (value) return (std::min)(static_cast<unsigned>(*value), maxStorageGb);
    const bool digits = !text.empty() && std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
    return digits ? maxStorageGb : 0;
}

namespace detail {
inline constexpr const wchar_t* codecNames[] = {L"h264", L"hevc", L"av1"};

inline std::wstring flag(bool value) { return value ? L"on" : L"off"; }

// "off", "default" or a fixed endpoint ID; endpoint IDs never use those words.
inline std::wstring deviceValue(const capture::AudioDevice& device) {
    using Mode = capture::AudioDevice::Mode;
    return device.mode == Mode::Off ? L"off" : device.mode == Mode::SystemDefault ? L"default" : device.id;
}
inline std::wstring deviceName(const capture::AudioDevice& device) {
    return device.mode == capture::AudioDevice::Mode::Fixed ? device.name : L"";
}

template <size_t N>
int pick(const std::wstring& text, const wchar_t* const (&names)[N], int fallback) {
    const auto name = lower(text);
    for (size_t i = 0; i < N; ++i) if (name == names[i]) return static_cast<int>(i);
    return fallback;
}
} // namespace detail

// The file's lines for these settings, in a fixed order.
inline std::vector<SettingEntry> settingEntries(const UserSettings& s) {
    using namespace detail;
    const auto& f = s.micFilters;
    return {
        {L"recording", L"source_monitor", s.sourceMonitor},
        {L"recording", L"replay_seconds", std::to_wstring(s.replaySeconds)},
        {L"recording", L"auto_capture", flag(s.autoCapture)},
        {L"recording", L"auto_capture_prompt", flag(s.autoCapturePrompt)},
        {L"video", L"resolution", resolutionNames[static_cast<int>(s.video.resolution)]},
        {L"video", L"fps", std::to_wstring(s.video.fps)},
        {L"video", L"quality", qualityNames[static_cast<int>(s.video.quality)]},
        {L"video", L"bitrate_mbps", std::to_wstring(s.video.bitrateMbps)},
        {L"video", L"codec", codecNames[static_cast<int>(s.video.codec)]},
        {L"audio", L"desktop_device", deviceValue(s.desktopAudio)},
        {L"audio", L"desktop_device_name", deviceName(s.desktopAudio)},
        {L"audio", L"microphone_device", deviceValue(s.microphone)},
        {L"audio", L"microphone_device_name", deviceName(s.microphone)},
        {L"audio", L"desktop_volume", std::to_wstring(s.levels.desktopPercent)},
        {L"audio", L"mic_volume", std::to_wstring(s.levels.microphonePercent)},
        {L"audio", L"hide_discord", flag(s.hideDiscord)},
        {L"audio", L"chime_volume", std::to_wstring(s.chimePercent)},
        {L"audio", L"noise_suppression", flag(f.noiseSuppression)},
        {L"audio", L"noise_gate", flag(f.noiseGate)},
        {L"audio", L"gate_open_db", std::to_wstring(static_cast<int>(f.gateOpenDb))},
        {L"audio", L"gate_close_db", std::to_wstring(static_cast<int>(f.gateCloseDb))},
        {L"audio", L"gate_attack_ms", std::to_wstring(f.gateAttackMs)},
        {L"audio", L"gate_hold_ms", std::to_wstring(f.gateHoldMs)},
        {L"audio", L"gate_release_ms", std::to_wstring(f.gateReleaseMs)},
        {L"storage", L"clip_folder", s.clipFolder.wstring()},
        {L"storage", L"max_storage_gb", std::to_wstring(s.maxStorageGb)},
        {L"storage", L"when_full", s.storageFullChosen ? storageFullNames[static_cast<int>(s.storageFull)] : L""},
        {L"library", L"density", s.smallTiles ? L"compact" : L"comfortable"},
        {L"library", L"folders", flag(s.groupFolders)},
        {L"library", L"ask_before_binning", flag(s.askBeforeBinning)},
        {L"library", L"player_volume", std::to_wstring(s.playerVolume)},
        {L"library", L"compress_copy_after_trim", flag(s.compressCopyAfterTrim)},
        {L"window", L"close_to_tray", flag(s.closeToTray)},
        {L"window", L"start_with_windows", flag(s.startWithWindows)},
        {L"window", L"start_minimised", flag(s.startMinimised)},
        {L"window", L"sidebar", s.sidebarCollapsed ? L"collapsed" : L"expanded"},
        {L"diagnostics", L"logging", flag(s.diagnosticLogging)},
        {L"updates", L"check", updateChoiceNames[static_cast<int>(s.updateCheck)]},
        {L"updates", L"last_check", std::to_wstring(s.lastUpdateCheck)},
        {L"updates", L"available_tag", s.availableUpdateTag},
        {L"updates", L"available_url", s.availableUpdateUrl},
    };
}

// Reads parseConfig's "section/key" values. Missing or unreadable values keep
// their defaults, one setting at a time, so a bad hand edit costs only that line.
inline UserSettings readSettings(const std::map<std::wstring, std::wstring>& values) {
    using namespace detail;
    const auto get = [&](const wchar_t* name) {
        const auto found = values.find(name);
        return found == values.end() ? std::wstring{} : found->second;
    };
    const auto toggle = [&](const wchar_t* name, bool fallback) {
        const auto text = lower(get(name));
        return text == L"on" ? true : text == L"off" ? false : fallback;
    };
    UserSettings s;
    s.sourceMonitor = get(L"recording/source_monitor");
    s.replaySeconds = parseReplaySeconds(get(L"recording/replay_seconds")).value_or(s.replaySeconds);
    s.autoCapture = toggle(L"recording/auto_capture", s.autoCapture);
    s.autoCapturePrompt = toggle(L"recording/auto_capture_prompt", s.autoCapturePrompt);
    s.video.replaySeconds = s.replaySeconds;
    s.video.resolution = static_cast<capture::Resolution>(pick(get(L"video/resolution"), resolutionNames, static_cast<int>(s.video.resolution)));
    // The page offers 30 and 60 fps.
    if (const auto fps = parseNumber(get(L"video/fps"), 30, 60); fps == 30 || fps == 60) s.video.fps = *fps;
    s.video.quality = static_cast<capture::Quality>(pick(get(L"video/quality"), qualityNames, static_cast<int>(s.video.quality)));
    s.video.bitrateMbps = parseNumber(get(L"video/bitrate_mbps"), capture::minBitrateMbps, capture::maxBitrateMbps).value_or(s.video.bitrateMbps);
    s.video.codec = static_cast<capture::Codec>(pick(get(L"video/codec"), codecNames, static_cast<int>(s.video.codec)));
    // Device keys win; files from before device choice only have on/off, which
    // means following the Windows default.
    const auto device = [&](const wchar_t* key, const wchar_t* nameKey, const wchar_t* legacy, capture::AudioDevice fallback) {
        using Mode = capture::AudioDevice::Mode;
        const auto value = get(key);
        if (value.empty()) {
            const bool on = toggle(legacy, fallback.enabled());
            return capture::AudioDevice{on ? Mode::SystemDefault : Mode::Off};
        }
        if (lower(value) == L"off") return capture::AudioDevice{Mode::Off};
        if (lower(value) == L"default") return capture::AudioDevice{Mode::SystemDefault};
        return capture::AudioDevice{Mode::Fixed, value, get(nameKey)};
    };
    s.desktopAudio = device(L"audio/desktop_device", L"audio/desktop_device_name", L"audio/desktop", s.desktopAudio);
    s.microphone = device(L"audio/microphone_device", L"audio/microphone_device_name", L"audio/microphone", s.microphone);
    s.levels.desktopPercent = parseNumber(get(L"audio/desktop_volume"), 0, 200).value_or(s.levels.desktopPercent);
    s.levels.microphonePercent = parseNumber(get(L"audio/mic_volume"), 0, 200).value_or(s.levels.microphonePercent);
    s.hideDiscord = toggle(L"audio/hide_discord", s.hideDiscord);
    s.chimePercent = parseNumber(get(L"audio/chime_volume"), 0, 100).value_or(s.chimePercent);
    auto& f = s.micFilters;
    f.noiseSuppression = toggle(L"audio/noise_suppression", f.noiseSuppression);
    f.noiseGate = toggle(L"audio/noise_gate", f.noiseGate);
    // The gate values only make sense together: take all five or none.
    const auto open = parseNumber(get(L"audio/gate_open_db"), gateLevelMinDb, gateLevelMaxDb);
    const auto close = parseNumber(get(L"audio/gate_close_db"), gateLevelMinDb, gateLevelMaxDb);
    const auto attack = parseNumber(get(L"audio/gate_attack_ms"), 0, gateTimeMaxMs);
    const auto hold = parseNumber(get(L"audio/gate_hold_ms"), 0, gateTimeMaxMs);
    const auto release = parseNumber(get(L"audio/gate_release_ms"), 0, gateTimeMaxMs);
    if (open && close && attack && hold && release && *close <= *open) {
        f.gateOpenDb = static_cast<float>(*open);
        f.gateCloseDb = static_cast<float>(*close);
        f.gateAttackMs = *attack;
        f.gateHoldMs = *hold;
        f.gateReleaseMs = *release;
    }
    s.clipFolder = get(L"storage/clip_folder");
    s.maxStorageGb = clampStorageGb(get(L"storage/max_storage_gb"));
    {
        const auto whenFull = lower(get(L"storage/when_full"));
        for (int i = 0; i < 3; ++i) {
            if (whenFull != storageFullNames[i]) continue;
            s.storageFull = static_cast<StorageFull>(i);
            s.storageFullChosen = true;
        }
    }
    s.smallTiles = lower(get(L"library/density")) == L"compact";
    s.groupFolders = toggle(L"library/folders", false);
    s.askBeforeBinning = toggle(L"library/ask_before_binning", true);
    s.playerVolume = parseNumber(get(L"library/player_volume"), 0, 100).value_or(s.playerVolume);
    s.compressCopyAfterTrim = toggle(L"library/compress_copy_after_trim", false);
    s.closeToTray = toggle(L"window/close_to_tray", s.closeToTray);
    s.startWithWindows = toggle(L"window/start_with_windows", s.startWithWindows);
    s.startMinimised = toggle(L"window/start_minimised", s.startMinimised);
    s.sidebarCollapsed = lower(get(L"window/sidebar")) == L"collapsed";
    s.diagnosticLogging = toggle(L"diagnostics/logging", false);
    s.updateCheck = static_cast<UpdateChoice>(pick(get(L"updates/check"), updateChoiceNames, static_cast<int>(s.updateCheck)));
    s.lastUpdateCheck = parseUnixSeconds(get(L"updates/last_check"));
    s.availableUpdateTag = get(L"updates/available_tag");
    s.availableUpdateUrl = get(L"updates/available_url");
    return s;
}

} // namespace refarter
