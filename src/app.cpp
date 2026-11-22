#include "app_internal.h"
#include "resources.h"
#include "capture/audio_capture.h"
#include "capture/clip_folder.h"
#include "capture/discord_audio.h"
#include "player/export_plan.h"
#include "chime.h"
#include "config.h"
#include "theme.h"
#include "message_box.h"
#include "save_errors.h"
#include "source_names.h"
#include "text.h"
#include "startup.h"
#include "jump_list.h"
#include "local_file.h"
#include "path_list.h"
#include "window_target.h"
#include "diagnostic_log.h"

#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <shellapi.h>

namespace refarter {
namespace {
constexpr int wheelStep = 48; // Logical pixels scrolled per wheel notch.
constexpr UINT recordingEvent = WM_APP + 1;
constexpr UINT trayEvent = WM_APP + 2;
constexpr size_t maxQueuedErrors = 4; // Error messages kept for the next activation; repeats of one are dropped.

constexpr UINT afterToggle = WM_APP + 5; // Saves and refreshes after a switch has already repainted.
constexpr UINT meterSync = WM_APP + 6;   // Rechecks the meters once a show or hide has taken effect.
constexpr int initialClientWidth = 1120, initialClientHeight = 760;

// Whether this process runs elevated (as administrator); false when the token cannot be read.
bool runsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool read = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
    CloseHandle(token);
    return read && elevation.TokenIsElevated != 0;
}

} // namespace

bool App::addTrayIcon() { return tray_.present() || tray_.add(smallIcon_, trayTip()); }

void App::removeTrayIcon() { tray_.remove(); }

void App::restoreWindow() {
    const bool firstStartupOpen = startupTray_;
    startupTray_ = false;
    if (!state_.closeToTray) removeTrayIcon();
    if (firstStartupOpen) registerJumpList(executablePath()); // Left out of the hidden sign-in start.
    if (firstStartupOpen) {
        // Sign-in may change the hidden window's bounds while displays settle.
        // Apply the ordinary size once; later tray restores keep the user's size.
        const UINT dpi = GetDpiForWindow(window_);
        RECT bounds{0, 0, MulDiv(initialClientWidth, dpi, 96), MulDiv(initialClientHeight, dpi, 96)};
        AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) {
            const auto& work = monitor.rcWork;
            const int width = std::min(bounds.right - bounds.left, work.right - work.left);
            const int height = std::min(bounds.bottom - bounds.top, work.bottom - work.top);
            SetWindowPos(window_, nullptr, work.left + (work.right - work.left - width) / 2,
                work.top + (work.bottom - work.top - height) / 2, width, height,
                SWP_NOACTIVATE | SWP_NOZORDER);
        }
    }
    ShowWindow(window_, firstStartupOpen || IsIconic(window_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(window_);
    if (welcome_ && firstStartupOpen) {
        // A hidden start never focused it. Start waits while a folder check runs: then skip, so Enter still acts.
        SetFocus(control(IsWindowEnabled(control(WelcomeStart)) ? WelcomeStart : WelcomeSkip));
    }
    if (notesDue_) PostMessageW(window_, notesDue, 0, 0); // The first time the window shows after an update, from a hidden sign-in start too.
    if (state_.updateCheck == UpdateChoice::Ask) PostMessageW(window_, updateQuestion, 0, 0); // Asked now if this brought Refarter to the front.
    if (!queuedErrors_.empty()) PostMessageW(window_, errorDialogs, 0, 0); // Likewise the errors that waited for it.
    if (!clipGrid_.scanned()) clipGrid_.scan(clipFolder_);
}

void App::quit() {
    if (closing_) return;
    // A value typed for a game and not yet left, as saveSettings keeps all games' one. A failed save is not waited on
    // (the window must not freeze): the status says so as Refarter closes, and the file keeps its old values. Quitting
    // is never refused.
    const bool unsaved = !commitScopeFields();
    saveSettings();
    closing_ = true;
    syncMeters(); // Closing: the timer and any meter-only capture end.
    compression_.cancel();
    updates_.cancel(); // A check in flight ends on its own and posts nothing.
    const bool downloading = updateStage_ == UpdateStage::Downloading;
    cancelDownload(); // So does a download (its partial file goes now, or with the next download).
    // The installer of an update that relaunched this Refarter was still running at start, so start-up could not
    // delete it: it goes now. Not while a cancelled download may still write there, or while setup runs from it.
    if (installed_ && !downloading && !installing()) {
        dropSetup();
        std::error_code noFolder;
        std::filesystem::remove_all(localAppDataFile(L"updates"), noFolder);
    }
    auto_.unwatchGameExit();
    rawHotkey_.configure(window_, {}, false);
    setStatus(std::wstring(L"finishing pending saves and closing…") + (unsaved ? L" · game settings could not be saved" : L""));
    assigning_ = false;
    for (int shortcut = 0; shortcut < shortcutCount; ++shortcut) {
        if (hotkeyRegistered_[shortcut]) UnregisterHotKey(window_, shortcut + 1);
        hotkeyRegistered_[shortcut] = false;
    }
    removeTrayIcon();
    trayFlyout_.hide();
    auto_.updateHook(); // closing_ removes the hook and the card.
    refreshControls();
    InvalidateRect(window_, nullptr, FALSE);
    recorder_->close(); // The worker finishes queued saves before teardown.
}

bool App::toggled(int id) const {
    switch (id) {
    case Suppression: return audio_.micFilters.noiseSuppression;
    case Gate: return audio_.micFilters.noiseGate;
    case RecordDiscord: return !audio_.hideDiscord; // Shown as "record discord".
    case CloseToTray: return state_.closeToTray;
    case StartWithWindows: return state_.startWithWindows;
    case StartMinimised: return state_.startMinimised;
    case SignInReplay: return state_.signInReplay;
    case DiagnosticLogging: return state_.diagnosticLogging;
    case CompressCopyAfterTrim: return state_.compressCopyAfterTrim;
    case GameColours: return state_.gameColours;
    case ShortGameNames: return state_.shortGameNames;
    case AskBeforeBinning: return state_.askBeforeBinning;
    case AutoCaptureStart: return autoCaptureOn_;
    case AutoCaptureOffers: return autoCapturePrompt_;
    case UpdateCheckSwitch: return state_.updateCheck == UpdateChoice::On; // "ask" counts as off until answered.
    case WelcomeUpdates: return welcomeUpdates_;
    case WelcomeStartup: return welcomeStartup_;
    case WelcomeTray: return welcomeTray_;
    default: return false;
    }
}

void App::navigate(Page page) {
    if (welcome_) return; // The jump list or tray must not move focus off the welcome screen to a disabled button.
    if (page != Page::Library && playerOpen_) closePlayer(); // Leaving the library stops playback.
    // Move focus before hiding page controls so keyboard traversal remains usable.
    state_.page = page;
    if (IsWindowVisible(window_)) SetFocus(control(page == Page::Library ? Library : page == Page::Recording ? Recording : Settings));
    if (page == Page::Recording) { probeCodecs(); checkDiscordOutput(); } // Codec and Discord rows live there.
    if (page == Page::Library) clipGrid_.scan(clipFolder_); // Picks up clips added or deleted elsewhere.
    layout(); // Repaints the whole window and its children.
}

void App::command(int id) {
    if (closing_) return;
    const float before = contains(toggleIds, id) ? switchPosition(id) : 0.f; // Where a switch is drawn now.
    bool redrawn = false; // The page was already repainted.
    switch (id) {
    case Library:
        if (playerOpen_) closePlayer();
        navigate(Page::Library);
        redrawn = true;
        break;
    case Recording: navigate(Page::Recording); redrawn = true; break;
    case Settings: navigate(Page::Settings); redrawn = true; break;
    case ShowNotes: showNotes(); redrawn = true; break;
    case AllClips: case Favourites: case Compressed:
        state_.clipView = id == AllClips ? ClipView::All : id == Favourites ? ClipView::Favourites : ClipView::Compressed;
        clipGrid_.setView(state_.clipView);
        placeControls(); // The chosen tab is taller; the empty-state text follows the view.
        redrawn = true;
        break;
    // The replay length of the scope the page edits: all games, or the chosen game.
    case ThirtySeconds: setScopeValue(ReplayReset, 30); break;
    case OneMinute: setScopeValue(ReplayReset, 60); break;
    case ThreeMinutes: setScopeValue(ReplayReset, 180); break;
    case FiveMinutes: setScopeValue(ReplayReset, 300); break;
    case SettingsFor: chooseSettingsFor(); break;
    case UseDefaults: useDefaults(); break;
    case RenameGame: renameGame(settingsFor_, knownGameName(settingsFor_, settingsForLabel_)); break;
    case ReplayReset: case ResolutionReset: case FrameRateReset: case QualityReset: case BitrateReset: case CodecReset:
        clearScopeValue(id);
        break;
    case LargeTiles: case SmallTiles:
        state_.smallTiles = id == SmallTiles;
        clipGrid_.setStyle(dpi_, fontOf(Font::Body), fontOf(Font::Small), state_.smallTiles); // Repaints the tiles.
        InvalidateRect(control(LargeTiles), nullptr, FALSE);
        InvalidateRect(control(SmallTiles), nullptr, FALSE);
        redrawn = true;
        break;
    case GroupFolders:
        state_.groupFolders = !state_.groupFolders;
        clipGrid_.setGrouped(state_.groupFolders);
        InvalidateRect(control(GroupFolders), nullptr, FALSE);
        redrawn = true;
        break;
    case CloseToTray:
        // Saved in refarter.ini below, like every other setting.
        if (!state_.closeToTray && !addTrayIcon()) {
            messageBox(window_, L"windows could not create the tray icon. refarter will stay open.", L"refarter", MB_OK | MB_ICONERROR);
            return;
        }
        state_.closeToTray = !state_.closeToTray;
        if (!state_.closeToTray && !startupTray_) removeTrayIcon();
        break;
    case StartWithWindows:
        if (!toggleStartWithWindows()) return;
        if (!state_.startWithWindows) cancelSignInReplay();
        break;
    case StartMinimised:
        if (!state_.startWithWindows || startupReadFailed_) return;
        state_.startMinimised = !state_.startMinimised;
        break;
    case SignInReplay:
        if (!state_.startWithWindows || startupReadFailed_) return;
        state_.signInReplay = !state_.signInReplay;
        if (!state_.signInReplay) cancelSignInReplay(); // A retry still waiting must not start replay.
        break;
    case DiagnosticLogging:
        if (!configureDiagnostics(!state_.diagnosticLogging)) {
            setStatus(L"diagnostic logging could not start");
            return;
        }
        break;
    case SelectProcess: chooseSource(); break;
    case AutoCaptureSource: chooseAutoCapture(); break;
    case SortClips: chooseClipOrder(); break;
    case StartReplay:
        if (busy_ || closing_) return;
        if (recording_ == capture::RecorderState::Off) startReplay();
        else stopReplay();
        break;
    case SaveReplay: saveReplay(); break;
    case AssignHotkey: if (!assigning_) startAssigning(saveShortcut); break;
    case AssignScreenshot: if (!assigning_) startAssigning(screenshotShortcut); break;
    case DesktopAudio: case Microphone: chooseAudioDevice(id == DesktopAudio); break;
    case DesktopReset: case MicrophoneReset: resetAudio(id == DesktopReset); break;
    case CompressCopyAfterTrim:
        state_.compressCopyAfterTrim = !state_.compressCopyAfterTrim;
        player_.setCompressCopyDefault(state_.compressCopyAfterTrim);
        break;
    case GameColours:
        state_.gameColours = !state_.gameColours;
        clipGrid_.setGameColours(state_.gameColours);
        break;
    case ShortGameNames: state_.shortGameNames = !state_.shortGameNames; break; // Names are worked out as they are shown.
    case AskBeforeBinning:
        state_.askBeforeBinning = !state_.askBeforeBinning;
        clipGrid_.setAskBeforeBinning(state_.askBeforeBinning);
        break;
    case AutoCaptureStart:
        autoCaptureOn_ = !autoCaptureOn_;
        auto_.updateHook();
        break;
    case AutoCaptureOffers:
        autoCapturePrompt_ = !autoCapturePrompt_;
        auto_.updateHook();
        break;
    case UpdateCheckSwitch:
        state_.updateCheck = state_.updateCheck == UpdateChoice::On ? UpdateChoice::Off : UpdateChoice::On;
        if (state_.updateCheck == UpdateChoice::On) scheduleUpdateCheck(std::chrono::seconds(0));
        else {
            if (updates_.running() && !updateManual_) updates_.cancel(); // A pending automatic check is dropped.
            updateTried_ = false; // Switching it on again in this session may check again (the daily rule still applies).
            abandonUpdate(); // Off means no network at all: a download stops too.
            updateTag_.clear();
            updateUrl_.clear();
            updateNote_.clear();
        }
        break;
    case UpdateCheckNow:
        if (!updateTag_.empty()) { updateClicked(); return; } // A newer version is known: update to it, or open its page.
        if (updates_.running() && updateManual_) return;
        startUpdateCheck(true, std::chrono::seconds(0)); // Ignores the daily rule and the choice: the click is the consent.
        break;
    case Suppression: audio_.micFilters.noiseSuppression = !audio_.micFilters.noiseSuppression; break;
    case Gate: audio_.micFilters.noiseGate = !audio_.micFilters.noiseGate; break;
    case RecordDiscord: audio_.hideDiscord = !audio_.hideDiscord; break;
    case StorageBehaviour: chooseStorageBehaviour(); break;
    case Resolution: chooseResolution(); break;
    case FrameRate: chooseFrameRate(); break;
    case SidebarToggle:
        state_.sidebarCollapsed = !state_.sidebarCollapsed;
        updateSidebarToggleText();
        layout(); // Every page moves with the sidebar edge.
        redrawn = true;
        break;
    case QualityPreset: chooseQuality(); break;
    case VideoCodec: chooseCodec(); break;
    case ChooseFolder: case WelcomeFolder: chooseFolder(); break;
    case WelcomeAuto: case WelcomeManual:
        welcomeAutomatic_ = id == WelcomeAuto;
        refreshControls(); // Their names carry the choice for screen readers.
        InvalidateRect(control(WelcomeAuto), nullptr, FALSE);
        InvalidateRect(control(WelcomeManual), nullptr, FALSE);
        return;
    case WelcomeShortcut: if (!assigning_) startAssigning(saveShortcut); break;
    case WelcomeUpdates: welcomeUpdates_ = !welcomeUpdates_; break;
    case WelcomeStartup:
        if (startupReadFailed_ || startupOtherCopy_) return; // Disabled then (refreshControls).
        welcomeStartup_ = !welcomeStartup_;
        welcomeStartupChosen_ = true;
        break;
    case WelcomeTray: welcomeTray_ = !welcomeTray_; break;
    case WelcomeLength: chooseWelcomeLength(); return; // Kept in memory until start, like the switches.
    case WelcomeSkip: case WelcomeStart: finishWelcome(id == WelcomeStart); return;
    case OpenFolder:
        if (!clipFolder_.empty())
            shellOpen(clipFolder_.c_str(), L"the folder is not available yet. choose a folder in settings or start replay first.", MB_ICONINFORMATION);
        break;
    default: return;
    }
    if (contains(toggleIds, id)) {
        // A switch: slide it now; the ini write and the other controls follow once it has painted.
        animateSwitch(id, before);
        PostMessageW(window_, afterToggle, static_cast<WPARAM>(id), 0);
        return;
    }
    saveSettings();
    refreshControls();
    // A new device or switching one off or on changes which meter-only captures run.
    if (id == DesktopAudio || id == Microphone || id == DesktopReset || id == MicrophoneReset) syncMeters(true);
    // Choices can add or remove rows (microphone, custom bitrate) or change notes.
    if (redrawn) return;
    refreshPages();
}

// Start or skip (Esc too). Start applies the choices; skip leaves auto-capture as it is and the update choice unmade,
// so the opt-in question still comes. The folder and shortcut were applied when chosen. Either way the screen is gone.
void App::finishWelcome(bool start) {
    if (!welcome_ || closing_) return;
    if (assigning_) finishAssigning(std::nullopt);
    welcome_ = false;
    bool trayRefused = false, startupChange = false;
    if (start) {
        state_.updateCheck = welcomeUpdates_ ? UpdateChoice::On : UpdateChoice::Off;
        // Automatically: listed games start replay and new fullscreen games are offered. Set here, so an unreadable
        // list or a save under way (useAutoCapture declines then) still keeps the choice.
        autoCaptureOn_ = autoCapturePrompt_ = welcomeAutomatic_;
        state_.replaySeconds = welcomeSeconds_; // All games: a fresh install has no game of its own yet.
        syncField(CustomSeconds);
        // As the Settings switch: without a tray icon, closing would leave no way back, so it stays off.
        if (welcomeTray_ != state_.closeToTray) {
            if (welcomeTray_ && !addTrayIcon()) trayRefused = true; // Said once the screen is gone (below).
            else {
                state_.closeToTray = welcomeTray_;
                if (!state_.closeToTray && !startupTray_) removeTrayIcon();
            }
        }
        // Only a switch the user flipped touches the Run entry (another copy's entry, or one read after the screen
        // opened, is left alone). Changed once the screen is gone (below): a refusal opens a dialog.
        startupChange = welcomeStartupChosen_; // Ownership and current state are re-read when applying the choice.
    }
    if (start && welcomeAutomatic_ && canUseAutoCapture()) {
        useAutoCapture(); // Also saves (the update choice too), refreshes, updates the hook and looks at the window in front.
    } else {
        auto_.updateHook(); // Auto-capture waited for the welcome screen (autoContext).
        PostMessageW(window_, AutoSession::foregroundCheck, 0, 0);
        // "Automatically" was kept above but could not take over (replay, a save or sleep, or the list is
        // unreadable): say so rather than "ready", which would claim the remembered source was let go.
        if (start && welcomeAutomatic_)
            setStatus(!autoCaptureReadable_ ? L"auto-capture is on · its game list could not be read" :
                      recording_ != capture::RecorderState::Off ? L"auto-capture is on · choose use auto-capture once replay is idle" :
                                                                  L"auto-capture is on · choose use auto-capture once refarter is idle");
        saveSettings(); // Only what changed: the update choice, or the auto-capture switches.
        refreshControls();
    }
    navigate(start && !welcomeAutomatic_ ? Page::Recording : Page::Library); // Lays out and repaints everything.
    if (trayRefused) // As the Settings switch says it; a status would be replaced by the auto-capture one.
        messageBox(window_, L"windows could not create the tray icon. refarter will stay open.", L"refarter", MB_OK | MB_ICONERROR);
    if (closing_) return; // Quit from the tray while that box was open: nothing more to apply.
    if (startupChange && applyStartupChoice(welcomeStartup_)) refreshControls(); // Writes the Run entry and the ini; says why it could not.
    scheduleUpdateCheck(startupUpdateDelay);
    resumeHeldDialogs(); // The opt-in question after a skip, and anything else that waited.
}

void App::afterToggleChanged(int id) {
    if (closing_) return;
    // Kept in memory until start: only their names change.
    if (id == WelcomeUpdates || id == WelcomeStartup || id == WelcomeTray) { refreshControls(); return; }
    saveSettings();
    refreshControls();
    refreshPages(); // The noise gate adds or removes rows; discord and the startup note change text in place.
}

namespace {
// A monitor's size; null: the main screen. Without an answer, 1080p.
capture::Size monitorSize(HMONITOR monitor) {
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(monitor ? monitor : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &info))
        return {info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top};
    return {1920, 1080};
}
constexpr ULONGLONG measureReuseMs = 250; // One refresh asks for the size several times; the window is measured once.
} // namespace

