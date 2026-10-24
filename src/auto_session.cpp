#include "auto_session.h"
#include "diagnostic_log.h"
#include "text.h"
#include "window_target.h"

#include <utility>

namespace refarter {

namespace {
ReplayState replayState(capture::RecorderState state) {
    switch (state) {
    case capture::RecorderState::Buffering: return ReplayState::Buffering;
    case capture::RecorderState::Stopped: return ReplayState::Stopped;
    default: return ReplayState::Off;
    }
}
} // namespace

AutoSession* AutoSession::hookOwner_ = nullptr;

void CALLBACK AutoSession::foregroundChanged(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG, DWORD, DWORD) {
    if (hookOwner_ && object == OBJID_WINDOW) hookOwner_->onForeground(window);
}

void AutoSession::updateHook() {
    const auto ctx = host_.context();
    const bool wanted = !ctx.closing && ctx.readable && (ctx.autoOn || ctx.prompt);
    if (wanted && !foregroundHook_) {
        hookOwner_ = this;
        // One message per foreground change (Alt-Tab, a game starting): no timer, scan or worker.
        foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, foregroundChanged,
                                          0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (foregroundHook_) PostMessageW(window_, foregroundCheck, 0, 0); // A listed game may already be in front.
    } else if (!wanted && foregroundHook_) {
        UnhookWinEvent(foregroundHook_);
        foregroundHook_ = nullptr;
        unwatchSize();
        card_.hide();
        host_.hideFlyout();
        offers_.clear();
    }
}

void AutoSession::shutdown() {
    if (foregroundHook_) UnhookWinEvent(foregroundHook_);
    foregroundHook_ = nullptr;
    unwatchSize();
    card_.hide();
}

void CALLBACK AutoSession::sizeChanged(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG, DWORD, DWORD) {
    // Fires for every move of that one process's windows: only the cheap rectangle check
    // runs here, and the full foreground check only once the window covers its monitor.
    if (!hookOwner_ || object != OBJID_WINDOW || window != hookOwner_->sizeWatch_) return;
    MONITORINFO monitor{sizeof(monitor)};
    RECT bounds{};
    if (GetWindowRect(window, &bounds) && GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) &&
        coversMonitor(bounds, monitor.rcMonitor) && GetForegroundWindow() == window)
        PostMessageW(hookOwner_->window_, foregroundCheck, 0, 0); // Not from inside the hook callback.
}

void AutoSession::watchSize(HWND window, DWORD pid) {
    unwatchSize();
    sizeHook_ = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, sizeChanged,
                                pid, 0, WINEVENT_OUTOFCONTEXT);
    if (!sizeHook_) return;
    sizeWatch_ = window;
    // Fullscreen switches happen within seconds; never keep listening to an app's own movements.
    SetTimer(window_, sizeWatchTimer, 15'000, nullptr);
}

void AutoSession::unwatchSize() {
    if (sizeHook_) UnhookWinEvent(sizeHook_);
    if (sizeHook_) KillTimer(window_, sizeWatchTimer);
    sizeHook_ = nullptr;
    sizeWatch_ = nullptr;
}

void AutoSession::onForeground(HWND window) {
    const auto ctx = host_.context();
    if (ctx.closing || !foregroundHook_) return;
    unwatchSize(); // Only the app in front is ever watched.
    auto target = windowTarget(GetAncestor(window, GA_ROOT));
    if (!target) return; // Refarter itself, the desktop, untitled or inaccessible windows.
    ForegroundFacts facts;
    // A whole monitor already records everything on screen: never ask or switch sources.
    facts.wholeMonitor = ctx.source && ctx.source->source.monitor;
    if (!facts.wholeMonitor) {
        const auto* pending = offers_.pending();
        facts.offerPending = pending != nullptr;
        facts.cardVisible = card_.visible();
        facts.offerReshown = pending && pending->reshown;
        facts.offerPid = pending ? pending->pid : 0;
        facts.pid = target->pid;
        // Overlays and tool windows, even of a listed game's own executable, are never a trigger.
        facts.appWindow = appWindowStyle(GetWindowLongPtrW(target->source.window, GWL_EXSTYLE));
        if (facts.appWindow) {
            MONITORINFO monitor{sizeof(monitor)};
            RECT bounds{};
            facts.fullscreen = GetWindowRect(target->source.window, &bounds) &&
                GetMonitorInfoW(MonitorFromWindow(target->source.window, MONITOR_DEFAULTTONEAREST), &monitor) &&
                coversMonitor(bounds, monitor.rcMonitor);
            facts.listed = ctx.list->find(target->path) != nullptr;
            facts.offerable = offerable(target->path);
            facts.prompt = ctx.prompt;
            facts.declined = ctx.list->isDeclined(target->path);
            facts.offeredBefore = offers_.offeredBefore(target->path);
        }
    }
    const auto plan = planForeground(facts);
    // An unanswered card gets one more chance once the user switches away from that app,
    // in case exclusive fullscreen hid it.
    if (plan.reshowCard) {
        offers_.markReshown();
        showCard(*target, offers_.pending()->label);
    }
    switch (plan.action) {
    case ForegroundAction::Listed:
        target->displayName = ctx.list->find(target->path)->label;
        detected_ = *target;
        tryAutoCapture(std::move(*target));
        break;
    case ForegroundAction::Offerable:
        detected_ = *target;
        offer(std::move(*target));
        break;
    case ForegroundAction::Watch:
        // Games often come to the front windowed and switch to fullscreen a moment later,
        // with no further foreground change: watch this one window until it does.
        watchSize(target->source.window, target->pid);
        return;
    case ForegroundAction::Ignore:
        return; // Ordinary apps change nothing on screen.
    }
    host_.refresh();
}

