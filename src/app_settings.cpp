// App: settings and shortcuts. Loading and saving refarter.ini, the start-with-Windows switch, and the save shortcut.
#include "app_internal.h"
#include "config.h"
#include "message_box.h"
#include "startup.h"
#include "text.h"
#include "window_target.h"
#include "capture/clip_folder.h"
#include "diagnostic_log.h"
#include "version.h"
#include "update_check.h"

#include <shellapi.h>

namespace refarter {

void App::loadSettings() {
    config_.load(); // The only read of the settings file; controls start with its values.
    const auto saved = readSettings(config_.values());
    stored_ = settingEntries(saved);
    state_.replaySeconds = saved.replaySeconds;
    state_.smallTiles = saved.smallTiles;
    state_.groupFolders = saved.groupFolders;
    state_.askBeforeBinning = saved.askBeforeBinning;
    video_ = saved.video;
    audio_.desktop = saved.desktopAudio;
    audio_.microphone = saved.microphone;
    audio_.micFilters = saved.micFilters;
    audio_.levels = saved.levels;
    audio_.hideDiscord = saved.hideDiscord;
    state_.maxStorageGb = saved.maxStorageGb;
    state_.storageFull = saved.storageFull;
    state_.storageFullChosen = saved.storageFullChosen;
    state_.closeToTray = saved.closeToTray;
    state_.startMinimised = saved.startMinimised;
    state_.sidebarCollapsed = saved.sidebarCollapsed;
    state_.updateCheck = saved.updateCheck;
    state_.lastUpdateCheck = saved.lastUpdateCheck;
    if (const auto known = update::cachedRelease(saved.availableUpdateTag, saved.availableUpdateUrl, L"" REFARTER_VERSION_FULL_STRING)) {
        updateTag_ = known->tag;
        updateUrl_ = known->url;
        updateNote_ = L"version " + update::displayVersion(updateTag_) + L" is available";
    }
    state_.compressCopyAfterTrim = saved.compressCopyAfterTrim;
    player_.setCompressCopyDefault(saved.compressCopyAfterTrim);
    configureDiagnostics(saved.diagnosticLogging);
    playerVolume_ = saved.playerVolume;
    autoCaptureOn_ = saved.autoCapture;
    autoCapturePrompt_ = saved.autoCapturePrompt;
    // An unreadable list is reported in its menu and never overwritten.
    if (auto list = readAutoCapture(autoCapturePath())) autoCapture_ = std::move(*list);
    else autoCaptureReadable_ = false;
    chimes_.prepare(saved.chimePercent);
    // A folder that has gone missing is created, or reported, when replay starts.
    clipFolder_ = saved.clipFolder.empty() ? capture::defaultClipFolder() : capture::normalisedFolder(saved.clipFolder); // No trailing separator: clips are matched to this folder by path.
    // The monitor chosen last time is selected again; replay still waits for the user.
    sourceMonitor_ = saved.sourceMonitor;
    const auto displays = displayMonitors();
    for (size_t i = 0; i < displays.size() && !sourceMonitor_.empty(); ++i) {
        if (monitorDevice(displays[i]) != sourceMonitor_) continue;
        target_ = {{nullptr, displays[i]}, 0, {}, L"monitor " + std::to_wstring(i + 1), L"whole monitor"};
        setStatus(L"monitor from last time selected · enable replay when ready", true);
    }
    if (!sourceMonitor_.empty() && !target_.source.monitor) setStatus(L"the monitor from last time is not connected · choose a source");
}

bool App::configureDiagnostics(bool enabled) {
    const auto config = configPath();
    if (!diagnosticLog().configure(enabled, config.empty() ? std::filesystem::path{} : config.parent_path() / L"logs",
            REFARTER_VERSION_FULL_STRING)) return false;
    state_.diagnosticLogging = enabled;
    if (enabled) diagnosticLog().write(DiagnosticEvent::SessionStarted);
    return true;
}

bool App::toggleStartWithWindows() {
    // Read on this explicit action; do not recreate an externally removed entry.
    refreshStartup();
    if (startupReadFailed_) {
        messageBox(window_, L"windows startup settings could not be read. nothing was changed.", L"refarter", MB_OK | MB_ICONERROR);
        return false;
    }
    const bool enable = !state_.startWithWindows;
    const auto path = executablePath();
    const auto entry = startupCommand(path);
    const auto previous = readStartup();
    if (previous.error != ERROR_SUCCESS) return false;
    const auto previousPreference = config_.get(L"window", L"start_with_windows", L"off");
    if (enable && !entry) {
        messageBox(window_, L"this app path is too long for windows startup. move or install refarter in a shorter path.", L"refarter", MB_OK | MB_ICONERROR);
        return false;
    }
    if (writeStartup(enable ? entry : std::nullopt) != ERROR_SUCCESS) {
        messageBox(window_, L"windows could not update startup. your startup setting was not changed.", L"refarter", MB_OK | MB_ICONERROR);
        return false;
    }
    // Keep the Windows entry and INI in step; roll back registration on write failure.
    if (!config_.set(L"window", L"start_with_windows", enable ? L"on" : L"off")) {
        const auto rollback = writeStartup(previous.command.empty() ? std::nullopt : std::optional(previous.command));
        config_.set(L"window", L"start_with_windows", previousPreference);
        refreshStartup();
        refreshControls();
        messageBox(window_, rollback == ERROR_SUCCESS ?
            L"your preference could not be saved. the startup change was undone." :
            L"your preference could not be saved or startup restored. check windows startup apps.", L"refarter", MB_OK | MB_ICONERROR);
        return false;
    }
    refreshStartup();
    return true;
}

void App::refreshStartup() {
    const auto registration = readStartup();
    startupReadFailed_ = registration.error != ERROR_SUCCESS;
    state_.startWithWindows = !registration.command.empty();
    const auto current = startupCommand(executablePath());
    startupOtherCopy_ = state_.startWithWindows && (!current || _wcsicmp(current->c_str(), registration.command.c_str()) != 0);
}

UserSettings App::settings() const {
    UserSettings s;
    s.replaySeconds = state_.replaySeconds;
    s.video = video_;
    s.desktopAudio = audio_.desktop;
    s.hideDiscord = audio_.hideDiscord;
    s.maxStorageGb = state_.maxStorageGb;
    s.storageFull = state_.storageFull;
    s.storageFullChosen = state_.storageFullChosen;
    s.microphone = audio_.microphone;
    s.micFilters = audio_.micFilters;
    if (clipFolder_ != capture::defaultClipFolder()) s.clipFolder = clipFolder_;
    s.smallTiles = state_.smallTiles;
    s.groupFolders = state_.groupFolders;
    s.askBeforeBinning = state_.askBeforeBinning;
    s.sourceMonitor = sourceMonitor_;
    s.levels = audio_.levels;
    s.closeToTray = state_.closeToTray;
    s.startWithWindows = state_.startWithWindows;
    s.startMinimised = state_.startMinimised;
    s.sidebarCollapsed = state_.sidebarCollapsed;
    s.diagnosticLogging = state_.diagnosticLogging;
    s.updateCheck = state_.updateCheck;
    s.lastUpdateCheck = state_.lastUpdateCheck;
    s.availableUpdateTag = updateTag_;
    s.availableUpdateUrl = updateUrl_;
    s.compressCopyAfterTrim = state_.compressCopyAfterTrim;
    s.chimePercent = chimes_.percent();
    s.playerVolume = playerVolume_;
    s.autoCapture = autoCaptureOn_;
    s.autoCapturePrompt = autoCapturePrompt_;
    return s;
}

void App::saveSettings() {
    // Only changed values are written, so a click rewrites one line at most.
    auto current = settingEntries(settings());
    // An entry whose write failed keeps its old stored value, so the next save tries it again.
    for (size_t i = 0; i < current.size(); ++i) {
        if (current[i].value != stored_[i].value && config_.set(current[i].section, current[i].key, current[i].value))
            stored_[i] = std::move(current[i]);
    }
}

bool App::registerHotkey(int shortcut, Hotkey hotkey) {
    hotkeyRegistered_[shortcut] = RegisterHotKey(window_, shortcut + 1, hotkey.modifiers | MOD_NOREPEAT, hotkey.key) != FALSE;
    return hotkeyRegistered_[shortcut];
}

// A shortcut another app held (at start-up, say, while sign-in was still starting its own) is taken when it is free
// again. Called on activation only: no timer, no polling. A combination held by the other shortcut keeps its note.
void App::retryHotkeys() {
    if (assigning_ || closing_) return;
    for (int shortcut = 0; shortcut < shortcutCount; ++shortcut) {
        const int other = shortcut == saveShortcut ? screenshotShortcut : saveShortcut;
        // The same combination as the other shortcut: the save shortcut owns it (it is tried first) and the screenshot keeps its clash note.
        const bool clash = hotkeys_[shortcut] == hotkeys_[other];
        if (hotkeyRegistered_[shortcut] || (clash && (hotkeyRegistered_[other] || shortcut == screenshotShortcut))) continue;
        if (registerHotkey(shortcut, hotkeys_[shortcut])) hotkeyNotes_[shortcut].clear();
    }
}

// What a shortcut says when its combination is held by the other one.
std::wstring App::clashNote(int shortcut) const {
    return describe(hotkeys_[shortcut]) + (shortcut == screenshotShortcut ? L" is the save shortcut · assign another" : L" is the screenshot shortcut · assign another");
}

std::wstring App::describe(Hotkey hotkey) const {
    if (!keyName(hotkey.key).empty()) return hotkeyText(hotkey);
    // Punctuation keys differ between layouts; let the active layout name them.
    wchar_t name[32]{};
    const auto scan = MapVirtualKeyW(hotkey.key, MAPVK_VK_TO_VSC);
    if (!scan || !GetKeyNameTextW(static_cast<LONG>(scan << 16), name, static_cast<int>(std::size(name)))) return hotkeyText(hotkey);
    return hotkeyText(hotkey, lower(name));
}

void App::startAssigning(int shortcut) {
    // Release the current shortcut so pressing it again reaches the window as keys. The other one stays registered.
    if (hotkeyRegistered_[shortcut]) UnregisterHotKey(window_, shortcut + 1);
    hotkeyRegistered_[shortcut] = false;
    assigning_ = true;
    assigningShortcut_ = shortcut;
    rawHotkey_.configure(window_, {}, false);
    hotkeyNotes_[shortcut].clear();
    heldModifiers_ = currentModifiers();
    SetFocus(control(shortcut == saveShortcut ? AssignHotkey : AssignScreenshot));
    refreshAssigning();
}

UINT App::currentModifiers() const {
    UINT modifiers = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) modifiers |= MOD_WIN;
    return modifiers;
}