capture::Size App::sourceSize() const {
    // Typing in a number field refreshes the page on each key: the game's window is measured once per refresh rather
    // than for each estimate, so a busy game is asked as little as possible.
    const HWND window = target_.source.window;
    const auto now = GetTickCount64();
    if (window && window == measuredWindow_ && now - measuredAt_ < measureReuseMs) return measuredSize_;
    measuredSize_ = measureSource();
    measuredWindow_ = window;
    measuredAt_ = now;
    return measuredSize_;
}

capture::Size App::measureSource() const {
    if (const HWND window = target_.source.window) {
        // A game's size is kept for this session, so its own settings' estimates describe it after the source changes.
        // Only a new game or size is written, so the many estimate redraws cost a comparison, not a map insert.
        const auto seen = [&](capture::Size size) {
            if (!target_.path.empty() && (size != sizedGameSize_ || !samePath(target_.path, sizedGamePath_))) {
                sizedGamePath_ = target_.path;
                sizedGameSize_ = size;
                gameSizes_[lower(target_.path)] = size;
            }
            return size;
        };
        RECT client{};
        if (!IsIconic(window) && GetClientRect(window, &client) && client.right >= 2 && client.bottom >= 2) {
            // Its frame is kept for when it is minimised: measured, so DPI and custom frames do not matter.
            RECT outer{};
            if (!IsZoomed(window) && GetWindowRect(window, &outer)) {
                sizedWindow_ = window;
                sizedFrame_ = {(outer.right - outer.left) - client.right, (outer.bottom - outer.top) - client.bottom};
            }
            return seen({client.right, client.bottom});
        }
        // A minimised window has no client area: the size it restores to, less its frame (measured when last seen, or
        // else estimated). One that restores maximised, or whose size cannot be read, falls back to its monitor.
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (IsIconic(window) && GetWindowPlacement(window, &placement) && !(placement.flags & WPF_RESTORETOMAXIMIZED)) {
            capture::Size frame = sizedFrame_;
            if (window != sizedWindow_) {
                RECT estimate{};
                const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)) & ~static_cast<DWORD>(WS_MINIMIZE);
                const auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
                const UINT dpi = GetDpiForWindow(window);
                AdjustWindowRectExForDpi(&estimate, style, GetMenu(window) != nullptr, exStyle, dpi ? dpi : 96);
                frame = {estimate.right - estimate.left, estimate.bottom - estimate.top};
            }
            const RECT& normal = placement.rcNormalPosition;
            const int width = (normal.right - normal.left) - frame.width;
            const int height = (normal.bottom - normal.top) - frame.height;
            if (width >= 2 && height >= 2) return seen({width, height});
        }
    }
    return monitorSize(target_.source.window ? MonitorFromWindow(target_.source.window, MONITOR_DEFAULTTOPRIMARY) : target_.source.monitor);
}

capture::Size App::encodedSize(const capture::VideoSettings& video, std::optional<capture::Size> size) const {
    return capture::outputSize(video.resolution, size ? *size : sourceSize());
}

App::ScopeSize App::scopeSize() const {
    // All games or the source's own game: the source. Another game: its size as last seen this session, else the
    // main screen's (most games run fullscreen), so the page's estimates describe the game being edited.
    if (!gameScope() || scopeIsSource()) return {sourceSize(), false};
    if (const auto found = gameSizes_.find(lower(settingsFor_)); found != gameSizes_.end()) return {found->second, false};
    return {monitorSize(nullptr), true};
}

void App::probeCodecs() {
    // Opens hardware encoders for a moment: never while replay holds the GPU.
    if (!codecs_.empty() || probing_ || busy_ || closing_ || recording_ != capture::RecorderState::Off) return;
    probing_ = true;
    recorder_->probeCodecs();
}

const capture::CodecSupport* App::codecSupport(capture::Codec codec) const {
    for (const auto& support : codecs_) if (support.codec == codec) return &support;
    return nullptr;
}

void App::resetAudio(bool desktop) {
    // As if the slider were moved to 100% and "system default" picked in the device menu.
    (desktop ? audio_.levels.desktopPercent : audio_.levels.microphonePercent) = 100;
    SendMessageW(control(desktop ? DesktopVolume : MicrophoneVolume), TBM_SETPOS, TRUE, 100);
    updateVolumeLabel(desktop ? DesktopVolume : MicrophoneVolume);
    (desktop ? audio_.desktop : audio_.microphone) = {capture::AudioDevice::Mode::SystemDefault};
    if (desktop) checkDiscordOutput();
}

void App::checkDiscordOutput() {
    // Asked when the audio page shows or the output changes; nothing watches it in the background.
    discordSharesOutput_ = audio_.desktop.mode == capture::AudioDevice::Mode::Fixed && capture::discordPlaysOn(audio_.desktop.id);
}