bool AutoSession::detectedAlive() const {
    // A closed game's window handle can be reused by another program; check the process too.
    const auto ctx = host_.context();
    DWORD pid = 0;
    return detected_.source.window && detected_.process && !ctx.source->source.monitor &&
           WaitForSingleObject(detected_.process.get(), 0) == WAIT_TIMEOUT &&
           GetWindowThreadProcessId(detected_.source.window, &pid) && pid == detected_.pid;
}

void AutoSession::fillTray(TrayFlyoutModel& model) const {
    const auto ctx = host_.context();
    model.detected = lower(detected_.displayName);
    model.autoCapture = ctx.list->find(detected_.path) != nullptr;
    model.autoCaptureEnabled = ctx.readable;
}

void AutoSession::tryAutoCapture(RecordingTarget target) {
    const auto ctx = host_.context();
    const bool wholeMonitor = ctx.source->source.monitor != nullptr;
    const bool suppressed = ctx.autoOn && !wholeMonitor && suppressed_.contains(target.pid);
    const auto decision = decideAutoStart(ctx.autoOn, wholeMonitor, suppressed, replayState(ctx.recording), ctx.busy);
    if (decision == StartDecision::Ignore) return; // Never interrupts a running replay.
    const auto label = lower(target.displayName);
    if (decision == StartDecision::BlockedByRetained) {
        // Never discard an unsaved replay to start another.
        host_.status(L"save or clear the retained replay to auto-capture " + label);
        return;
    }
    if (!host_.start(std::move(target))) {
        host_.status(L"auto-capture could not start " + label + L" · check the replay settings");
        return;
    }
    // The remembered explicit source stays: an automatic session does not replace it.
    autoSession_ = true;
    diagnosticLog().write(DiagnosticEvent::AutoStartRequested);
    host_.status(L"auto-capture · starting " + label + L"…");
}

void AutoSession::showCard(const RecordingTarget& target, const std::wstring& label) {
    card_.show(GetModuleHandleW(nullptr), MonitorFromWindow(target.source.window, MONITOR_DEFAULTTOPRIMARY), label,
               [this](AutoCaptureCard::Answer answer) { answerOffer(answer); });
}

void AutoSession::offer(RecordingTarget target) {
    const auto ctx = host_.context();
    // Once per app and session; "not this one" is remembered across sessions.
    if (!offers_.begin(target.path, lower(target.displayName), target.pid, ctx.prompt, ctx.list->isDeclined(target.path), card_.visible())) return;
    showCard(target, offers_.pending()->label);
}

void AutoSession::answerOffer(AutoCaptureCard::Answer answer) {
    const auto reply = offers_.answer(answer);
    if (reply.what == OfferTracker::Reply::Do::Add) {
        host_.add(reply.offer.path, reply.offer.label);
        PostMessageW(window_, foregroundCheck, 0, 0); // Still in front: start now.
    } else if (reply.what == OfferTracker::Reply::Do::Decline) {
        host_.decline(reply.offer.path);
    }
}

void AutoSession::suppress(const RecordingTarget& target) { suppressed_.add(target.pid, target.process); }

