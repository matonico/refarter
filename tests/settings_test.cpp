#include "config.h"
#include "hotkey.h"
#include "settings.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    using namespace refarter;
    // What the app writes, as the INI parser reads it back.
    const auto roundTrip = [](const UserSettings& s) {
        std::wstring text;
        for (const auto& entry : settingEntries(s)) text += L"[" + std::wstring(entry.section) + L"]\n" + entry.key + L" = " + entry.value + L"\n";
        return readSettings(parseConfig(text));
    };
    const auto same = [](const UserSettings& a, const UserSettings& b) {
        const auto x = settingEntries(a), y = settingEntries(b);
        for (size_t i = 0; i < x.size(); ++i) if (x[i].value != y[i].value) return false;
        return true;
    };

    check(same(readSettings({}), UserSettings{}), "missing file gives defaults");
    const auto fresh = readSettings({}).micFilters;
    check(!fresh.noiseSuppression && !fresh.noiseGate && fresh.gateOpenDb == -26.f && fresh.gateCloseDb == -32.f,
          "mic filters start off, with OBS's gate levels");
    check(same(roundTrip(UserSettings{}), UserSettings{}), "defaults round trip");

    UserSettings changed;
    changed.sourceMonitor = L"\\\\.\\DISPLAY2";
    changed.replaySeconds = 45;
    changed.video = {capture::Resolution::P1440, 30, 120, 45, capture::Quality::Custom, capture::Codec::AV1};
    changed.desktopAudio = {};
    changed.microphone = {capture::AudioDevice::Mode::Fixed, L"{0.0.1.00000000}.{6b1c2a3d-1111-4c2b-9a8b-0123456789ab}",
                          L"Headset Microphone (USB Headset)"};
    changed.micFilters = {false, false, -40.f, -55.f, 10, 300, 90};
    changed.levels = {150, 0};
    changed.hideDiscord = true;
    changed.maxStorageGb = 250;
    changed.storageFull = StorageFull::Bin;
    changed.storageFullChosen = true;
    changed.chimePercent = 40;
    changed.clipFolder = L"D:\\Clips \u00fcber alles";
    changed.smallTiles = true;
    changed.groupFolders = true;
    changed.askBeforeBinning = false;
    changed.playerVolume = 35;
    changed.compressCopyAfterTrim = true;
    changed.closeToTray = true;
    changed.startWithWindows = true;
    changed.startMinimised = false;
    changed.sidebarCollapsed = true;
    changed.diagnosticLogging = true;
    changed.autoCapture = false;
    changed.autoCapturePrompt = false;
    const auto read = roundTrip(changed);
    check(same(read, changed), "every setting round trips");
    check(read.microphone == changed.microphone, "fixed device id and name round trip exactly");
    UserSettings standard;
    standard.microphone = {capture::AudioDevice::Mode::SystemDefault};
    check(roundTrip(standard).microphone.mode == capture::AudioDevice::Mode::SystemDefault &&
          roundTrip(standard).desktopAudio.mode == capture::AudioDevice::Mode::SystemDefault, "system default round trips");

    // Files from before device choice: on follows the default, off stays off.
    using Mode = capture::AudioDevice::Mode;
    const auto legacy = readSettings(parseConfig(L"[audio]\ndesktop = off\nmicrophone = on\n"));
    check(legacy.desktopAudio.mode == Mode::Off && legacy.microphone.mode == Mode::SystemDefault, "legacy toggles migrate");
    const auto newer = readSettings(parseConfig(
        L"[audio]\ndesktop = off\nmicrophone = on\ndesktop_device = DEFAULT\nmicrophone_device = off\n"));
    check(newer.desktopAudio.mode == Mode::SystemDefault && newer.microphone.mode == Mode::Off, "device keys win over legacy toggles");
    const auto nameless = readSettings(parseConfig(L"[audio]\ndesktop_device = {0.0.0.00000000}.{abc}\n"));
    check(nameless.desktopAudio.mode == Mode::Fixed && nameless.desktopAudio.id == L"{0.0.0.00000000}.{abc}" &&
          nameless.desktopAudio.name.empty(), "a fixed id without a name stays fixed");
    check(read.video.replaySeconds == 45, "recorder replay length follows");

    const auto bad = readSettings(parseConfig(
        L"[recording]\nreplay_seconds = 2\n"
        L"[video]\nresolution = 8k\nfps = 45\nquality = HIGH\nbitrate_mbps = 900\ncodec = vp9\n"
        L"[audio]\ndesktop = maybe\nmicrophone = ON\ndesktop_volume = 300\nmic_volume = 0\nchime_volume = 101\ngate_open_db = -60\ngate_close_db = -50\n"
        L"gate_attack_ms = 1\ngate_hold_ms = 1\ngate_release_ms = 1\n"
        L"[library]\ndensity = tiny\n"
        L"[window]\nsidebar = narrow\n"));
    const UserSettings defaults;
    check(bad.replaySeconds == defaults.replaySeconds, "replay length out of range ignored");
    check(bad.video.resolution == defaults.video.resolution, "unknown resolution ignored");
    check(bad.video.fps == defaults.video.fps, "fps other than 30/60 ignored");
    check(bad.video.quality == capture::Quality::High, "names are case-insensitive");
    check(bad.video.bitrateMbps == defaults.video.bitrateMbps, "bitrate out of range ignored");
    check(bad.video.codec == defaults.video.codec, "unknown codec ignored");
    check(bad.desktopAudio == defaults.desktopAudio, "unreadable toggle ignored");
    check(bad.microphone.mode == capture::AudioDevice::Mode::SystemDefault, "toggle is case-insensitive");
    check(bad.levels.desktopPercent == 100 && bad.levels.microphonePercent == 0, "volume above 200% ignored, 0% kept");
    check(!bad.closeToTray, "close to tray is off by default");
    check(!bad.startWithWindows, "old settings leave startup off");
    check(defaults.startMinimised && bad.startMinimised, "fresh and old settings start minimised by default");
    check(!readSettings(parseConfig(L"[window]\nstart_minimised = off\n")).startMinimised, "visible sign-in launch is read");
    check(readSettings(parseConfig(L"[window]\nstart_minimised = invalid\n")).startMinimised, "invalid startup visibility keeps the tray default");
    check(!bad.diagnosticLogging, "old settings leave diagnostic logging off");
    check(bad.askBeforeBinning, "old settings still ask before binning");
    check(!bad.compressCopyAfterTrim, "old settings leave compress & copy off");
    check(bad.autoCapture && bad.autoCapturePrompt, "auto-capture switches default on; the list itself starts empty");
    check(bad.chimePercent == 100, "chime above 100% ignored");
    check(bad.micFilters.gateOpenDb == defaults.micFilters.gateOpenDb &&
          bad.micFilters.gateAttackMs == defaults.micFilters.gateAttackMs, "close above open drops the whole gate set");
    check(!bad.smallTiles, "unknown density is large tiles");
    check(!bad.sidebarCollapsed, "unknown sidebar value is expanded");
    const auto removed = readSettings(parseConfig(L"[audio]\ngame_track = 0\nhide_discord = on\n"));
    check(removed.hideDiscord, "the removed game_track key is ignored and its neighbours still load");
    check(defaults.maxStorageGb == 0 && defaults.storageFull == StorageFull::Warn && !defaults.storageFullChosen, "storage defaults: no limit, warn, not chosen");
    const auto storage = readSettings(parseConfig(L"[storage]\nmax_storage_gb = 100000\nwhen_full = delete\n"));
    check(storage.maxStorageGb == 99999 && storage.storageFull == StorageFull::Warn && !storage.storageFullChosen, "an over-large limit is clamped and an unknown behaviour is ignored");
    check(readSettings(parseConfig(L"[storage]\nmax_storage_gb = 99999999999\n")).maxStorageGb == 99999 &&
          readSettings(parseConfig(L"[storage]\nmax_storage_gb = abc\n")).maxStorageGb == 0, "a huge limit clamps, text is no limit");
    const auto chosen = readSettings(parseConfig(L"[storage]\nmax_storage_gb = 80\nwhen_full = REFUSE\n"));
    check(chosen.maxStorageGb == 80 && chosen.storageFull == StorageFull::Refuse && chosen.storageFullChosen, "limit and a chosen behaviour are read");
    check(!roundTrip(UserSettings{}).storageFullChosen && roundTrip(chosen).storageFullChosen, "a missing choice stays missing through the file");
    check(readSettings(parseConfig(L"[storage]\nmax_storage_gb =\n")).maxStorageGb == 0, "an empty limit is no limit");

    // [updates]: ask by default (a missing or unknown value never switches the check on), and a time that is not unix seconds is "never".
    check(defaults.updateCheck == UpdateChoice::Ask && defaults.lastUpdateCheck == 0, "update check defaults: ask, never checked");
    check(readSettings(parseConfig(L"[updates]\ncheck = maybe\nlast_check = soon\n")).updateCheck == UpdateChoice::Ask, "an unknown update choice stays ask");
    check(readSettings(parseConfig(L"[updates]\nlast_check = soon\n")).lastUpdateCheck == 0 &&
          readSettings(parseConfig(L"[updates]\nlast_check = -5\n")).lastUpdateCheck == 0 &&
          readSettings(parseConfig(L"[updates]\nlast_check = 99999999999999999999\n")).lastUpdateCheck == 0, "an unreadable last check is never");
    const auto updates = readSettings(parseConfig(L"[updates]\ncheck = ON\nlast_check = 1790000000\n"));
    check(updates.updateCheck == UpdateChoice::On && updates.lastUpdateCheck == 1790000000LL, "update choice and time are read");
    UserSettings declined;
    declined.updateCheck = UpdateChoice::Off;
    declined.lastUpdateCheck = 1790000123LL;
    check(roundTrip(declined).updateCheck == UpdateChoice::Off && roundTrip(declined).lastUpdateCheck == 1790000123LL, "update choice and time round trip");
    changed.availableUpdateTag = L"v2.0.0";
    changed.availableUpdateUrl = L"https://github.com/matonico/Refarter/releases/tag/v2.0.0";
    changed.updateCheck = UpdateChoice::On;
    changed.lastUpdateCheck = 1790000456LL;
    check(same(roundTrip(changed), changed), "every setting round trips, updates included");

    // The commented template that first launch writes (config/refarter.ini, embedded in the exe) must
    // spell exactly the defaults the code uses, with no missing, stray or misspelt keys.
    {
        std::ifstream file(std::string(REFARTER_CONFIG_DIR) + "/refarter.ini", std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        check(!bytes.empty(), "the first-launch template can be read");
        bool ascii = true;
        for (const unsigned char c : bytes) ascii = ascii && c < 0x80;
        check(ascii, "the template is plain ASCII, so widening it byte by byte is exact");
        const auto template_ = parseConfig(std::wstring(bytes.begin(), bytes.end()));
        std::set<std::wstring> known;
        for (const auto& entry : settingEntries(UserSettings{})) {
            const auto name = lower(std::wstring(entry.section) + L"/" + entry.key);
            known.insert(name);
            const auto found = template_.find(name);
            if (found == template_.end()) {
                std::wcerr << L"template is missing " << name << L'\n';
                check(false, "every setting the app writes is documented in the template");
            } else if (found->second != entry.value) {
                std::wcerr << name << L" = " << found->second << L" in the template, " << entry.value << L" in the code\n";
                check(false, "template value equals the code default");
            }
        }
        for (const auto& [name, value] : template_) {
            if (name.rfind(L"shortcuts/", 0) == 0 || known.count(name)) continue;
            std::wcerr << L"template has an unknown key " << name << L'\n';
            check(false, "the template holds no key the app does not read");
        }
        check(same(readSettings(template_), UserSettings{}), "reading the template gives the defaults");
        const auto save = parseHotkey(template_.count(L"shortcuts/save_replay") ? template_.at(L"shortcuts/save_replay") : L"");
        const auto screenshot = parseHotkey(template_.count(L"shortcuts/screenshot") ? template_.at(L"shortcuts/screenshot") : L"");
        check(save && *save == defaultHotkey, "template save shortcut is the default");
        check(screenshot && *screenshot == defaultScreenshotHotkey, "template screenshot shortcut is the default");
    }
    return failures ? 1 : 0;
}