bool App::startReplay(bool userStart) {
    if (userStart) cancelSignInReplay(); // The user took over: the sign-in start's retry must not start a replay later.
    if (busy_ || closing_ || endingSession_ || recording_ != capture::RecorderState::Off) return false;
    revalidateMonitor(); // The displays may have changed since the choice (wake, hotplug): never start on a stale handle.
    // A start the user asked for counts as leaving a game's number field, as saveSettings below does for all games. If
    // the source's own values could not be saved, nothing starts: recording the old ones while the page shows the new
    // would be a silent swap. The typed values stay, and the status says why. An automatic start (auto-capture, a
    // restart after sleep) never stores half-typed text: it records what is stored, and the typing goes on.
    // Another game's value that could not be saved does not stop this start, but the status still says so.
    bool unsaved = false;
    if (userStart && !commitScopeFields()) {
        if (scopeIsSource()) return false;
        unsaved = true;
    }
    // Every start (manual, auto-capture, restart) comes here: a game window with its own values records with them.
    bool own = false;
    std::optional<capture::Codec> refused;
    const auto video = sourceVideo(&own, &refused);
    if ((!target_.source.window && !target_.source.monitor) || !fieldsReady(video) ||
        (audio_.microphone.enabled() && audio_.micFilters.noiseGate && !gateInputValid())) return false;
    video_.replaySeconds = state_.replaySeconds;
    saveSettings(); // Number fields are written when left; starting a replay counts as leaving them.
    busy_ = true;
    closeMeterCaptures(); // Hand the devices over: the recorder opens its own and feeds the same meters.
    audio_.desktopMeter = &desktopMeter_;
    audio_.microphoneMeter = &microphoneMeter_;
    session_ = video;
    sessionOwn_ = own;
    // Never a silent swap: the started status says the game's codec was replaced.
    startNote_ = refused ? std::wstring(capture::codecName(*refused)) + L" failed this pc's check · recording h.264" : std::wstring{};
    if (unsaved) startNote_ += std::wstring(startNote_.empty() ? L"" : L" · ") + L"game settings could not be saved";
    // Only that a game's own values were used (and whether its codec fell back): no path or name, like the rest of the log.
    if (own) diagnosticLog().write(DiagnosticEvent::GameSettingsUsed, S_OK, refused ? 1u : 0u);
    // A minimised game is waited for (up to ten seconds) until it is restored.
    setStatus(std::wstring(target_.source.window && IsIconic(target_.source.window) ?
        L"return to your game · waiting for capture…" : L"starting replay…") + (unsaved ? L" · game settings could not be saved" : L""));
    recorder_->start(target_, video, audio_, clipFolder_);
    return true;
}

const GameVideo* App::gameOwn(const std::wstring& path) const {
    if (!gameSettingsReadable_ || path.empty()) return nullptr;
    const auto* entry = gameSettings_.find(path);
    return entry ? &entry->video : nullptr;
}

const GameSettingsEntry* App::scopeEntry() const {
    return gameScope() ? gameSettings_.find(settingsFor_) : nullptr;
}

capture::VideoSettings App::scopeVideo() const {
    const auto* entry = scopeEntry();
    auto video = resolveVideo(video_, state_.replaySeconds, entry ? &entry->video : nullptr);
    // What is being typed for the game shows at once, as all games' values do; commitScopeFields clears these before
    // it compares, so a stored value is still only what the game sets.
    if (typedSeconds_) video.replaySeconds = *typedSeconds_;
    if (typedBitrate_) video.bitrateMbps = *typedBitrate_;
    return video;
}

capture::VideoSettings App::sourceVideo(bool* own, std::optional<capture::Codec>* refused) const {
    // A monitor records with the all-games values; so does a game without its own (see startVideo).
    const auto start = startVideo(video_, state_.replaySeconds, target_.source.window ? gameOwn(target_.path) : nullptr, codecs_);
    if (own) *own = start.own;
    if (refused) *refused = start.refused;
    return start.video;
}

capture::VideoSettings App::scopeRecordVideo(std::optional<capture::Codec>* refused) const {
    auto video = scopeVideo();
    // Only a game's own codec falls back here; all games' choice is changed by the codec check itself.
    const auto fallback = refusedCodec(scopeEntry() != nullptr, video.codec, codecs_);
    if (refused) *refused = fallback;
    if (fallback) video.codec = capture::Codec::H264;
    return video;
}

capture::VideoSettings App::recordingVideo() const {
    return recording_ != capture::RecorderState::Off ? session_ : sourceVideo();
}

namespace {
// The value a reset arrow names: videoResetIds is in GameValue's order.
GameValue valueOf(int reset) {
    const auto found = std::find(std::begin(videoResetIds), std::end(videoResetIds), reset);
    return static_cast<GameValue>(found - std::begin(videoResetIds));
}
} // namespace

bool App::scopeOwns(int reset) const {
    const auto* entry = scopeEntry();
    return entry && contains(videoResetIds, reset) && ownValue(entry->video, valueOf(reset)).has_value();
}

bool App::scopeIsSource() const {
    return gameScope() && target_.source.window && samePath(settingsFor_, target_.path);
}

bool App::fieldsReady(const capture::VideoSettings& video) const {
    // The number fields stop a start only while they edit what it records with (see fieldsAllowStart). The bitrate
    // field is hidden unless the page's scope records at custom quality, and a hidden field can never block a start.
    const bool allGames = !gameScope();
    const auto* sourceOwn = target_.source.window ? gameOwn(target_.path) : nullptr;
    const bool bitrateValid = bitrateValid_ || scopeVideo().quality != capture::Quality::Custom;
    return fieldsAllowStart(allGames || scopeIsSource(), allGames, sourceOwn, video.quality, state_.replayInputValid, bitrateValid);
}

bool App::commitScopeFields() {
    // A game's typed length and bitrate are stored once the field is left (or replay starts), as all games' are
    // written to refarter.ini then: typing 40 never stores 4 on the way. Both go in one write, so it keeps both or
    // neither; on failure they stay typed and the status says so.
    if (!gameScope() || (!typedSeconds_ && !typedBitrate_)) {
        typedSeconds_.reset();
        typedBitrate_.reset();
        return true;
    }
    const auto* entry = scopeEntry();
    const GameVideo current = entry ? entry->video : GameVideo{};
    const auto allGames = resolveVideo(video_, state_.replaySeconds, nullptr);
    auto own = current;
    if (typedSeconds_) own = chooseValue(own, GameValue::ReplaySeconds, *typedSeconds_, allGames);
    if (typedBitrate_) own = chooseValue(own, GameValue::Bitrate, *typedBitrate_, allGames);
    if (own != current && !storeScope(std::move(own))) return false;
    typedSeconds_.reset();
    typedBitrate_.reset();
    return true; // The caller refreshes the page once (leaving a field, a scope change, a start).
}

void App::setScopeValue(int reset, int value) {
    if (!gameScope()) {
        // All games: as before, in the settings written to refarter.ini by the caller.
        // The length lives in state_ (the presets and the custom field), the rest in video_.
        if (reset == ReplayReset) state_.replaySeconds = value;
        else setVideoValue(video_, valueOf(reset), value);
        if (reset == ReplayReset) syncField(CustomSeconds); // A preset: the custom field shows the new length.
        if (reset == QualityReset && video_.quality != capture::Quality::Custom) {
            // A quality off custom hides the bitrate row: its text and validity go back to the stored rate, and games
            // that inherited custom quality drop their own bitrate (it belongs to custom quality, see chooseValue).
            syncField(Bitrate);
            auto list = gameSettings_;
            if (gameSettingsReadable_ && list.dropUnusedBitrates(resolveVideo(video_, state_.replaySeconds, nullptr))) {
                if (writeGameSettings(gameSettingsPath(), list)) gameSettings_ = std::move(list);
                else setStatus(L"game settings could not be saved");
            }
        }
        return;
    }
    // All games' value clears the game's own one rather than saving a copy (see chooseValue). A failed write: what was
    // typed stays (the status says why).
    const auto* entry = scopeEntry();
    const GameVideo current = entry ? entry->video : GameVideo{};
    auto own = chooseValue(current, valueOf(reset), value, resolveVideo(video_, state_.replaySeconds, nullptr));
    if (own == current) {
        // No change: a preset still puts its length in the field, but the quality already chosen keeps a typed
        // bitrate (custom again must not undo 70 typed but not yet saved).
        if (reset == ReplayReset) syncField(CustomSeconds);
        return;
    }
    if (storeScope(std::move(own))) syncFieldsFor(reset);
}

void App::syncFieldsFor(int reset) {
    // Only the number field the change touched: a quality change can drop or hide the game's bitrate.
    if (reset == ReplayReset) syncField(CustomSeconds);
    else if (reset == BitrateReset || reset == QualityReset) syncField(Bitrate);
}

void App::clearScopeValue(int reset) {
    const auto* entry = scopeEntry();
    if (!entry) return;
    // Clearing the game's quality applies the same bitrate rule as choosing one (see clearValue).
    // Not saved: the arrow stays, and so does the focus, so the press can be tried again.
    if (!storeScope(clearValue(entry->video, valueOf(reset), resolveVideo(video_, state_.replaySeconds, nullptr)))) return;
    syncFieldsFor(reset);
    // The arrow hides with the value it cleared: keyboard focus stays on the row.
    if (GetFocus() == control(reset)) SetFocus(control(controlOfReset(reset)));
}

bool App::storeScope(GameVideo video) {
    if (!gameScope()) return false;
    auto list = gameSettings_;
    // A game already in the file keeps its stored name: names are display only and never rewritten.
    const auto* stored = list.find(settingsFor_);
    list.set(settingsFor_, stored ? stored->label : settingsForLabel_, std::move(video)); // All values empty: the game is removed.
    // The values change only once they are safely on disk, as the auto-capture list does.
    if (!writeGameSettings(gameSettingsPath(), list)) {
        setStatus(L"game settings could not be saved");
        return false;
    }
    gameSettings_ = std::move(list);
    return true;
}

void App::useDefaults() {
    // No values at all removes the game (see GameSettingsList::set).
    if (!scopeEntry() || !storeScope({})) return;
    syncScopeFields();
    // The scope stays on the game, now following all games; the button hides.
    if (GetFocus() == control(UseDefaults)) SetFocus(control(SettingsFor));
}

bool App::setScope(std::wstring path, std::wstring label) {
    // A value typed for the previous game is kept before the page moves on; if it cannot be saved, the page stays on
    // that game with the value still typed (the status says why).
    if (!commitScopeFields()) return false;
    settingsFor_ = std::move(path);
    settingsForLabel_ = oneLineLabel(std::move(label)); // A window title may hold a tab or line break.
    syncScopeFields();
    return true;
}

std::wstring App::knownGameName(const std::wstring& path, std::wstring fallback) const {
    if (path.empty()) return fallback;
    if (const auto* game = autoCapture_.find(path)) return game->label;
    if (const auto* entry = gameSettings_.find(path)) return entry->label;
    return fallback;
}

std::wstring App::storedGameName(const RecordingTarget& window) const {
    return knownGameName(window.path, friendlySourceName(window.title, window.name));
}

std::wstring App::displayGameName(const std::wstring& path, const std::wstring& stored) const {
    // A name the user gave is never turned into a short name; like every name, it is lower case and cut where shown
    // (shownName: menus, the footer and the tray stop at 48 characters, the field allows maxCustomGameName).
    if (const auto* custom = path.empty() ? nullptr : gameNames_.find(path)) return lower(custom->name);
    return automaticGameName(path, stored);
}

std::wstring App::automaticGameName(const std::wstring& path, const std::wstring& stored) const {
    return state_.shortGameNames ? gameName(stored, path) : lower(stored);
}

void App::renameGame(std::wstring path, std::wstring stored) {
    if (closing_ || dialogOpen() || !canRename(path)) return; // Another dialog (or this one) is open: it waits.
    const auto* custom = gameNames_.find(path);
    // Cut as it would be kept, so the field never holds more than can be saved.
    std::wstring name = custom ? custom->name : customGameName(automaticGameName(path, stored));
    // Only the title: the field and its buttons say what happens. Clearing the field brings back the usual name.
    std::optional<std::wstring> typed;
    {
        // The dialog pumps the App's messages: a queued error or question waits until it closes (dialogOpen), then opens.
        renamingGame_ = true;
        struct Done { App& app; ~Done() { app.renamingGame_ = false; app.resumeHeldDialogs(); } } done{*this};
        if (answerName_) typed = answerName_(name);
        else if (nameBox(window_, L"", L"rename game", L"save", name, static_cast<int>(maxCustomGameName))) typed = std::move(name);
    }
    if (!typed || closing_) return;
    // The usual name typed back (or saved unchanged) is no custom name: it keeps following "short game names".
    // Compared as cut, as the field offered it: a usual name over 80 characters is offered cut.
    if (lower(customGameName(*typed)) == customGameName(automaticGameName(path, stored))) typed->clear();
    setGameName(path, std::move(*typed));
}

