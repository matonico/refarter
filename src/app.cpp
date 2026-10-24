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
constexpr UINT storageQuestion = WM_APP + 8; // Asks the storage question that a save over a game had to leave for later.
constexpr int initialClientWidth = 1120, initialClientHeight = 760;

} // namespace

bool App::addTrayIcon() { return tray_.present() || tray_.add(smallIcon_, trayTip()); }

void App::removeTrayIcon() { tray_.remove(); }

void App::restoreWindow() {
    const bool firstStartupOpen = startupTray_;
    startupTray_ = false;
    if (!state_.closeToTray) removeTrayIcon();
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
    if (state_.updateCheck == UpdateChoice::Ask) PostMessageW(window_, updateQuestion, 0, 0); // Asked now if this brought Refarter to the front.
    if (!queuedErrors_.empty()) PostMessageW(window_, errorDialogs, 0, 0); // Likewise the errors that waited for it.
    if (!clipGrid_.scanned()) clipGrid_.scan(clipFolder_);
}

void App::quit() {
    if (closing_) return;
    saveSettings();
    closing_ = true;
    syncMeters(); // Closing: the timer and any meter-only capture end.
    compression_.cancel();
    updates_.cancel(); // A check in flight ends on its own and posts nothing.
    auto_.unwatchGameExit();
    rawHotkey_.configure(window_, {}, false);
    setStatus(L"finishing pending saves and closing…");
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
    case DiagnosticLogging: return state_.diagnosticLogging;
    case CompressCopyAfterTrim: return state_.compressCopyAfterTrim;
    case AskBeforeBinning: return state_.askBeforeBinning;
    case UpdateCheckSwitch: return state_.updateCheck == UpdateChoice::On; // "ask" counts as off until answered.
    default: return false;
    }
}

