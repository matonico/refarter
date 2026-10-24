#include "power_resume.h"
#include "message_wait.h"
#include <iostream>

namespace {
ULONGLONG trafficUntil = 0;
int dispatched = 0;
bool completed = false;
LRESULT CALLBACK waitProbe(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_APP + 90) {
        ++dispatched;
        if (w == 1) completed = true;
        if (GetTickCount64() < trafficUntil) PostMessageW(window, message, w, l);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
bool ignoreNothing(UINT) { return false; }
}

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    using namespace refarter;
    check(classifyPower(0x4) == PowerEvent::Suspend, "suspend");
    check(classifyPower(0x12) == PowerEvent::Resume, "resume automatic");
    check(classifyPower(0x7) == PowerEvent::Resume, "resume after user wake");
    for (const auto other : {0x0ull, 0x6ull, 0xAull, 0xBull, 0x8013ull, 0x9ull, 0x32ull}) check(classifyPower(other) == PowerEvent::Other, "other events ignored");
    check(resumeDelayMs >= 1000 && suspendWaitMs < 2000, "timings leave devices time and suspend inside the Windows allowance");

    const SleepMemory manual{true, false}, automatic{true, true}, wasOff{false, false};
    ResumeFacts idle;
    idle.sourceAlive = true;
    check(decideResume(manual, idle) == ResumeAction::Restart, "manual replay restarts");
    check(decideResume(automatic, idle) == ResumeAction::LeaveToAutoCapture, "auto-capture resumes through the foreground check");
    check(decideResume(wasOff, idle) == ResumeAction::Nothing, "replay that was off stays off");

    auto gone = idle;
    gone.sourceAlive = false;
    check(decideResume(manual, gone) == ResumeAction::SourceGone, "closed source is reported");
    check(decideResume(automatic, gone) == ResumeAction::LeaveToAutoCapture, "auto-capture ignores the old source");

    auto busy = idle;
    busy.busy = true;
    busy.recordingOff = false; // The stop that sleep started has not been reported yet.
    check(decideResume(manual, busy) == ResumeAction::Wait, "waits for the recorder to finish stopping");
    check(decideResume(automatic, busy) == ResumeAction::Wait, "automatic waits too");

    auto started = idle;
    started.recordingOff = false;
    check(decideResume(manual, started) == ResumeAction::Nothing, "the user already started replay");

    auto closing = idle;
    closing.closing = true;
    check(decideResume(manual, closing) == ResumeAction::Nothing, "never restarts while quitting");
    check(decideResume(wasOff, busy) == ResumeAction::Nothing, "nothing to wait for when replay was off");

    // Suspend: a buffering, idle recorder is paused; a busy one defers; everything else is left alone.
    SuspendFacts live;
    live.buffering = true;
    check(decideSuspend(live) == SuspendAction::Pause, "buffering replay is paused for sleep");
    auto starting = live;
    starting.buffering = false;
    starting.busy = true; // A start is in progress: its answer is not known yet.
    check(decideSuspend(starting) == SuspendAction::Defer, "suspend during a start is remembered");
    auto stopping = live;
    stopping.busy = true;
    check(decideSuspend(stopping) == SuspendAction::Defer, "suspend while the recorder is busy is remembered");
    check(decideSuspend(SuspendFacts{}) == SuspendAction::Nothing, "replay off: nothing to pause");
    auto again = live;
    again.alreadyAsleep = true;
    check(decideSuspend(again) == SuspendAction::Nothing, "a second suspend keeps what the first remembered");
    auto quitting = live;
    quitting.closing = true;
    check(decideSuspend(quitting) == SuspendAction::Nothing, "never pauses while quitting");

    // Resume with an unsaved replay kept through sleep: it is never restarted over (that would clear it).
    const SleepMemory kept{true, false, true}, keptAutomatic{true, true, true};
    auto retained = idle;
    retained.recordingOff = false;
    retained.retained = true;
    check(decideResume(kept, retained) == ResumeAction::KeepRetained, "replay kept from before sleep stays");
    check(decideResume(keptAutomatic, retained) == ResumeAction::KeepRetained, "an automatic session's kept replay stays too");
    auto keptGone = retained;
    keptGone.sourceAlive = false;
    check(decideResume(kept, keptGone) == ResumeAction::KeepRetained, "the source closing does not matter for a kept replay");
    auto cleared = idle; // The user cleared it before the settle delay ended.
    check(decideResume(kept, cleared) == ResumeAction::Nothing, "a kept replay the user cleared is not restarted");
    auto keptBusy = retained;
    keptBusy.busy = true; // The pause (or a clear) is still finishing.
    check(decideResume(kept, keptBusy) == ResumeAction::Wait, "waits for the pause to finish");
    auto keptClosing = retained;
    keptClosing.closing = true;
    check(decideResume(kept, keptClosing) == ResumeAction::Nothing, "never acts while quitting");
    // Nothing kept (an empty buffer): the previous behaviour.
    check(decideResume(manual, idle) == ResumeAction::Restart && decideResume(SleepMemory{true, false, false}, retained) == ResumeAction::Nothing,
          "without a kept replay a manual session restarts only when nothing runs");
    // Native message-queue regression: no capture hardware, visible window or OS sleep.
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = waitProbe;
    cls.lpszClassName = L"RefarterPowerWaitTest";
    RegisterClassW(&cls);
    HWND window = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    check(window != nullptr, "message-only test window created");
    if (window) {
        const auto begin = GetTickCount64();
        // The probe reposts itself until trafficUntil, so a wait that only returned once the queue
        // drained could not return before then. Five seconds is a fail-safe, not a measured bound:
        // the wait must come back (30 ms deadline) while the flood is still running, and the flood
        // is then stopped so the rest of the test runs on a quiet queue.
        trafficUntil = begin + 5000;
        PostMessageW(window, WM_APP + 90, 0, 0);
        check(!waitForMessages([] { return false; }, 30, ignoreNothing), "busy queue still times out");
        check(dispatched > 0 && GetTickCount64() < trafficUntil, "deadline checked between messages instead of after the whole queue");
        trafficUntil = 0;
        completed = false;
        PostMessageW(window, WM_APP + 90, 1, 0);
        check(waitForMessages([] { return completed; }, 1000, ignoreNothing), "completion noticed without draining the busy queue");
        check(waitForMessages([] { return true; }, 0, ignoreNothing), "already complete succeeds without waiting");
        PostQuitMessage(23);
        check(!waitForMessages([] { return false; }, 1000, ignoreNothing), "quit interrupts wait");
        MSG quit{};
        check(PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) && quit.wParam == 23, "quit and its code preserved for outer loop");
        DestroyWindow(window);
        UnregisterClassW(cls.lpszClassName, cls.hInstance);
    }
    return failures ? 1 : 0;
}