bool App::setGameName(const std::wstring& path, std::wstring name) {
    if (!canRename(path)) return false;
    name = customGameName(std::move(name)); // Empty: the game's own name is used again.
    const auto* current = gameNames_.find(path);
    if (current ? current->name == name : name.empty()) return true;
    auto names = gameNames_;
    names.set(path, std::move(name));
    const bool saved = writeGameNames(gameNamesPath(), names);
    if (saved) gameNames_ = std::move(names);
    else setStatus(L"game names could not be saved · the name was not changed");
    // Names are worked out as they are shown: the page, the footer and the tray tip only need drawing again.
    refreshControls();
    refreshPages();
    updateTrayTip();
    InvalidateRect(window_, nullptr, FALSE);
    return saved;
}

std::wstring App::shownSourceName(const RecordingTarget& source) const {
    // A monitor's label is short and its end ("· primary") matters: it is shown whole, as before.
    if (!source.source.window) return lower(source.displayName.empty() ? source.name : source.displayName);
    auto name = displayGameName(source.path, storedGameName(source));
    auto suffix = pickerSuffix(source.displayName, source.name);
    // A label saved before short names may already end with the suffix, or with its executable part.
    if (name.ends_with(suffix)) suffix.clear();
    else if (const auto next = suffix.find(L" · ", 3); next != std::wstring::npos && name.ends_with(std::wstring_view(suffix).substr(0, next)))
        suffix.erase(0, next);
    if (suffix.size() > 24) {
        // "· window N" is what tells the windows apart: a long executable name gives way first.
        const auto window = suffix.rfind(L" · window ");
        const auto ordinal = window == std::wstring::npos ? std::wstring{} : suffix.substr(window);
        suffix = shownName(std::wstring_view(suffix).substr(0, suffix.size() - ordinal.size()), 24 - std::min<std::size_t>(ordinal.size(), 16)) + ordinal;
    }
    // The suffix is what tells the windows apart, so the name gives way: 48 characters in all.
    return shownName(name, suffix.empty() ? 48 : 48 - suffix.size()) + suffix;
}

void App::syncField(int id) {
    if (!control(id)) return;
    const bool length = id == CustomSeconds;
    (length ? typedSeconds_ : typedBitrate_).reset(); // The field now shows the stored value: nothing typed is waiting.
    const auto shown = scopeVideo();
    // Setting the text raises EN_CHANGE; syncingFields_ keeps that from counting as an edit.
    syncingFields_ = true;
    setTextIfChanged(control(id), std::to_wstring(length ? shown.replaySeconds : shown.bitrateMbps));
    syncingFields_ = false;
    (length ? state_.replayInputValid : bitrateValid_) = true;
}

void App::syncScopeFields() {
    syncField(CustomSeconds);
    syncField(Bitrate);
}

void App::followSource() {
    const std::wstring source = target_.source.window ? target_.path : std::wstring{};
    // Only a change of source moves the page: choosing the same game again keeps the scope the user picked since (an
    // automatic start never moves the page, so scopeSource_ is the source the user last chose).
    if (samePath(source, scopeSource_)) return;
    // A new source with its own values opens on them; anything else (a monitor too) on all games. Only once the page has
    // moved is the source remembered: if a typed value could not be saved, the next source change tries again.
    const auto* entry = source.empty() || !gameSettingsReadable_ ? nullptr : gameSettings_.find(source);
    if (!setScope(entry ? entry->path : std::wstring{}, entry ? entry->label : std::wstring{})) return;
    scopeSource_ = source;
    if (!control(StartReplay)) return;
    refreshControls();
    refreshPages();
}

void App::cancelSignInReplay() {
    signInPending_ = false;
    KillTimer(window_, signInReplayTimer);
    // The retry is used up, but a sign-in start already in flight stays one: its failure is still a status, not a dialog.
    signInRetried_ = true;
}

// Sign-in replay: only the remembered whole monitor (a game starts through auto-capture instead). Never a dialog, as
// the window is usually hidden: a failure is a status, and one retry covers displays and drivers still settling.
void App::startSignInReplay() {
    // KillTimer leaves already-queued WM_TIMER messages behind. Only a still-pending request may start anything.
    if (!std::exchange(signInPending_, false)) return;
    if (welcome_) { cancelSignInReplay(); return; } // No unattended recording before onboarding is answered.
    KillTimer(window_, signInReplayTimer);
    if (closing_ || endingSession_ || !state_.signInReplay || !state_.startWithWindows) return; // Switched off meanwhile.
    // The user or auto-capture got there first, or the source is no longer the remembered monitor: nothing to say.
    if (recording_ != capture::RecorderState::Off || sourceMonitor_.empty() || target_.source.window) return;
    if (asleep_ || busy_) {
        signInReplayFailed({}); // May pass, as a monitor not yet ready does.
    } else {
        signInReplay_ = true; // Only until this start's Started event: another start is never taken for it.
        // On success the Started event says how it went (see signInReplayFailed).
        if (!startReplay(false)) {
            signInReplay_ = false;
            // startReplay looked for the monitor again. Only one not yet connected is worth a retry; invalid settings would fail again.
            if (!target_.source.monitor) signInReplayFailed(L"the monitor from last time is not connected");
            else setStatus(L"replay could not start at sign-in · check the replay settings");
        }
    }
    refreshControls(); // "starting…" or the status, even with the window open.
    refreshPages();
}

// A sign-in start failed for a reason that may pass (monitor not ready, capture refused): one retry, then a status.
void App::signInReplayFailed(const std::wstring& reason) {
    signInReplay_ = false;
    const bool retry = !std::exchange(signInRetried_, true);
    signInPending_ = retry && SetTimer(window_, signInReplayTimer, signInRetryMs, nullptr) != 0;
    setStatus((reason.empty() ? std::wstring(L"replay could not start at sign-in") : reason) +
                (signInPending_ ? L" · trying again shortly" : L" · enable replay"));
}

void App::stopReplay() {
    if (busy_ || closing_ || recording_ == capture::RecorderState::Off) return;
    cancelSignInReplay();
    auto_.onManualStop(target_, recording_ == capture::RecorderState::Buffering);
    busy_ = true;
    setStatus(L"stopping replay…");
    recorder_->stop();
}

AutoSession::Context App::autoContext() const {
    // Nothing starts or is offered until the welcome screen is answered, including after a hidden sign-in start that
    // has not shown it yet or a close to the tray: the user has not chosen how recording should start. Opening the
    // window (or any tray command) shows the screen again.
    const bool waiting = welcome_;
    return {closing_ || endingSession_, busy_ || asleep_, recording_, &target_, autoCaptureOn_ && !waiting, autoCapturePrompt_ && !waiting,
            autoCaptureReadable_, &autoCapture_};
}

bool App::startAutoReplay(RecordingTarget target) {
    auto previous = std::move(target_);
    target_ = std::move(target);
    // An automatic start leaves the page alone: the user may be typing in it, and the page locks while replay runs.
    if (startReplay(false)) return true;
    target_ = std::move(previous);
    return false;
}

void App::declineGame(const std::wstring& path) {
    auto list = autoCapture_;
    list.decline(path);
    if (writeAutoCapture(autoCapturePath(), list)) autoCapture_ = std::move(list);
}

AutoSession::Host App::autoHost() {
    AutoSession::Host host;
    host.context = [this] { return autoContext(); };
    host.start = [this](RecordingTarget target) { return startAutoReplay(std::move(target)); };
    host.stop = [this] { busy_ = true; recorder_->stop(); };
    host.status = [this](std::wstring text) { setStatus(std::move(text)); };
    host.add = [this](const std::wstring& path, const std::wstring& label) { setAutoCaptured(path, label, true); };
    host.name = [this](const RecordingTarget& target) { return storedGameName(target); };
    host.shown = [this](const std::wstring& path, const std::wstring& stored) { return shownName(displayGameName(path, stored)); };
    host.decline = [this](const std::wstring& path) { declineGame(path); };
    host.refresh = [this] { refreshControls(); InvalidateRect(window_, nullptr, FALSE); };
    host.hideFlyout = [this] { trayFlyout_.hide(); };
    return host;
}