void AutoSession::onManualStop(const RecordingTarget& target, bool buffering) {
    // A manual stop wins: a listed game is not auto-captured again until that instance exits.
    // Clearing a retained replay is not a stop: the game's next window may still be captured.
    if (suppressOnManualStop(buffering, autoSession_, host_.context().list->find(target.path) != nullptr)) suppress(target);
    autoSession_ = false;
    rearm_ = !buffering; // Clearing a retained replay lets the listed foreground game start without another Alt-Tab.
}

AutoSession::EventResult AutoSession::onRecorderEvent(bool started, capture::RecorderState recording, bool success, std::wstring status, bool retryableStart) {
    const auto ctx = host_.context();
    // An auto-capture failure must never put a dialog over the game: status only.
    EventResult result{std::move(status), autoSession_ && started};
    const auto state = replayState(recording);
    const auto& target = *ctx.source;
    bool rearming = false, clearingGame = false;
    if (started && success && autoSession_) autoStartedAt_ = GetTickCount64();
    if (state == ReplayState::Off) {
        autoSession_ = false;
        unwatchGameExit();
        recheckWhenIdle();
    } else if (state != ReplayState::Off && (clearingGame = watchGameExit())) {
        // The old game exited; allow a fresh automatic session after cleanup.
    } else if (rearmShortSession(state, autoSession_, ctx.closing, ctx.busy, target.process && WaitForSingleObject(target.process.get(), 0) == WAIT_TIMEOUT,
                                 autoStartedAt_, GetTickCount64())) {
        // An auto session whose window closed within a minute while the game runs on was a splash
        // or replaced window: discard that short replay and capture the game's next window instead.
        rearming = rearm_ = true;
        host_.stop();
    }
    if (result.quiet) result.status = L"auto-capture · " + result.status;
    if (clearingGame) result.status = L"auto-capture · game closed · clearing replay";
    if (rearming) result.status = L"auto-capture · the game opened a new window · following it";
    // A game auto-capture cannot record (elevated, protected) is not retried on every Alt-Tab.
    // A start that timed out because the game was minimised meanwhile is retried next time it is in front.
    RECT client{};
    const bool restored = target.source.window && IsWindow(target.source.window) && !IsIconic(target.source.window) &&
        GetClientRect(target.source.window, &client) && client.right >= 2 && client.bottom >= 2;
    if (suppressOnFailedStart(result.quiet, success, restored, retryableStart)) suppress(target);
    return result;
}

void AutoSession::unwatchGameExit() {
    if (gameExitWait_) UnregisterWaitEx(gameExitWait_, INVALID_HANDLE_VALUE);
    gameExitWait_ = nullptr;
    gameExitProcess_.reset();
}

void AutoSession::releaseForSleep() {
    autoSession_ = rearm_ = false;
    unwatchGameExit();
}

void AutoSession::recheckWhenIdle() {
    const auto ctx = host_.context();
    if (!rearm_ || ctx.closing || ctx.busy || ctx.recording != capture::RecorderState::Off) return;
    rearm_ = false;
    PostMessageW(window_, foregroundCheck, 0, 0);
}

bool AutoSession::clearExitedGameReplay() {
    const auto ctx = host_.context();
    const auto& target = *ctx.source;
    if (!gameExitClears(ctx.closing, ctx.busy, replayState(ctx.recording), exitedAutoCapture(autoSession_, target.process.get())))
        return false;
    unwatchGameExit();
    rearm_ = true;
    host_.status(L"auto-capture · game closed · clearing replay");
    diagnosticLog().write(DiagnosticEvent::GameExited);
    host_.stop(); // Finishes pending saves; only the unsaved buffer is discarded.
    return true;
}

bool AutoSession::watchGameExit() {
    const auto ctx = host_.context();
    const auto& target = *ctx.source;
    if (!autoSession_ || !target.process || ctx.closing) return false;
    if (clearExitedGameReplay()) return true;
    // Observe process exit even if WGC never reports window closure. Wait once,
    // without polling, then verify the current session again on the UI thread.
    if (!gameExitWait_) {
        gameExitProcess_ = target.process;
        if (!RegisterWaitForSingleObject(&gameExitWait_, gameExitProcess_.get(),
            [](PVOID context, BOOLEAN) { PostMessageW(static_cast<HWND>(context), gameExited, 0, 0); },
            window_, INFINITE, WT_EXECUTEONLYONCE)) {
            gameExitWait_ = nullptr;
            gameExitProcess_.reset(); // Failure retains the replay rather than discarding it.
        }
    }
    return false;
}

} // namespace refarter
