#include "app_internal.h"
#include "leftover_files.h"
#include "message_box.h"
#include "message_wait.h"
#include "text.h"
#include "window_target.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <system_error>
#include <thread>

namespace refarter {
namespace {
constexpr DWORD saveWaitMs = 20'000;       // The longest a shutdown waits for an accepted save.
constexpr DWORD closeWaitMs = 8'000;       // The longest the closing path is waited for once the session ends.
constexpr DWORD leftoverWaitMs = 2'000;    // The longest quitting waits for the leftover cleanup thread.
constexpr DWORD criticalWaitMs = 1'000;     // The same, when Windows ends the session regardless (ENDSESSION_CRITICAL).

// Input that can start a modal loop (popup menus, the folder dialog, a title-bar drag): dropped while waitFor pumps.
// Mouse movement, wheel, timers and posted app messages still run.
bool isUserInput(UINT message) {
    switch (message) { // Releases pass, so a button or key pressed before the wait does not stay held.
    case WM_KEYUP: case WM_SYSKEYUP: case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
    case WM_NCLBUTTONUP: case WM_NCRBUTTONUP: case WM_NCMBUTTONUP: case WM_NCXBUTTONUP: return false;
    default: break;
    }
    return (message >= WM_KEYFIRST && message <= WM_KEYLAST) || message == WM_CONTEXTMENU ||
           (message >= WM_LBUTTONDOWN && message <= WM_MBUTTONDBLCLK) || (message >= WM_XBUTTONDOWN && message <= WM_XBUTTONDBLCLK) ||
           (message >= WM_NCLBUTTONDOWN && message <= WM_NCXBUTTONDBLCLK);
}
} // namespace

// State shared with the leftover cleanup thread, which may outlive the App.
struct App::LeftoverTask {
    std::atomic<bool> appGone{false}; // Set when the App stopped waiting; the thread then posts nothing.
    std::promise<void> finished;
};

bool App::waitFor(const std::function<bool()>& done, DWORD timeoutMs) {
    // These waits pump messages, so a timer or event could open a dialog that holds the wait open: for their duration
    // nothing opens one (messageBox answers at once) and callers report through the status line instead.
    struct Quiet {
        App& app;
        explicit Quiet(App& a) : app(a) { ++app.waiting_; suppressDialogs(true); }
        ~Quiet() { --app.waiting_; suppressDialogs(false); }
    } quiet(*this);
    return waitForMessages(done, timeoutMs, isUserInput);
}

// ---- Sleep and resume ------------------------------------------------------------------------------------------

void App::powerBroadcast(WPARAM wParam) {
    switch (classifyPower(wParam)) {
    case PowerEvent::Suspend: suspendForSleep(); break;
    case PowerEvent::Resume: resumeFromSleep(); break;
    case PowerEvent::Other: break;
    }
}

void App::suspendForSleep() {
    if (closing_) return;
    KillTimer(window_, resumeTimer); // Back to sleep before the settle delay ended: keep what was remembered.
    resumeDue_ = false;
    resumeSeenWhilePending_ = false; // A suspend after the wake: the pending one is current again.
    SuspendFacts facts;
    facts.closing = closing_;
    facts.alreadyAsleep = asleep_;
    facts.buffering = recording_ == capture::RecorderState::Buffering;
    facts.busy = busy_;
    switch (decideSuspend(facts)) {
    case SuspendAction::Nothing: return;
    case SuspendAction::Defer:
        // A start (or stop) is in progress and its answer decides. Applied in receiveEvents; asleep_ already keeps
        // auto-capture and the resume decision from acting meanwhile.
        pendingSuspend_ = asleep_ = true;
        sleep_ = {};
        break;
    case SuspendAction::Pause: pauseForSleep(); break;
    }
    // Windows allows about two seconds in all: wait for the answer, bounded; the rest finishes in the background.
    waitFor([this] { return (!busy_ && !pendingSuspend_) || closing_; }, suspendWaitMs);
    refreshControls();
    refreshPages();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::pauseForSleep() {
    // Capture devices and the encoder do not survive sleep. End them now but keep the unsaved replay in the
    // retained state (as when a game closes), so it can still be saved after waking. A save already accepted keeps
    // running from the buffered data.
    sleep_ = {true, auto_.active(), false};
    asleep_ = true;
    busy_ = true;
    pauseInFlight_ = true;
    setStatus(L"pausing replay for sleep…");
    recorder_->pause();
}

void App::applyPendingSuspend() {
    if (!pendingSuspend_ || busy_) return;
    pendingSuspend_ = false;
    if (closing_) return;
    if (resumeSeenWhilePending_) {
        // The machine woke before the recorder answered: pausing now would stop a replay that is fine.
        resumeSeenWhilePending_ = resumeDue_ = false;
        KillTimer(window_, resumeTimer);
        asleep_ = false;
        sleep_ = {};
        return;
    }
    if (recording_ == capture::RecorderState::Buffering) pauseForSleep();
    else { sleep_ = {}; } // Nothing to pause; remain asleep so auto-capture waits for the resume notification.
}

void App::resumeFromSleep() {
    if (!asleep_ || closing_) return;
    resumeDue_ = false;
    if (pendingSuspend_) { resumeSeenWhilePending_ = true; return; } // Dropped when the recorder answers; no settle delay.
    SetTimer(window_, resumeTimer, resumeDelayMs, nullptr); // Both resume notifications restart the one delay.
}

bool App::sourceStillThere() const {
    const auto& source = target_.source;
    if (source.window) {
        DWORD pid = 0;
        return IsWindow(source.window) && GetWindowThreadProcessId(source.window, &pid) &&
               (!target_.process || (pid == target_.pid && WaitForSingleObject(target_.process.get(), 0) == WAIT_TIMEOUT));
    }
    MONITORINFO info{sizeof(info)};
    return source.monitor != nullptr && GetMonitorInfoW(source.monitor, &info); // A handle for a gone display fails.
}

void App::revalidateMonitor() {
    // Waking can change the displays: a handle may be stale, or even point at another screen, although the chosen one
    // is there under a new handle. The device name is the identity; the handle is only compared against it.
    if (target_.source.window || sourceMonitor_.empty()) return;
    if (target_.source.monitor && monitorDevice(target_.source.monitor) == sourceMonitor_) return;
    for (const auto monitor : displayMonitors()) {
        if (monitorDevice(monitor) != sourceMonitor_) continue;
        target_.source.monitor = monitor;
        return;
    }
    target_.source.monitor = nullptr; // Not connected; sourceMonitor_ stays so the choice is remembered.
}

void App::finishResume() {
    if (!asleep_ || !resumeDue_ || endingSession_) return;
    ResumeFacts facts;
    facts.closing = closing_;
    facts.busy = busy_ || pendingSuspend_;
    facts.recordingOff = recording_ == capture::RecorderState::Off;
    facts.retained = recording_ == capture::RecorderState::Stopped;
    const bool monitorSource = !target_.source.window;
    if (monitorSource && !facts.busy) revalidateMonitor(); // Always: a kept replay must not leave a stale handle for the next start.
    facts.sourceAlive = sourceStillThere();
    const auto action = decideResume(sleep_, facts);
    if (action == ResumeAction::Wait) return; // receiveEvents asks again once the recorder is idle.
    asleep_ = resumeDue_ = false;
    sleep_ = {};
    switch (action) {
    case ResumeAction::KeepRetained:
        // Starting replay would clear it, so nothing starts: the user saves it, or enables replay for a fresh one.
        setStatus(L"replay kept · save or clear it");
        break;
    case ResumeAction::Restart:
        sleepRestart_ = true;
        setStatus(L"restarting replay after sleep…");
        if (!startReplay()) {
            sleepRestart_ = false;
            setStatus(L"could not restart · start it again");
        }
        break;
    case ResumeAction::SourceGone:
        setStatus(monitorSource ? L"monitor gone · pick a source" : L"source closed · choose it again");
        break;
    case ResumeAction::LeaveToAutoCapture:
        PostMessageW(window_, AutoSession::foregroundCheck, 0, 0); // The normal foreground check starts it again.
        break;
    case ResumeAction::Nothing: case ResumeAction::Wait: break;
    }
    auto_.recheckWhenIdle(); // For example, a retained replay was cleared during the sleep transition.
    refreshControls();
    refreshPages(); // The source row's note shows the new status now, not after the next page switch.
    InvalidateRect(window_, nullptr, FALSE);
}

// ---- Shutdown and log-off --------------------------------------------------------------------------------------

bool App::queryEndSession(LPARAM flags) {
    // Never vetoes the shutdown. While an accepted save (or our own quit) is finishing it gives Windows a reason and
    // waits, bounded, so the clip is not cut off; otherwise it answers at once. A critical shutdown (the machine is
    // going down regardless) is not waited for at all: no reason is shown and the save is not held up.
    const auto working = [this] { return closing_ ? !recorderClosed_ : saving_; };
    if (!working()) return true;
    endingSession_ = true;
    if (flags & ENDSESSION_CRITICAL) return true;
    // Kept until WM_ENDSESSION's own wait ends (or the shutdown is cancelled), not only while this save finishes.
    if (ShutdownBlockReasonCreate(window_, L"finishing a clip save")) shutdownBlocked_ = true;
    waitFor([&] { return !working(); }, saveWaitMs);
    return true;
}

void App::endSession(bool ending, LPARAM flags) {
    const auto unblock = [this] { if (shutdownBlocked_) { ShutdownBlockReasonDestroy(window_); shutdownBlocked_ = false; } };
    if (!ending) { endingSession_ = false; unblock(); return; } // Another program cancelled the shutdown.
    endingSession_ = true;
    if (!closing_) quit(); // The same path as `--quit`: settings saved, devices released, saves finished.
    waitFor([this] { return recorderClosed_; }, (flags & ENDSESSION_CRITICAL) ? criticalWaitMs : closeWaitMs);
    unblock(); // The wait is over: Windows may go on.
    if (recorderClosed_ && !compression_.active() && IsWindow(window_)) DestroyWindow(window_);
}

void App::report(const std::wstring& text, UINT icon) {
    if (!quietDialogs()) {
        messageBox(window_, text.c_str(), L"refarter", MB_OK | icon);
        return;
    }
    // Shutting down, asleep or inside a bounded wait: say it in the status line (and the source row) instead.
    setStatus(lower(text));
    if (!closing_) refreshPages();
    InvalidateRect(window_, nullptr, FALSE);
}

// ---- Unfinished files from an interrupted save, compression or trim ---------------------------------------------

void App::startLeftoverCleanup() {
    if (clipFolder_.empty()) return;
    // One short task on a dedicated thread, working only on copies and the shared task, never on App.
    // Recovery uses same-volume renames, never the shell's delete operation or a permanent-delete fallback.
    auto task = std::make_shared<LeftoverTask>();
    try {
        leftoverFinished_ = task->finished.get_future();
        leftoverThread_ = std::thread([task, folder = clipFolder_, window = window_] {
            int found = 0, moved = 0;
            try {
                const auto files = findAllLeftovers(folder); // The clip folder and its screenshots subfolder.
                found = static_cast<int>(files.size());
                if (found > 0) {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                    moved = recoverLeftovers(files);
                }
            } catch (...) {} // Anything not moved is reported below as files left in place.
            if (found > 0 && !task->appGone.load() && IsWindow(window))
                PostMessageW(window, leftoversDone, static_cast<WPARAM>(found), static_cast<LPARAM>(moved));
            task->finished.set_value();
        });
        leftoverTask_ = std::move(task);
    } catch (const std::system_error&) {} // No thread: the files stay and the next start tries again.
}

void App::releaseLeftoverTask() {
    if (leftoverTask_) leftoverTask_->appGone = true; // The thread keeps working but posts nothing to this window.
}

void App::finishLeftoverCleanup() {
    if (!leftoverThread_.joinable()) return;
    if (leftoverFinished_.wait_for(std::chrono::milliseconds(leftoverWaitMs)) == std::future_status::ready) {
        leftoverThread_.join();
    } else {
        // Probably a slow or offline volume: do not hold the process hostage. The thread no longer reports back.
        leftoverTask_->appGone = true;
        leftoverThread_.detach();
    }
}

void App::leftoversHandled(int found, int moved) {
    if (closing_ || found <= 0) return;
    moved = std::clamp(moved, 0, found);
    const auto count = [](int n) { return std::to_wstring(n) + (n == 1 ? L" unfinished file" : L" unfinished files"); };
    if (moved == found) leftoverNote_ = L"moved " + count(moved) + L" to recovery";
    else if (moved == 0) leftoverNote_ = count(found) + L" left in place";
    else leftoverNote_ = L"recovered " + std::to_wstring(moved) + L", left " + std::to_wstring(found - moved);
    showLeftoverNote();
}

void App::showLeftoverNote() {
    // Never replaces the status of an operation that is still changing it (a start, a stop, a save): shown once idle.
    if (leftoverNote_.empty() || closing_ || busy_ || saving_) return;
    // Keep a just-completed save, sleep result or error visible when recovery finishes too.
    setStatus(!statusRoutine_ && !status_.empty() ? status_ + L" · " + leftoverNote_ : leftoverNote_);
    leftoverNote_.clear();
    refreshPages(); // The source row's note shows it now, not after the next page switch.
    InvalidateRect(window_, nullptr, FALSE);
}

} // namespace refarter