void App::setAutoCaptured(const std::wstring& path, const std::wstring& label, bool on) {
    if (!autoCaptureReadable_ || path.empty()) return;
    auto list = autoCapture_;
    if (on) list.add(path, label);
    else list.remove(path);
    // The list changes only once it is safely on disk.
    if (writeAutoCapture(autoCapturePath(), list)) {
        autoCapture_ = std::move(list);
        shareListedGames();
    } else setStatus(L"the auto-capture list could not be saved");
    refreshControls();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::shareListedGames() {
    std::vector<std::pair<std::wstring, std::wstring>> games;
    for (const auto& game : autoCapture_.games) games.emplace_back(game.label, game.path);
    clipGrid_.setListedGames(games);
}

bool App::canUseAutoCapture() const {
    return !pickerStale() && !saving_ && !asleep_ && !endingSession_ && autoCaptureReadable_;
}

void App::useAutoCapture() {
    // Recheck after a menu closes: an automatic start or shutdown may have happened inside it.
    if (!canUseAutoCapture()) return;
    target_ = {};
    setSourceMonitor({});
    followSource();
    autoCaptureOn_ = true;
    saveSettings(); // Forget the remembered monitor too, including after relaunch.
    auto_.updateHook();
    PostMessageW(window_, AutoSession::foregroundCheck, 0, 0);
    setStatus(autoCapture_.games.empty() ? L"auto-capture ready · add a game to your list" :
                                        L"auto-capture ready · waiting for a listed game", true);
    refreshControls();
    refreshPages();
}

void App::trayCommand(TrayCommand command) {
    if (closing_) return;
    // Re-read the state: it may have changed while the flyout was open.
    const bool detected = auto_.detectedAlive();
    switch (command) {
    case TrayCommand::Open: restoreWindow(); return;
    case TrayCommand::Quit: quit(); return;
    default:
        if (welcome_) { restoreWindow(); return; } // Nothing else runs until the welcome screen is answered: it opens instead.
        break;
    }
    switch (command) {
    case TrayCommand::UseAutoCapture: useAutoCapture(); return;
    case TrayCommand::AutoCapture: {
        if (!detected) return;
        const bool listed = autoCapture_.find(auto_.detected().path) != nullptr;
        auto target = auto_.detected();
        // A game with own settings keeps that name; another gets its short name.
        setAutoCaptured(target.path, storedGameName(target), !listed);
        if (!listed) {
            // Added while it runs: start now, as if it came to the front, under its name on the list (if it was saved).
            target.displayName = knownGameName(target.path, target.displayName);
            auto_.tryAutoCapture(std::move(target));
        }
        break;
    }
    case TrayCommand::Action:
        if (recording_ != capture::RecorderState::Off) stopReplay();
        else {
            if (!target_.source.window && !target_.source.monitor && detected) {
                target_ = auto_.detected();
                setSourceMonitor({});
                followSource();
            }
            if (!startReplay()) report(L"replay could not start. check the source and replay settings in refarter.", MB_ICONINFORMATION);
        }
        break;
    }
    refreshControls();
    InvalidateRect(window_, nullptr, FALSE);
}


App::SessionLine App::sessionLine() const {
    using State = SessionLine::State;
    SessionLine line;
    line.source = shownSourceName(target_);
    // The footer and the tray say when the replay records with a game's own values.
    if (sessionOwn_ && recording_ != capture::RecorderState::Off && !line.source.empty()) line.source += L" · own settings";
    line.automatic = auto_.active();
    // The footer's wording; the tray tip and flyout build on it.
    if (closing_) line.state = State::Closing, line.status = L"closing…";
    else if (saving_) line.state = State::Saving, line.status = L"saving replay…";
    else if (busy_) line.state = State::Busy, line.status = L"please wait…";
    else if (recording_ == capture::RecorderState::Buffering) line.state = State::Live, line.status = L"replay on";
    else if (recording_ == capture::RecorderState::Stopped) line.state = State::Retained, line.status = L"replay retained";
    else line.state = State::Off, line.status = L"recording off";
    return line;
}

std::wstring App::SessionLine::summary() const {
    switch (state) {
    case State::Live: return (automatic ? L"auto-capture · " : L"replay on · ") + source;
    case State::Retained: return L"replay retained · save or clear it";
    case State::Off: return source.empty() ? status : status + L" · " + source;
    default: return status;
    }
}

bool App::replayKeptNow() const {
    if (recording_ == capture::RecorderState::Off || closing_ || endingSession_) return false;
    return !busy_ || (asleep_ && sleep_.wasBuffering); // A stop in progress clears it; the sleep pause keeps it.
}

std::wstring App::trayTip() const {
    const auto line = sessionLine();
    std::wstring tip = L"refarter · " + (line.state == SessionLine::State::Off ? std::wstring(L"open, or right-click for replay") : line.summary());
    if (tip.size() > 127) tip.resize(127);
    return tip;
}

void App::updateTrayTip() {
    if (tray_.present()) tray_.setTip(trayTip());
}

std::wstring App::saveName(std::wstring* executable) const {
    if (target_.source.window) {
        if (executable) *executable = target_.path;
        return friendlySourceName(target_.title, target_.name);
    }
    // A whole monitor: a listed game in front on it at the press names the save. Read once here; no hook or polling.
    // Only while replay runs: a retained replay was recorded before, perhaps of another game now in front.
    std::optional<ForegroundApp> front;
    if (executable) executable->clear(); // The monitor's own name has no game behind it.
    if (recording_ != capture::RecorderState::Buffering) return monitorSaveName(target_.name, front);
    // The root owner, so a dialog of the game in front still counts as the game.
    std::wstring frontPath;
    if (const auto app = windowTarget(GetAncestor(GetForegroundWindow(), GA_ROOTOWNER))) {
        const auto* listed = autoCapture_.find(app->path);
        // The list's label, not the live window title, which some games change (server, account): one folder per game.
        front = ForegroundApp{listed != nullptr,
                              target_.source.monitor && MonitorFromWindow(app->source.window, MONITOR_DEFAULTTONULL) == target_.source.monitor,
                              listed ? listed->label : app->title, app->name};
        frontPath = app->path;
    }
    // The game that named it, so its folder shows that game's icon; none when the monitor's name stays.
    if (executable && front && front->listed && front->onMonitor) *executable = frontPath;
    return monitorSaveName(target_.name, front);
}

void App::saveReplay() {
    if (recording_ == capture::RecorderState::Off || busy_ || saving_ || closing_ || asking_ || showingErrors_) return; // asking_: a second press under the question.
    if (installing()) { setStatus(updatingNote); return; } // Setup is about to close Refarter.
    // The clip ends at the press: the save worker may start seconds later (a queue, a sleeping clip drive).
    const auto requestedAt = capture::captureClock();
    std::wstring note;
    if (!storageAllowsSave(note)) { // The status says why; the replay is kept.
        if (!quietDialogs()) chimes_.play(Chimes::Kind::Failed, false); // The game is likely in front.
        return;
    }
    saving_ = true;
    setStatus(note.empty() ? L"saving replay…" : note);
    std::wstring executable;
    const auto name = saveName(&executable); // Read once: a monitor's name follows the app in front at the press.
    recorder_->save(clipFolder_, name, requestedAt);
    savingExecutable_ = std::move(executable); // Remembered once the clip is written.
    // Acknowledge the accepted request, not completion; a failed write plays its own sound later.
    chimes_.play(Chimes::Kind::Saved, false);
    refreshControls();
    InvalidateRect(window_, nullptr, FALSE);
    // The first-time storage question, now that the save has started (the dialog is modal; asking_ guards a second press).
    if (std::exchange(askAfterSave_, false)) askQueuedStorageQuestion();
}

void App::shortcutPressed(int shortcut) {
    if (shortcut == screenshotShortcut) takeScreenshot();
    else saveReplay();
}

// The screenshot shortcut: the next picture of the running capture, saved beside the clips (see RecordingController).
void App::takeScreenshot() {
    if (closing_ || shooting_) return;
    if (installing()) { setStatus(updatingNote); return; }
    if (recording_ != capture::RecorderState::Buffering || busy_) {
        setStatus(L"screenshots need replay running");
        return;
    }
    shooting_ = true;
    setStatus(L"taking a screenshot…");
    recorder_->screenshot(clipFolder_, saveName());
    chimes_.play(Chimes::Kind::Saved, false); // The same acknowledgement as a save request.
}

void App::receiveEvents() {
    std::vector<RecordingEvent> events;
    {
        std::lock_guard lock(eventsMutex_);
        events.swap(events_);
    }
    // Shown once the window is up to date, never from inside the loop; true when the failed-save sound already
    // played, so the dialog adds no beep of its own over it.
    std::vector<std::pair<std::wstring, bool>> errors;
    bool changed = false;           // Something the pages show.
    for (auto& event : events) {
        std::optional<AutoSession::EventResult> automatic; // Set for Started and Stopped.
        bool sounded = false;
        switch (event.kind) {
        case RecordingEvent::Kind::Started:
        case RecordingEvent::Kind::Stopped:
            // An ordinary event that arrives while the sent sleep pause is still due is not its answer: busy_ stays
            // set for the pause's own stop. A deferred suspend (nothing sent yet) needs this answer to clear busy_,
            // or applyPendingSuspend could never run. Neither the short-session nor the game-exit rule runs while asleep.
            if (!(pauseInFlight_ && !event.sleeping)) busy_ = false;
            if (event.sleeping) pauseInFlight_ = false;
            recording_ = event.state;
            if (event.kind == RecordingEvent::Kind::Started && event.state == capture::RecorderState::Buffering) bufferingSince_ = std::chrono::steady_clock::now();
            else if (event.state == capture::RecorderState::Off) bufferingSince_ = {};
            desktopMissing_ = microphoneMissing_ = false; // Devices report again after a start.
            // A replay kept for sleep is not an auto-capture session ending, and only the user clears it: it is
            // detached from the auto session (no game-exit watch, no short-session rule).
            if (sleepKeepReleasesAutoSession(event.sleeping, event.state == capture::RecorderState::Stopped)) auto_.releaseForSleep();
            else if (asleep_ && !event.sleeping &&
                     !(event.kind == RecordingEvent::Kind::Stopped && event.state == capture::RecorderState::Off)) break;
            else
                automatic = auto_.onRecorderEvent(event.kind == RecordingEvent::Kind::Started, event.state, event.success, lower(event.message), event.retryableStart);
            break;
        case RecordingEvent::Kind::Audio:
            desktopMissing_ = event.desktopMissing;
            microphoneMissing_ = event.microphoneMissing;
            discordUnavailable_ = discordUnavailable_ || event.discordUnavailable; // Windows does not change meanwhile.
            changed = true;
            continue; // Shown on the recording page; the status line keeps its message.
        case RecordingEvent::Kind::Saved:
            saving_ = false;
            if (event.success) {
                // The game's folder shows its icon: keyed by the game as the library reads it back from the clip's name.
                if (!savingExecutable_.empty()) clipGrid_.rememberGame(clipSourceFromName(event.path), savingExecutable_);
                clipGrid_.scan(clipFolder_); // The new clip goes to the front.
            } else {
                // The replay survives a failed save only if nothing is stopping, quitting or clearing it meanwhile.
                event.message = failedSaveText(event.message, replayKeptNow());
                // Heard over the game; silent while sleep, sign-out or quitting waits for the save.
                sounded = !quietDialogs();
                if (sounded) chimes_.play(Chimes::Kind::Failed, false);
            }
            savingExecutable_.clear();
            break;
        case RecordingEvent::Kind::Folder:
            busy_ = false;
            if (event.success) {
                clipFolder_ = capture::normalisedFolder(event.path);
                clipGrid_.scan(clipFolder_);
                saveSettings();
            }
            break;
        case RecordingEvent::Kind::Codecs:
            probing_ = false;
            codecs_ = std::move(event.codecs);
            // A saved hevc/av1 choice this gpu cannot record falls back to h.264.
            if (const auto* support = codecSupport(video_.codec);
                video_.codec != capture::Codec::H264 && (!support || !support->records)) {
                video_.codec = capture::Codec::H264;
                saveSettings();
                changed = true;
            }
            // The chosen game's own codec may be refused now: its codec row and estimate say so (see scopeRecordVideo).
            if (std::optional<capture::Codec> refused; gameScope() && (scopeRecordVideo(&refused), refused)) changed = true;
            continue; // Shown in the codec menu; no status line.
        case RecordingEvent::Kind::Screenshot:
            // A status, never a dialog: the shortcut is usually pressed over a game.
            shooting_ = false;
            setStatus(lower(event.message));
            changed = true;
            continue;
        case RecordingEvent::Kind::Closed:
            recorderClosed_ = true;
            if (!compression_.active()) DestroyWindow(window_);
            return;
        }
        changed = true;
        // An auto-capture failure must never put a dialog over the game: status only. So is a failed restart after sleep.
        const bool sleepStart = sleepRestart_ && event.kind == RecordingEvent::Kind::Started;
        // Likewise the sign-in start: the window is usually hidden, and a dialog would wait for the first opening.
        const bool signInStart = signInReplay_ && event.kind == RecordingEvent::Kind::Started;
        const bool quiet = (automatic && automatic->quiet) || sleepStart || signInStart;
        setStatus(automatic ? std::move(automatic->status) : lower(event.message));
        // A game's codec that fell back to h.264 says so once the replay is on, after the wording below is chosen (a
        // restart after sleep replaces the status).
        const auto note = event.kind == RecordingEvent::Kind::Started ? std::exchange(startNote_, {}) : std::wstring{};
        if (event.sleeping && asleep_) {
            sleep_.kept = event.state == capture::RecorderState::Stopped;
            setStatus(sleep_.kept ? L"paused for sleep · replay kept" : L"replay paused for sleep");
        }
        if (sleepStart) {
            sleepRestart_ = false;
            setStatus(event.success ? L"restarted after sleep" : L"restart failed · enable replay");
        }
        if (signInStart) {
            if (event.success) signInReplay_ = false;
            else signInReplayFailed(status_); // The recorder's reason, as a status: the hidden window shows no dialog.
        } else if (event.kind == RecordingEvent::Kind::Started) {
            cancelSignInReplay(); // Another start (auto-capture) came first: once it ends, the retry must not start the monitor.
        }
        if (event.success && !note.empty()) setStatus(status_ + L" · " + note);
        if (!event.success && !closing_ && !quiet) errors.emplace_back(status_, sounded);
    }
    if (pendingSuspend_) { applyPendingSuspend(); changed = true; } // A suspend that came while the recorder was busy.
    auto_.recheckWhenIdle(); // A deferred stop cannot rearm until sleep has ended.
    // An exit notification may have arrived while another worker command was busy.
    if (auto_.clearExitedGameReplay()) changed = true;
    if (!leftoverNote_.empty()) { // A cleanup result that waited for an idle recorder.
        // Never in the batch that just set a Saved or sleep status: a later, event-free batch shows it.
        if (events.empty()) { showLeftoverNote(); changed = true; }
        else PostMessageW(window_, recordingEvent, 0, 0);
    }
    refreshControls();
    syncMeters(); // Replay on or off decides which meters run.
    // The sidebar's save button follows the recording state; status, missing devices
    // and Discord notes can change row heights on the scrolling pages.
    if (changed) refreshPages();
    const bool quiet = quietDialogs(); // Decided before finishResume: errors seen while asleep stay status text.
    if (resumeDue_) finishResume(); // The recorder may have just become idle after the stop that sleep began.
    for (const auto& [error, sounded] : errors) {
        if (quiet || quietDialogs()) break; // Quitting or a session end can begin while an earlier dialog is open.
        // Never a dialog over a game or while the window is hidden: the status line and the failed-save sound have
        // told the player, and the message waits for the next activation (see showQueuedErrors).
        queueMessage(error, sounded ? 0u : static_cast<UINT>(MB_ICONERROR)); // The chime already sounded: no error beep.
    }
    showQueuedErrors();
}

bool App::queueMessage(const std::wstring& text, UINT icon) {
    if (queuedErrors_.size() >= maxQueuedErrors ||
        std::any_of(queuedErrors_.begin(), queuedErrors_.end(), [&](const auto& queued) { return queued.first == text; })) return false;
    queuedErrors_.emplace_back(text, icon & MB_ICONMASK);
    return true;
}

// Refarter's window came to the front: the error messages that arrived meanwhile, now where a dialog is welcome.
// Over the welcome screen too (modalOpen, not dialogOpen): a clip folder it chose may be refused.
void App::showQueuedErrors() {
    if (queuedErrors_.empty() || showingErrors_ || modalOpen() || quietDialogs() || !ownWindowForeground()) return;
    showingErrors_ = true;
    // One bounded batch. Nested messages append to the same queue without opening another dialog.
    const auto count = queuedErrors_.size();
    for (std::size_t i = 0; i < count && !queuedErrors_.empty(); ++i) {
        if (modalOpen() || quietDialogs() || !ownWindowForeground()) break;
        auto [error, icon] = std::move(queuedErrors_.front());
        queuedErrors_.erase(queuedErrors_.begin());
        messageBox(window_, error.c_str(), L"refarter", MB_OK | icon);
    }
    showingErrors_ = false;
    // A dialog's nested loop may have consumed the notification for newly arrived errors.
    // Focus loss leaves the unshown tail for activation; another question resumes it on dismissal.
    if (!modalOpen() && !quietDialogs() && ownWindowForeground()) {
        if (!queuedErrors_.empty()) PostMessageW(window_, errorDialogs, 0, 0); // The questions check dialogOpen themselves.
        if (storageQuestionQueued_) PostMessageW(window_, storageQuestion, 0, 0);
        if (state_.updateCheck == UpdateChoice::Ask) PostMessageW(window_, updateQuestion, 0, 0);
    }
}

LRESULT CALLBACK App::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        app->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    if (restoreMessage_ && message == restoreMessage_) { if (!closing_) restoreWindow(); return 0; }
    if (quitMessage_ && message == quitMessage_) { quit(); return 0; }
    if (commandMessage_ && message == commandMessage_) {
        // As the shortcuts do: save and screenshot only while replay runs, with their chime, and the window stays where it is.
        if (closing_) return 0;
        // While a shortcut is being assigned no press saves (see WM_HOTKEY), so neither does the jump list's.
        if (assigning_ && static_cast<LaunchCommand>(wParam) != LaunchCommand::Library) return 0;
        switch (static_cast<LaunchCommand>(wParam)) {
        case LaunchCommand::SaveReplay: shortcutPressed(saveShortcut); break;
        case LaunchCommand::Screenshot: shortcutPressed(screenshotShortcut); break;
        case LaunchCommand::Library: restoreWindow(); command(Library); break;
        default: break;
        }
        return 0;
    }
    if (tray_.taskbarCreated(message)) {
        // Explorer discarded the icon; recreate it without polling. Never
        // leave the app inaccessible if the shell cannot accept a new icon.
        tray_.forget();
        if ((state_.closeToTray || startupTray_) && !closing_ && !addTrayIcon()) restoreWindow();
        return 0;
    }
    switch (message) {
    case trayEvent:
        if (closing_ || waiting_ > 0 || HIWORD(lParam) != TrayIcon::id) return 0;
        if (LOWORD(lParam) == NIN_SELECT || LOWORD(lParam) == NIN_KEYSELECT ||
            LOWORD(lParam) == WM_LBUTTONDBLCLK) restoreWindow();
        else if (LOWORD(lParam) == WM_CONTEXTMENU) {
            const POINT point{static_cast<short>(LOWORD(wParam)), static_cast<short>(HIWORD(wParam))};
            // Built only when it opens; nothing is tracked in between.
            const bool detected = auto_.detectedAlive();
            const bool off = recording_ == capture::RecorderState::Off;
            const bool chosen = target_.source.window || target_.source.monitor;
            const auto& source = chosen ? target_ : auto_.detected();
            const auto sourceName = shownSourceName(source);
            auto line = sessionLine();
            if (!chosen) line.source.clear();
            TrayFlyoutModel model;
            model.live = recording_ == capture::RecorderState::Buffering;
            model.status = line.summary();
            model.action = !off ? (recording_ == capture::RecorderState::Stopped ? std::wstring(L"clear retained replay") : L"stop replay") :
                chosen || detected ? L"start replay · " + sourceName : L"choose a source to start";
            model.actionEnabled = !busy_ && (!off || chosen || detected);
            model.actionPrimary = off;
            model.useAutoCaptureEnabled = canUseAutoCapture();
            if (detected) auto_.fillTray(model);
            trayFlyout_.show(GetModuleHandleW(nullptr), point, std::move(model), [this](TrayCommand command) { trayCommand(command); });
        }
        return 0;
    case WM_HSCROLL: {
        const auto slider = reinterpret_cast<HWND>(lParam);
        if (!slider || !IsWindowEnabled(slider)) break;
        const int id = GetDlgCtrlID(slider);
        if (id != DesktopVolume && id != MicrophoneVolume && id != ChimeVolume) break;
        const auto percent = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
        if (id == DesktopVolume) audio_.levels.desktopPercent = percent;
        else if (id == MicrophoneVolume) audio_.levels.microphonePercent = percent;
        else chimes_.setPercent(percent);
        if (LOWORD(wParam) != TB_THUMBTRACK) saveSettings(); // Once per drag, on release.
        if (id == ChimeVolume && LOWORD(wParam) == TB_ENDTRACK) {
            chimes_.prepare(chimes_.percent());
            chimes_.play(Chimes::Kind::Saved, true); // Let the user hear the new level.
        }
        updateVolumeLabel(id);
        UpdateWindow(control(volumeLabelOf(id)));
        InvalidateRect(slider, nullptr, FALSE);
        return 0;
    }
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->code == TTN_GETDISPINFOW) {
            // An icon button's tooltip is its current text. Sidebar buttons show their
            // text while expanded, so only the collapsed sidebar needs tooltips.
            auto* info = reinterpret_cast<NMTTDISPINFOW*>(lParam);
            // Meter and locked-row ids are small numbers; a button's tool is its handle, which could collide with them.
            const bool isHandle = (info->uFlags & TTF_IDISHWND) != 0;
            if (!isHandle && isMeterTool(header->idFrom)) {
                wcsncpy_s(info->szText, meterToolText(header->idFrom), _TRUNCATE);
                return 0;
            }
            if (!isHandle && isLockedTool(header->idFrom)) {
                wcsncpy_s(info->szText, lockedToolText(), _TRUNCATE);
                return 0;
            }
            const auto button = reinterpret_cast<HWND>(header->idFrom);
            const int id = GetDlgCtrlID(button);
            info->szText[0] = 0;
            if (id == SaveReplay) {
                // Save always names its shortcut, which no longer shows on the Recording page.
                std::wstring tip = L"save replay";
                if (hotkeyRegistered_[saveShortcut]) tip += L" · " + describe(hotkeys_[saveShortcut]);
                wcsncpy_s(info->szText, tip.c_str(), _TRUNCATE);
            } else if (id == DesktopReset || id == MicrophoneReset) {
                wcsncpy_s(info->szText, L"reset to 100% and system default", _TRUNCATE);
            } else if (contains(videoResetIds, id)) {
                wcsncpy_s(info->szText, L"use the all-games value", _TRUNCATE);
            } else if (contains(libraryIconIds, id) || id == SidebarToggle || state_.sidebarCollapsed) {
                GetWindowTextW(button, info->szText, static_cast<int>(std::size(info->szText)));
            }
            return 0;
        }
        if (header->code == NM_CUSTOMDRAW &&
            (header->idFrom == DesktopVolume || header->idFrom == MicrophoneVolume || header->idFrom == ChimeVolume)) {
            const auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(lParam);
            // Native dragging can draw an individual item directly instead of
            // issuing a complete WM_PAINT. Cover both routes with one frame: inside a paint (pre-paint to post-paint)
            // the whole slider is composed once, in the post-paint, after anything the native control drew; an item
            // drawn on its own is composed there and then.
            if (draw->dwDrawStage == CDDS_PREPAINT) {
                sliderPainting_ = true;
                return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
            }
            if (draw->dwDrawStage == CDDS_ITEMPREPAINT) {
                if (!sliderPainting_) paintVolumeSlider(header->hwndFrom, draw->hdc);
                return CDRF_SKIPDEFAULT;
            }
            if (draw->dwDrawStage == CDDS_POSTPAINT) {
                sliderPainting_ = false;
                paintVolumeSlider(header->hwndFrom, draw->hdc);
                return CDRF_DODEFAULT;
            }
        }
        break;
    }
    case WM_MOUSEMOVE: case WM_MOUSELEAVE: {
        const POINT at{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
        // The tooltip sees the main window's moves only from here (its meter tools are not TTF_SUBCLASS): a disabled
        // button (a locked reset) gets no mouse messages, they come here, so a move over one is relayed as its own.
        // Only direct children are found, which every button is. Leaving the window pops the tip.
        if (tooltips_) {
            if (message == WM_MOUSELEAVE) {
                SendMessageW(tooltips_, TTM_POP, 0, 0);
            } else {
                const HWND under = ChildWindowFromPointEx(window_, at, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
                MSG relay{window_, message, wParam, lParam, static_cast<DWORD>(GetMessageTime())};
                relay.pt = at;
                ClientToScreen(window_, &relay.pt);
                if (under && under != window_ && !IsWindowEnabled(under) && !lockedTooltipAt(at)) {
                    POINT local = at;
                    MapWindowPoints(window_, under, &local, 1);
                    relay.hwnd = under;
                    relay.lParam = MAKELPARAM(local.x, local.y);
                }
                SendMessageW(tooltips_, TTM_RELAYEVENT, 0, reinterpret_cast<LPARAM>(&relay));
                TRACKMOUSEEVENT leave{sizeof leave, TME_LEAVE, window_, 0};
                TrackMouseEvent(&leave);
            }
        }
        // Above the footer (and on leaving the window) nothing is hot: clear the flags once and measure nothing.
        RECT client{};
        GetClientRect(window_, &client);
        const int footerTopPx = client.bottom - px(footerHeight);
        if (message != WM_MOUSEMOVE || closing_ || at.y < footerTopPx) {
            if (footerHot_ || updateHot_ || versionHot_) {
                footerHot_ = updateHot_ = versionHot_ = false;
                const RECT footer{0, footerTopPx, client.right, client.bottom};
                InvalidateRect(window_, &footer, FALSE);
            }
            return 0;
        }
        const RECT status = footerStatusBounds();
        const RECT update = footerUpdateBounds();
        const RECT version = footerVersionBounds();
        // The welcome screen holds everything else back, the footer's links too.
        const bool hot = !welcome_ && PtInRect(&status, at);
        const bool updateOver = !welcome_ && !IsRectEmpty(&update) && PtInRect(&update, at) &&
            updateClick() != UpdateView::Click::Nothing; // Progress is not a link.
        const bool versionOver = !welcome_ && PtInRect(&version, at);
        TRACKMOUSEEVENT track{sizeof track, TME_LEAVE, window_, 0}; // Only in the footer is there a flag to clear on leaving.
        TrackMouseEvent(&track);
        if (hot != footerHot_) { footerHot_ = hot; InvalidateRect(window_, &status, FALSE); }
        if (updateOver != updateHot_) { updateHot_ = updateOver; InvalidateRect(window_, &update, FALSE); }
        if (versionOver != versionHot_) { versionHot_ = versionOver; InvalidateRect(window_, &version, FALSE); }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && (footerHot_ || updateHot_ || versionHot_)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
    case WM_LBUTTONUP: {
        if (welcome_) return 0; // The footer's links wait for the welcome screen.
        POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
        const RECT status = footerStatusBounds();
        const RECT update = footerUpdateBounds();
        const RECT version = footerVersionBounds();
        if (!closing_ && waiting_ == 0 && !IsRectEmpty(&update) && PtInRect(&update, point)) { updateClicked(); return 0; }
        if (!closing_ && waiting_ == 0 && PtInRect(&version, point)) { command(ShowNotes); return 0; }
        if (!closing_ && waiting_ == 0 && PtInRect(&status, point)) {
            scroll_[static_cast<int>(Page::Recording)] = 0;
            if (state_.page == Page::Recording) placeControls(); else command(Recording);
            return 0;
        }
        return 0;
    }
    case WM_ACTIVATEAPP:
        if (wParam) { refreshStartup(); retryHotkeys(); refreshControls(); refreshPages(); } // The startup note, the source size and a shortcut another app held may have changed.
        // Returning from Explorer or a player: show clips added, renamed or deleted there.
        if (wParam && ((state_.page == Page::Library) || (state_.maxStorageGb && clipGrid_.scanOlderThan(std::chrono::seconds(30)))) && clipGrid_.scanned()) clipGrid_.scan(clipFolder_); // A limit needs a current size, but saves and bins already rescan: not on every activation.
        if (wParam && storageQuestionQueued_) PostMessageW(window_, storageQuestion, 0, 0); // Not inside the activation itself.
        if (wParam && notesDue_) PostMessageW(window_, notesDue, 0, 0); // This version's notes, if they waited for Refarter to be in front.
        if (wParam && state_.updateCheck == UpdateChoice::Ask) PostMessageW(window_, updateQuestion, 0, 0); // The opt-in question, if Refarter is in front.
        if (wParam && !queuedErrors_.empty()) PostMessageW(window_, errorDialogs, 0, 0); // Errors that arrived while a game was in front.
        if (wParam) PostMessageW(window_, meterForeground, 0, 0); // Meters stopped under a game run again; foreground settles first (the hook skips our own process).
        break;
    case WM_CREATE: {
        dpi_ = GetDpiForWindow(window_);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        const COLORREF caption = sidebar;
        DwmSetWindowAttribute(window_, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
        surfaceBrush_ = CreateSolidBrush(surface);
        editBrush_ = CreateSolidBrush(background);
        loadIcons();
        loadSettings();
        tray_.create(window_, trayEvent);
        restoreMessage_ = RegisterWindowMessageW(restoreMessageName);
        quitMessage_ = RegisterWindowMessageW(quitMessageName);
        commandMessage_ = RegisterWindowMessageW(commandMessageName);
        // Only a Refarter run as administrator (for shortcuts over an elevated game) opens the filter, so it still takes
        // these from the jump list, Explorer or a tool at the user's usual level, and Explorer's TaskbarCreated, which
        // Windows would otherwise drop without a word (UIPI). A normal Refarter keeps the filter: opening it would let
        // lower-integrity processes, such as browser sandboxes, post these messages too.
        if (runsElevated())
            for (const UINT handOver : {restoreMessage_, quitMessage_, commandMessage_, tray_.taskbarCreatedMessage()})
                if (handOver) ChangeWindowMessageFilterEx(window_, handOver, MSGFLT_ALLOW, nullptr);
        refreshStartup(); // Also sets the welcome screen's start with windows switch.
        if (state_.closeToTray) addTrayIcon();
        createControls();
        createFonts();
        // Library is the first page: list the folder (no decoding) on the library worker.
        if (!startupLaunch_ || state_.maxStorageGb) clipGrid_.scan(clipFolder_); // A limit needs the size even when the window stays hidden.
        RecordingController::Notify notify = [this](RecordingEvent event) {
            {
                std::lock_guard lock(eventsMutex_);
                events_.push_back(std::move(event));
            }
            PostMessageW(window_, recordingEvent, 0, 0);
        };
        recorder_ = makeRecorder_ ? makeRecorder_(std::move(notify)) : std::make_unique<RecordingController>(std::move(notify));
        startLeftoverCleanup(); // Settings are loaded, so the clip folder is known.
        scheduleUpdateCheck(startupUpdateDelay); // Opt-in and at most daily; one worker that sleeps first. No timer.
        hotkeys_[saveShortcut] = parseHotkey(config_.get(L"shortcuts", L"save_replay")).value_or(defaultHotkey);
        hotkeys_[screenshotShortcut] = parseHotkey(config_.get(L"shortcuts", L"screenshot")).value_or(defaultScreenshotHotkey);
        registerHotkey(saveShortcut, hotkeys_[saveShortcut]);
        // A save shortcut set to the screenshot's default before screenshots existed: say so, rather than blame another app.
        if (hotkeys_[screenshotShortcut] == hotkeys_[saveShortcut])
            hotkeyNotes_[screenshotShortcut] = clashNote(screenshotShortcut);
        else registerHotkey(screenshotShortcut, hotkeys_[screenshotShortcut]);
        auto_.attach(window_, autoHost());
        auto_.updateHook();
        refreshControls();
        layout();
        return 0;
    }
    case WM_MOUSEWHEEL:
        // Buttons, fields and sliders pass the wheel up here; Recording and Settings scroll.
        if (state_.page != Page::Library && !fullscreen_.on) {
            scrollPage(-GET_WHEEL_DELTA_WPARAM(wParam) * wheelStep / WHEEL_DELTA);
            return 0;
        }
        break;
    case WM_APPCOMMAND:
        // The mouse's back button or a keyboard's browser back key, passed up from whichever child is under the pointer
        // or focused, steps back out of the player as Esc does. The library grid uses the mouse button itself for folders.
        if (GET_APPCOMMAND_LPARAM(lParam) == APPCOMMAND_BROWSER_BACKWARD && playerOpen_) {
            if (!closing_ && waiting_ == 0) player_.back(); // A release still arrives during a bounded wait.
            return TRUE;
        }
        break;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) layout();
        else hideGateBubble();
        syncMeters(); // Minimised: nothing is shown.
        return 0;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        // The Recording and Settings pages scroll, so this is no longer their height; it keeps the sidebar and the
        // library laid out above the footer. The width keeps the trim bar on one row for a clip with four audio sources,
        // beside the expanded sidebar (see computeLayout): 836 px of player + 196 sidebar + 2 x 24 gaps.
        RECT minimum{0, 0, px(1080), px(680)};
        AdjustWindowRectExForDpi(&minimum, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
        limits->ptMinTrackSize = {minimum.right - minimum.left, minimum.bottom - minimum.top};
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wParam);
        hideSliderBubble();
        hideGateBubble();
        createFonts();
        loadIcons();
        player_.setDpi(dpi_);
        const auto* suggested = reinterpret_cast<RECT*>(lParam);
        RECT bounds = *suggested;
        // Fullscreen keeps covering the monitor it moved to, not the windowed size the system suggests.
        if (MONITORINFO monitor{sizeof(monitor)}; fullscreen_.on && GetMonitorInfoW(MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST), &monitor))
            bounds = monitor.rcMonitor;
        SetWindowPos(window_, nullptr, bounds.left, bounds.top,
            bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_COMMAND:
        // A bounded shutdown or sleep wait: no menu or folder dialog may open and hold it, so user commands are dropped;
        // internal notifications (clipsChanged, EN_CHANGE) still run.
        if (waiting_ > 0 && (HIWORD(wParam) == BN_CLICKED || (LOWORD(wParam) == Clips && (HIWORD(wParam) == ClipGrid::clipOpened || HIWORD(wParam) == ClipGrid::clipMenu)))) return 0;
        if (LOWORD(wParam) == Clips && HIWORD(wParam) == ClipGrid::clipOpened) openClip(clipGrid_.target());
        else if (LOWORD(wParam) == Clips && HIWORD(wParam) == ClipGrid::clipMenu) showClipMenu();
        else if (LOWORD(wParam) == Clips && HIWORD(wParam) == ClipGrid::askChanged) {
            // The question's "don't ask again" box was ticked and answered yes: keep it in the file and on the switch.
            state_.askBeforeBinning = clipGrid_.askBeforeBinning();
            saveSettings();
            refreshControls();
            InvalidateRect(control(AskBeforeBinning), nullptr, FALSE);
        }
        else if (LOWORD(wParam) == Clips && HIWORD(wParam) == ClipGrid::noteChanged) {
            invalidateStrip(); // The strip under the card shows the note.
        }
        else if (LOWORD(wParam) == Clips && HIWORD(wParam) == ClipGrid::clipsChanged) {
            invalidateStrip(); // The worker's scan carried the folder size and the drive's free space.
            if (!clipGrid_.count() && GetFocus() == control(Clips))
                SetFocus(control(state_.clipView == ClipView::Favourites ? Favourites : state_.clipView == ClipView::Compressed ? Compressed : AllClips));
            placeControls(); // The grid shows only with clips; the empty-state text is painted.
        }
        else if (LOWORD(wParam) == CustomSeconds && HIWORD(wParam) == EN_CHANGE) updateReplayInput();
        else if (LOWORD(wParam) == Bitrate && HIWORD(wParam) == EN_CHANGE) updateBitrateInput();
        else if (LOWORD(wParam) == ClipSearch && HIWORD(wParam) == EN_CHANGE) updateSearch();
        else if (LOWORD(wParam) == StorageLimitField && HIWORD(wParam) == EN_CHANGE) updateStorageInput();
        else if (LOWORD(wParam) >= GateOpen && LOWORD(wParam) <= GateRelease && HIWORD(wParam) == EN_CHANGE) updateGateInput();
        else if (HIWORD(wParam) == BN_CLICKED) command(LOWORD(wParam));
        return 0;
    case WM_INPUT: {
        // A watched-only key ("=", "r") works in Refarter too, except where Refarter uses the key itself: the
        // player ("=" zooms), an open popup menu (first-letter pick; it holds the mouse capture), a text field
        // (search, rename, number fields) and the clip grid's letters (F favourites).
        const auto typesHere = [&](UINT key) {
            if (!ownProcessInFront()) return false;
            if (GetCapture()) return true;
            const HWND focus = GetFocus();
            // The tray flyout is not owned by the main window and has no such keys.
            if (playerOpen_ && GetAncestor(GetForegroundWindow(), GA_ROOTOWNER) == window_) return true;
            if (focus == control(Clips) && key >= 'A' && key <= 'Z') return true;
            wchar_t kind[16]{};
            return focus && GetClassNameW(focus, kind, static_cast<int>(std::size(kind))) && _wcsicmp(kind, L"Edit") == 0;
        };
        if (const int shortcut = rawHotkey_.input(reinterpret_cast<HRAWINPUT>(lParam));
            shortcut >= 0 && !(watchedOnly(hotkeys_[shortcut]) && typesHere(hotkeys_[shortcut].key)) &&
            pressReports_[shortcut].accept(true, GetTickCount64())) shortcutPressed(shortcut);
        // Foreground raw input requires DefWindowProc cleanup.
        return DefWindowProcW(window_, message, wParam, lParam);
    }
    case WM_INPUT_DEVICE_CHANGE:
        if (rawHotkey_.active()) rawHotkey_.prime(); // Avoid a stuck press after unplug/reconnect.
        return 0;
    case WM_HOTKEY:
        // Raw Input misses presses while an elevated window is in front; some games block this
        // path instead. Either path saves, and the second report of one press is dropped.
        if (wParam >= 1 && wParam <= shortcutCount) {
            const int shortcut = static_cast<int>(wParam) - 1;
            // While the other shortcut is being assigned this one stays registered, so pressing it lands here, not as keys.
            if (assigning_) {
                if (shortcut != assigningShortcut_) {
                    hotkeyNotes_[assigningShortcut_] = describe(hotkeys_[shortcut]) +
                        (shortcut == saveShortcut ? L" is the save shortcut · try another" : L" is the screenshot shortcut · try another");
                    refreshAssigning();
                }
            } else if (pressReports_[shortcut].accept(false, GetTickCount64())) {
                shortcutPressed(shortcut);
            }
        }
        return 0;
    case recordingEvent: receiveEvents(); return 0;
    case storageQuestion: askQueuedStorageQuestion(); return 0;
    case updateChecked: updateCheckDone(); return 0;
    case notesDue: showNotesIfDue(); return 0;
    case signInReplayDue: startSignInReplay(); return 0;
    case updateQuestion: askUpdateQuestion(); return 0;
    case updateDownloaded: downloadDone(); return 0;
    case updateSetupEnded: setupEnded(); return 0;
    case updateProgress:
        if (download_.current(lParam) && static_cast<int>(wParam) != downloadPercent_) { // Not a late one from a cancelled download.
            downloadPercent_ = static_cast<int>(wParam);
            if (downloadBox_) SendMessageW(downloadBox_, progressPercent, wParam, 0); // It shows the percent; the footer catches up when it closes.
            else invalidateFooter();
        }
        return 0;
    case errorDialogs: showQueuedErrors(); return 0;
    case AutoSession::gameExited:
        if (auto_.clearExitedGameReplay()) {
            refreshControls();
            refreshPages();
        }
        return 0;
    case AutoSession::foregroundCheck: auto_.onForeground(GetForegroundWindow()); return 0;
    case WM_TIMER:
        if (wParam == AutoSession::sizeWatchTimer) auto_.unwatchSize();
        if (wParam == exportTimer) receiveCompression();
        if (wParam == switchTimer) stepSwitches();
        if (wParam == meterTimer) stepMeters();
        if (wParam == bubbleTimer && !bubbleDrag_) hideSliderBubble();
        if (wParam == gateBubbleTimer) hideGateBubble();
        if (wParam == resumeTimer) { KillTimer(window_, resumeTimer); resumeDue_ = true; finishResume(); }
        if (wParam == signInReplayTimer) startSignInReplay();
        return 0;
    case WM_SHOWWINDOW:
        DefWindowProcW(window_, message, wParam, lParam);
        if (!wParam) hideGateBubble();
        // Sent before the visibility changes: recheck afterwards, so hiding to the tray
        // stops the timer and closes any meter-only capture.
        PostMessageW(window_, meterSync, 0, 0);
        return 0;
    case meterSync: syncMeters(); return 0;
    case meterForeground: meterForegroundChanged(); return 0;
    case leftoversDone: leftoversHandled(static_cast<int>(wParam), static_cast<int>(lParam)); return 0;
    case WM_POWERBROADCAST: powerBroadcast(wParam); return TRUE;
    case WM_QUERYENDSESSION: return queryEndSession(lParam) ? TRUE : FALSE;
    case WM_ENDSESSION: endSession(wParam != 0, lParam); return 0;
    case afterToggle: afterToggleChanged(static_cast<int>(wParam)); return 0;
    case WM_CLOSE:
        closePlayer(); // Hidden in the tray or quitting: playback stops either way.
        if (!closing_ && state_.closeToTray) {
            if (addTrayIcon()) ShowWindow(window_, SW_HIDE);
            else report(L"windows could not create the tray icon. refarter will stay open.", MB_ICONERROR); // Waits while clips are ordered.
            return 0;
        }
        quit();
        return 0;
    case WM_CTLCOLOREDIT: {
        auto dc = reinterpret_cast<HDC>(wParam);
        const int field = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
        if (contains(typedVolumeIds, field)) {
            // A source's percentage sits on the card like plain text; it is brighter while typed in.
            SetTextColor(dc, typingVolume_ == field ? ink : muted);
            SetBkColor(dc, surface);
            return reinterpret_cast<LRESULT>(surfaceBrush_);
        }
        // A value the chosen game sets itself is in the accent colour, as on its drop-down rows.
        SetTextColor(dc, resetOf(field) && scopeOwns(resetOf(field)) ? accent : ink);
        SetBkColor(dc, background);
        return reinterpret_cast<LRESULT>(editBrush_);
    }
    case WM_CTLCOLORSTATIC: {
        auto dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, muted);
        // Disabled edits ask for static colours; keep them on their input background.
        const bool typed = contains(typedVolumeIds, GetDlgCtrlID(reinterpret_cast<HWND>(lParam)));
        wchar_t kind[8]{};
        GetClassNameW(reinterpret_cast<HWND>(lParam), kind, static_cast<int>(std::size(kind)));
        if (!typed && lstrcmpiW(kind, L"Edit") == 0) {
            SetBkColor(dc, background);
            return reinterpret_cast<LRESULT>(editBrush_);
        }
        SetBkColor(dc, surface);
        return reinterpret_cast<LRESULT>(surfaceBrush_);
    }
    case WM_DRAWITEM:
    {
        // Composed off-screen and copied once, so a hover or an animation frame never shows the bare backdrop.
        auto item = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        const HDC target = item.hDC;
        const RECT box = item.rcItem;
        if (box.left == 0 && box.top == 0 && box.right > 0 && box.bottom > 0) {
            if (const HDC memory = buttonBuffer_.acquire(target, box.right, box.bottom)) {
                item.hDC = memory;
                drawButton(item);
                buttonBuffer_.present(target);
                return TRUE;
            }
        }
        drawButton(item);
        return TRUE;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window_, &ps);
        RECT client;
        GetClientRect(window_, &client);
        // Only the invalidated part is drawn; the buffer is copied through the paint region's clip.
        if (const HDC buffer = backBuffer_.acquire(dc, client.right, client.bottom)) {
            IntersectClipRect(buffer, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);
            paint(buffer, ps.rcPaint);
            SelectClipRgn(buffer, nullptr);
            backBuffer_.present(dc);
        } else {
            paint(dc, ps.rcPaint);
        }
        EndPaint(window_, &ps);
        return 0;
    }
    case WM_DESTROY:
        releaseLeftoverTask(); // Before the window dies, so the cleanup thread never posts to a dead or reused handle.
        backBuffer_.release();
        removeTrayIcon();
        auto_.shutdown();
        trayFlyout_.hide();
        bubble_.destroy();
        gateBubble_.destroy();
        for (auto font : fonts_) if (font) DeleteObject(font);
        fonts_.fill(nullptr);
        DeleteObject(surfaceBrush_);
        DeleteObject(editBrush_);
        if (largeIcon_) DestroyIcon(largeIcon_);
        if (smallIcon_) DestroyIcon(smallIcon_);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

App::~App() {
    finishLeftoverCleanup();
    compression_.stop();
    auto_.unwatchGameExit(); recorder_.reset(); // Join callbacks before the mailbox is destroyed.
    watchMeterForeground(false); // Normally gone already (quit() syncs the meters); never leave a hook behind.
    closeMeterCaptures();
    joinMeterClosers(); // Every meter capture thread has ended before the meters, COM and the process go.
    diagnosticLog().write(DiagnosticEvent::SessionEnded);
    diagnosticLog().finish();
}

bool App::create(HINSTANCE instance, bool startup) {
    startupLaunch_ = startup;
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&common);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_REFARTER));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"RefarterDesktopShell";
    if (!RegisterClassExW(&wc)) return false;
    dpi_ = GetDpiForSystem();
    RECT bounds{0, 0, px(initialClientWidth), px(initialClientHeight)};
    AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
    window_ = CreateWindowExW(0, wc.lpszClassName, L"refarter",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, instance, this);
    if (!window_) {
        MessageBoxW(nullptr, L"the window could not be created.", L"refarter", MB_ICONERROR);
        return false;
    }
    // Handled from the message loop, once run() has shown the window or tray icon; ordinary launches never start
    // replay. Posted here rather than in run() so that tests/app_sign_in_replay_test.cpp drives the real launch. On a
    // visible --startup launch it now comes before notesDue and updateQuestion, which is harmless: the sign-in start
    // only reports through the status, never a dialog, and either dialog's modal loop would have dispatched it while open anyway.
    if (startup && state_.signInReplay) {
        signInPending_ = PostMessageW(window_, signInReplayDue, 0, 0) != FALSE;
    }
    return true;
}