void App::navigate(Page page) {
    if (page != Page::Library && playerOpen_) closePlayer(); // Leaving the library stops playback.
    // Move focus before hiding page controls so keyboard traversal remains usable.
    state_.page = page;
    SetFocus(control(page == Page::Library ? Library : page == Page::Recording ? Recording : Settings));
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
    case AllClips: case Favourites: case Compressed:
        state_.clipView = id == AllClips ? ClipView::All : id == Favourites ? ClipView::Favourites : ClipView::Compressed;
        clipGrid_.setView(state_.clipView);
        placeControls(); // The chosen tab is taller; the empty-state text follows the view.
        redrawn = true;
        break;
    case ThirtySeconds: state_.replaySeconds = 30; break;
    case OneMinute: state_.replaySeconds = 60; break;
    case ThreeMinutes: state_.replaySeconds = 180; break;
    case FiveMinutes: state_.replaySeconds = 300; break;
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
        break;
    case StartMinimised:
        if (!state_.startWithWindows || startupReadFailed_) return;
        state_.startMinimised = !state_.startMinimised;
        break;
    case DiagnosticLogging:
        if (!configureDiagnostics(!state_.diagnosticLogging)) {
            setStatus(L"diagnostic logging could not start");
            return;
        }
        break;
    case SelectProcess: chooseSource(); break;
    case AutoCaptureSource: chooseAutoCapture(); break;
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
    case AskBeforeBinning:
        state_.askBeforeBinning = !state_.askBeforeBinning;
        clipGrid_.setAskBeforeBinning(state_.askBeforeBinning);
        break;
    case UpdateCheckSwitch:
        state_.updateCheck = state_.updateCheck == UpdateChoice::On ? UpdateChoice::Off : UpdateChoice::On;
        if (state_.updateCheck == UpdateChoice::On) scheduleUpdateCheck(std::chrono::seconds(0));
        else {
            if (updates_.running() && !updateManual_) updates_.cancel(); // A pending automatic check is dropped.
            updateTried_ = false; // Switching it on again in this session may check again (the daily rule still applies).
            updateTag_.clear();
            updateUrl_.clear();
            updateNote_.clear();
        }
        break;
    case UpdateCheckNow:
        if (!updateTag_.empty()) { openUpdatePage(); return; } // A newer version is known: the button opens its page.
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
    case ChooseFolder: chooseFolder(); break;
    case OpenFolder:
        if (!clipFolder_.empty())
            shellOpen(clipFolder_.c_str(), L"the folder is not available yet. choose a folder in settings or start replay first.", MB_ICONINFORMATION);
        break;
    default: return;
    }
    if (id >= ThirtySeconds && id <= FiveMinutes) {
        state_.replayInputValid = true;
        setTextIfChanged(control(CustomSeconds), std::to_wstring(state_.replaySeconds));
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

void App::afterToggleChanged(int) {
    if (closing_) return;
    saveSettings();
    refreshControls();
    refreshPages(); // The noise gate adds or removes rows; discord and the startup note change text in place.
}

capture::Size App::sourceSize() const {
    RECT client{};
    if (target_.source.window && GetClientRect(target_.source.window, &client)) return {client.right, client.bottom};
    MONITORINFO info{sizeof(info)};
    const auto monitor = target_.source.monitor ? target_.source.monitor : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (GetMonitorInfoW(monitor, &info)) return {info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top};
    return {1920, 1080};
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

bool App::startReplay() {
    if (busy_ || closing_ || endingSession_ || recording_ != capture::RecorderState::Off) return false;
    revalidateMonitor(); // The displays may have changed since the choice (wake, hotplug): never start on a stale handle.
    if ((!target_.source.window && !target_.source.monitor) || !state_.replayInputValid ||
        (!bitrateValid_ && video_.quality == capture::Quality::Custom) ||
        (audio_.microphone.enabled() && audio_.micFilters.noiseGate && !gateInputValid())) return false;
    video_.replaySeconds = state_.replaySeconds;
    saveSettings(); // Number fields are written when left; starting a replay counts as leaving them.
    busy_ = true;
    closeMeterCaptures(); // Hand the devices over: the recorder opens its own and feeds the same meters.
    audio_.desktopMeter = &desktopMeter_;
    audio_.microphoneMeter = &microphoneMeter_;
    // A minimised game is waited for (up to ten seconds) until it is restored.
    setStatus(target_.source.window && IsIconic(target_.source.window) ?
        L"return to your game · waiting for capture…" : L"starting replay…");
    recorder_->start(target_, video_, audio_, clipFolder_);
    return true;
}

void App::stopReplay() {
    if (busy_ || closing_ || recording_ == capture::RecorderState::Off) return;
    auto_.onManualStop(target_, recording_ == capture::RecorderState::Buffering);
    busy_ = true;
    setStatus(L"stopping replay…");
    recorder_->stop();
}

AutoSession::Context App::autoContext() const {
    return {closing_ || endingSession_, busy_ || asleep_, recording_, &target_, autoCaptureOn_, autoCapturePrompt_, autoCaptureReadable_, &autoCapture_};
}

bool App::startAutoReplay(RecordingTarget target) {
    auto previous = std::move(target_);
    target_ = std::move(target);
    if (startReplay()) return true;
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
    if (writeAutoCapture(autoCapturePath(), list)) autoCapture_ = std::move(list);
    else setStatus(L"the auto-capture list could not be saved");
    refreshControls();
    InvalidateRect(window_, nullptr, FALSE);
}

bool App::canUseAutoCapture() const {
    return !pickerStale() && !saving_ && !asleep_ && !endingSession_ && autoCaptureReadable_;
}

void App::useAutoCapture() {
    // Recheck after a menu closes: an automatic start or shutdown may have happened inside it.
    if (!canUseAutoCapture()) return;
    target_ = {};
    sourceMonitor_.clear();
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
    case TrayCommand::UseAutoCapture: useAutoCapture(); return;
    case TrayCommand::AutoCapture: {
        if (!detected) return;
        const bool listed = autoCapture_.find(auto_.detected().path) != nullptr;
        setAutoCaptured(auto_.detected().path, auto_.detected().displayName, !listed);
        if (!listed) auto_.tryAutoCapture(auto_.detected()); // Added while it runs: start now, as if it came to the front.
        break;
    }
    case TrayCommand::Action:
        if (recording_ != capture::RecorderState::Off) stopReplay();
        else {
            if (!target_.source.window && !target_.source.monitor && detected) {
                target_ = auto_.detected();
                sourceMonitor_.clear();
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
    const auto& label = target_.displayName.empty() ? target_.name : target_.displayName;
    line.source = lower(label);
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

std::wstring App::saveName() const {
    return target_.source.window ? friendlySourceName(target_.title, target_.name) : lower(target_.name);
}

void App::saveReplay() {
    if (recording_ == capture::RecorderState::Off || busy_ || saving_ || closing_ || asking_ || showingErrors_) return; // asking_: a second press under the question.
    // The clip ends at the press: the save worker may start seconds later (a queue, a sleeping clip drive).
    const auto requestedAt = capture::captureClock();
    std::wstring note;
    if (!storageAllowsSave(note)) { // The status says why; the replay is kept.
        if (!quietDialogs()) chimes_.play(Chimes::Kind::Failed, false); // The game is likely in front.
        return;
    }
    saving_ = true;
    setStatus(note.empty() ? L"saving replay…" : note);
    recorder_->save(clipFolder_, saveName(), requestedAt);
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
            if (event.success) clipGrid_.scan(clipFolder_); // The new clip goes to the front.
            else {
                // The replay survives a failed save only if nothing is stopping, quitting or clearing it meanwhile.
                event.message = failedSaveText(event.message, replayKeptNow());
                // Heard over the game; silent while sleep, sign-out or quitting waits for the save.
                sounded = !quietDialogs();
                if (sounded) chimes_.play(Chimes::Kind::Failed, false);
            }
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
        const bool quiet = (automatic && automatic->quiet) || sleepStart;
        setStatus(automatic ? std::move(automatic->status) : lower(event.message));
        if (event.sleeping && asleep_) {
            sleep_.kept = event.state == capture::RecorderState::Stopped;
            setStatus(sleep_.kept ? L"paused for sleep · replay kept" : L"replay paused for sleep");
        }
        if (sleepStart) {
            sleepRestart_ = false;
            setStatus(event.success ? L"restarted after sleep" : L"restart failed · enable replay");
        }
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
        if (queuedErrors_.size() < maxQueuedErrors && std::none_of(queuedErrors_.begin(), queuedErrors_.end(), [&](const auto& queued) { return queued.first == error; }))
            queuedErrors_.emplace_back(error, sounded);
    }
    showQueuedErrors();
}

// Refarter's window came to the front: the error messages that arrived meanwhile, now where a dialog is welcome.
void App::showQueuedErrors() {
    if (queuedErrors_.empty() || showingErrors_ || asking_ || quietDialogs() || !ownWindowForeground()) return;
    showingErrors_ = true;
    // One bounded batch. Nested messages append to the same queue without opening another dialog.
    const auto count = queuedErrors_.size();
    for (std::size_t i = 0; i < count && !queuedErrors_.empty(); ++i) {
        if (asking_ || quietDialogs() || !ownWindowForeground()) break;
        auto [error, sounded] = std::move(queuedErrors_.front());
        queuedErrors_.erase(queuedErrors_.begin());
        messageBox(window_, error.c_str(), L"refarter", sounded ? MB_OK : MB_OK | MB_ICONERROR);
    }
    showingErrors_ = false;
    // A dialog's nested loop may have consumed the notification for newly arrived errors.
    // Focus loss leaves the unshown tail for activation; another question resumes it on dismissal.
    if (!asking_ && !quietDialogs() && ownWindowForeground()) {
        if (!queuedErrors_.empty()) PostMessageW(window_, errorDialogs, 0, 0);
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
            const auto sourceName = lower(source.displayName.empty() ? source.name : source.displayName);
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
            if (isMeterTool(header->idFrom)) {
                wcsncpy_s(info->szText, meterToolText(header->idFrom), _TRUNCATE);
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
        // Above the footer (and on leaving the window) nothing is hot: clear the flags once and measure nothing.
        RECT client{};
        GetClientRect(window_, &client);
        const int footerTopPx = client.bottom - px(footerHeight);
        if (message != WM_MOUSEMOVE || closing_ || at.y < footerTopPx) {
            if (footerHot_ || updateHot_) {
                footerHot_ = updateHot_ = false;
                const RECT footer{0, footerTopPx, client.right, client.bottom};
                InvalidateRect(window_, &footer, FALSE);
            }
            return 0;
        }
        const RECT status = footerStatusBounds();
        const RECT update = footerUpdateBounds();
        const bool hot = PtInRect(&status, at);
        const bool updateOver = !IsRectEmpty(&update) && PtInRect(&update, at);
        TRACKMOUSEEVENT track{sizeof track, TME_LEAVE, window_, 0}; // Only in the footer is there a flag to clear on leaving.
        TrackMouseEvent(&track);
        if (hot != footerHot_) { footerHot_ = hot; InvalidateRect(window_, &status, FALSE); }
        if (updateOver != updateHot_) { updateHot_ = updateOver; InvalidateRect(window_, &update, FALSE); }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && (footerHot_ || updateHot_)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
    case WM_LBUTTONUP: {
        POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
        const RECT status = footerStatusBounds();
        const RECT update = footerUpdateBounds();
        if (!closing_ && waiting_ == 0 && !IsRectEmpty(&update) && PtInRect(&update, point)) { openUpdatePage(); return 0; }
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
        if (wParam && state_.updateCheck == UpdateChoice::Ask) PostMessageW(window_, updateQuestion, 0, 0); // The opt-in question, if Refarter is in front.
        if (wParam && !queuedErrors_.empty()) PostMessageW(window_, errorDialogs, 0, 0); // Errors that arrived while a game was in front.
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
        refreshStartup();
        if (state_.closeToTray) addTrayIcon();
        createControls();
        createFonts();
        // Library is the first page: list the folder (no decoding) on the library worker.
        if (!startupLaunch_ || state_.maxStorageGb) clipGrid_.scan(clipFolder_); // A limit needs the size even when the window stays hidden.
        recorder_ = std::make_unique<RecordingController>([this](RecordingEvent event) {
            {
                std::lock_guard lock(eventsMutex_);
                events_.push_back(std::move(event));
            }
            PostMessageW(window_, recordingEvent, 0, 0);
        });
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
        // library laid out above the footer.
        RECT minimum{0, 0, px(960), px(740)};
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
    case WM_INPUT:
        if (const int shortcut = rawHotkey_.input(reinterpret_cast<HRAWINPUT>(lParam));
            shortcut >= 0 && pressReports_[shortcut].accept(true, GetTickCount64())) shortcutPressed(shortcut);
        // Foreground raw input requires DefWindowProc cleanup.
        return DefWindowProcW(window_, message, wParam, lParam);
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
    case updateQuestion: askUpdateQuestion(); return 0;
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
        return 0;
    case WM_SHOWWINDOW:
        DefWindowProcW(window_, message, wParam, lParam);
        if (!wParam) hideGateBubble();
        // Sent before the visibility changes: recheck afterwards, so hiding to the tray
        // stops the timer and closes any meter-only capture.
        PostMessageW(window_, meterSync, 0, 0);
        return 0;
    case meterSync: syncMeters(); return 0;
    case leftoversDone: leftoversHandled(static_cast<int>(wParam), static_cast<int>(lParam)); return 0;
    case WM_POWERBROADCAST: powerBroadcast(wParam); return TRUE;
    case WM_QUERYENDSESSION: return queryEndSession(lParam) ? TRUE : FALSE;
    case WM_ENDSESSION: endSession(wParam != 0, lParam); return 0;
    case afterToggle: afterToggleChanged(static_cast<int>(wParam)); return 0;
    case WM_CLOSE:
        closePlayer(); // Hidden in the tray or quitting: playback stops either way.
        if (!closing_ && state_.closeToTray) {
            if (addTrayIcon()) ShowWindow(window_, SW_HIDE);
            else messageBox(window_, L"windows could not create the tray icon. refarter will stay open.", L"refarter", MB_OK | MB_ICONERROR);
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
        SetTextColor(dc, ink);
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
    closeMeterCaptures();
    joinMeterClosers(); // Every meter capture thread has ended before the meters, COM and the process go.
    diagnosticLog().write(DiagnosticEvent::SessionEnded);
    diagnosticLog().finish();
}

int App::run(HINSTANCE instance, int showCommand, bool startup) {
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
    if (!RegisterClassExW(&wc)) return 1;
    dpi_ = GetDpiForSystem();
    RECT bounds{0, 0, px(initialClientWidth), px(initialClientHeight)};
    AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
    window_ = CreateWindowExW(0, wc.lpszClassName, L"refarter",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, instance, this);
    if (!window_) {
        MessageBoxW(nullptr, L"the window could not be created.", L"refarter", MB_ICONERROR);
        return 1;
    }
    startupTray_ = startup && state_.startMinimised && addTrayIcon();
    if (!startupTray_) {
        // Failed tray creation must never leave an inaccessible hidden process.
        ShowWindow(window_, startup ? SW_SHOW : showCommand);
        UpdateWindow(window_);
        SetFocus(control(Library));
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