void App::refreshAssigning() {
    refreshControls(); // The button's text.
    refreshPages();    // The row's note, rebuilt from the state; the window repaints.
    InvalidateRect(control(AssignHotkey), nullptr, FALSE);
    InvalidateRect(control(AssignScreenshot), nullptr, FALSE);
}

// A modifier went down or up while assigning: show what is held so far.
void App::modifiersChanged() {
    if (!assigning_) return;
    const UINT held = currentModifiers();
    if (held == heldModifiers_) return;
    heldModifiers_ = held;
    hotkeyNotes_[assigningShortcut_].clear(); // An earlier problem no longer describes what is held.
    refreshAssigning();
}

void App::assignKey(UINT key) {
    if (isModifierKey(key)) return modifiersChanged(); // Wait for the key that completes the combination.
    const UINT modifiers = currentModifiers();
    heldModifiers_ = modifiers;
    releaseKey_ = key;
    auto& note = hotkeyNotes_[assigningShortcut_];
    if (!modifiers && key == VK_ESCAPE) return finishAssigning(std::nullopt);
    if (!modifiers && key == VK_BACK) return finishAssigning(assigningShortcut_ == saveShortcut ? defaultHotkey : defaultScreenshotHotkey);
    const Hotkey pressed{modifiers, key};
    switch (checkHotkey(pressed)) {
    case HotkeyProblem::None: return finishAssigning(pressed);
    case HotkeyProblem::NeedsModifier: note = describe(pressed) + L" needs ctrl, alt or win · try another"; break;
    case HotkeyProblem::Reserved: note = describe(pressed) + L" is reserved by windows · try another"; break;
    default: note = L"that key cannot be a shortcut · try another"; break;
    }
    refreshAssigning();
}