int App::run(HINSTANCE instance, int showCommand, bool startup) {
    if (!create(instance, startup)) return 1;
    startupTray_ = startup && state_.startMinimised && addTrayIcon();
    // An unanswered welcome screen stays pending: write that now, so a crash or a kill before the next save does not
    // lose it.
    if (welcome_) saveSettings();
    if (!startupTray_) {
        // Failed tray creation must never leave an inaccessible hidden process.
        ShowWindow(window_, startup ? SW_SHOW : showCommand);
        UpdateWindow(window_);
        // The taskbar's save replay, screenshot and open library, once per start, after the window is up; a hidden
        // sign-in start leaves this shell work for the first time its window opens (restoreWindow).
        registerJumpList(executablePath());
        SetFocus(control(!welcome_ ? Library : IsWindowEnabled(control(WelcomeStart)) ? WelcomeStart : WelcomeSkip));
        if (notesDue_) PostMessageW(window_, notesDue, 0, 0); // Before the opt-in question: the notes come first.
        PostMessageW(window_, updateQuestion, 0, 0);
        if (!clipGrid_.scanned()) clipGrid_.scan(clipFolder_);
    }
    MSG message{};
    BOOL status;
    while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        // While assigning, every key press is a candidate shortcut, including Alt and F10 system keys.
        const bool keyDown = message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
        const bool keyUp = message.message == WM_KEYUP || message.message == WM_SYSKEYUP;
        if (assigning_ && keyDown) {
            assignKey(static_cast<UINT>(message.wParam));
            continue;
        }
        if (assigning_ && keyUp && isModifierKey(static_cast<UINT>(message.wParam))) modifiersChanged();
        if (assigning_ && (keyUp || message.message == WM_CHAR || message.message == WM_SYSCHAR)) continue;
        if (keyUp && releaseKey_ == message.wParam) {
            releaseKey_ = 0;
            continue;
        }
        if (message.message == WM_KEYDOWN && clipGrid_.renameKey(message.wParam)) continue; // A clip name is being typed.
        if (message.message == WM_KEYDOWN && player_.renameKey(message.wParam)) continue; // Or in the player.
        // The player handles its own Escape and Enter (leave fullscreen, save a trim).
        const bool inPlayer = playerOpen_ && GetFocus() == control(Player);
        if (message.message == WM_KEYDOWN && GetFocus() == control(ClipSearch) && searchKey(message.wParam)) continue;
        // Ctrl+F on the library goes to the search (the grid's own F, the favourite, needs no Ctrl). Not while a clip is
        // being renamed: leaving the name field would rename the file to what is typed so far.
        if (message.message == WM_KEYDOWN && message.wParam == 'F' && (GetKeyState(VK_CONTROL) & 0x8000) &&
            state_.page == Page::Library && !playerOpen_ && !clipGrid_.renaming() && IsWindowVisible(control(ClipSearch))) {
            SetFocus(control(ClipSearch));
            continue;
        }
        // A key for another window of this thread (the tray flyout) is that window's:
        // the Esc, Tab and Enter routing below is the main window's own and must not take it.
        if (keyDown && message.hwnd && GetAncestor(message.hwnd, GA_ROOT) != window_) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            continue;
        }
        // Enter, Esc or a press anywhere else in the window (empty space too, which takes no focus) leaves a text field with
        // nothing focused. Its WM_KILLFOCUS saves a number or commits a volume; a clicked control then takes focus itself.
        if (const HWND field = focusedTextField()) {
            const bool leaveKey = message.message == WM_KEYDOWN && (message.wParam == VK_RETURN || message.wParam == VK_ESCAPE);
            const bool pressElsewhere = isMousePress(message.message) && message.hwnd != field && (message.hwnd == window_ || IsChild(window_, message.hwnd));
            if (leaveKey || pressElsewhere) SetFocus(window_);
            if (leaveKey) continue;
        }
        // A repeat is ignored: holding Esc to cancel assigning the shortcut must not skip the screen too.
        if (welcome_ && message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE && !(message.lParam & (1 << 30))) {
            finishWelcome(false); // Esc skips the welcome screen.
            continue;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE && !inPlayer) {
            // Esc clears a selection first, then leaves an open folder.
            if (GetFocus() == control(Clips) && (clipGrid_.cancelBand() || clipGrid_.clearSelection() || clipGrid_.leaveFolder())) continue;
            continue;
        }
        setKeyboardFocus(keyDown ? (message.wParam == VK_TAB || (message.wParam >= VK_LEFT && message.wParam <= VK_DOWN) || message.wParam == VK_SPACE || message.wParam == VK_RETURN) || keyboardFocus_ :
            (message.message == WM_LBUTTONDOWN || message.message == WM_RBUTTONDOWN || message.message == WM_NCLBUTTONDOWN) ? false : keyboardFocus_);
        if (message.message == WM_KEYDOWN && message.wParam == VK_TAB) {
            const HWND from = GetFocus() == window_ ? nullptr : GetFocus(); // The window holds focus after a click-away.
            auto next = GetNextDlgTabItem(window_, from, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            if (next) SetFocus(next);
            continue;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN && !inPlayer) {
            const auto focused = GetFocus();
            if (focused && GetParent(focused) == window_ && IsWindowEnabled(focused)) SendMessageW(focused, BM_CLICK, 0, 0);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return status == -1 ? 1 : static_cast<int>(message.wParam);
}

} // namespace refarter