void App::finishAssigning(std::optional<Hotkey> hotkey) {
    if (!assigning_) return;
    assigning_ = false;
    heldModifiers_ = 0;
    const int shortcut = assigningShortcut_, other = shortcut == saveShortcut ? screenshotShortcut : saveShortcut;
    auto& current = hotkeys_[shortcut];
    auto& note = hotkeyNotes_[shortcut];
    note.clear();
    if (closing_) return; // quit() releases the shortcuts; do not take one again.
    if (hotkey && *hotkey != current && *hotkey == hotkeys_[other]) {
        // Backspace's default can be what the other shortcut already uses.
        note = describe(*hotkey) + (other == saveShortcut ? L" is the save shortcut · kept " : L" is the screenshot shortcut · kept ") + describe(current);
        registerHotkey(shortcut, current);
    } else if (hotkey && *hotkey != current) {
        if (registerHotkey(shortcut, *hotkey)) {
            const Hotkey released = current;
            current = *hotkey;
            // Layout-independent text, so the file reads the same on every keyboard.
            config_.set(L"shortcuts", shortcut == saveShortcut ? L"save_replay" : L"screenshot", hotkeyText(current));
            // The other shortcut may have been left unregistered because it matched the combination just released.
            if (!hotkeyRegistered_[other] && hotkeys_[other] == released) {
                hotkeyNotes_[other].clear();
                if (!registerHotkey(other, hotkeys_[other])) hotkeyNotes_[other] = describe(hotkeys_[other]) + L" is in use by another app · assign another";
            }
        } else {
            note = describe(*hotkey) + L" is in use by another app · kept " + describe(current);
            registerHotkey(shortcut, current);
        }
    } else if (current == hotkeys_[other] && hotkeyRegistered_[other]) {
        note = clashNote(shortcut); // Still held by the other shortcut; registering again would only blame another app.
    } else {
        registerHotkey(shortcut, current);
    }
    refreshAssigning();
}

} // namespace refarter
