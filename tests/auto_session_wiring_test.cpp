#include "auto_session.h"
#include <iostream>

using namespace refarter;

namespace refarter {
// The offer steps onForeground and the card take, without a foreground hook or a click.
struct AutoSessionTestAccess {
    static void offer(AutoSession& session, RecordingTarget target) { session.offer(std::move(target)); }
    // What onForeground does when plan.reshowCard is set: the card had hidden and another app came to the front.
    static void reshowOver(AutoSession& session, const RecordingTarget& front) {
        session.card_.hide();
        session.offers_.markReshown();
        session.showCard(front);
    }
    static void answer(AutoSession& session, OfferAnswer answer) {
        session.card_.hide();
        session.answerOffer(answer);
    }
};
} // namespace refarter

namespace {
// A manual-reset event stands in for a game process: unsignalled while it runs, signalled once it has exited.
std::shared_ptr<void> fakeProcess() {
    return std::shared_ptr<void>(CreateEventW(nullptr, TRUE, FALSE, nullptr), [](void* handle) { CloseHandle(handle); });
}
void exitProcess(const std::shared_ptr<void>& process) { SetEvent(process.get()); }

// The real AutoSession against a stand-in App: the chosen source, the recorder state and what it was asked to do.
struct Rig {
    HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    AutoCaptureList list;
    RecordingTarget target;
    capture::RecorderState recording = capture::RecorderState::Off;
    int stops = 0;
    bool prompt = false;
    std::wstring shownFor, added, declined; // The path the card was named from, and what "add" or "not this one" acted on.
    bool startWorks = true;
    std::wstring status;
    AutoSession session;

    explicit Rig(bool wholeMonitor = false) {
        target.pid = 100;
        target.process = fakeProcess();
        target.path = L"C:\\Games\\game.exe";
        target.displayName = L"Game";
        if (wholeMonitor) target.source.monitor = MonitorFromPoint({}, MONITOR_DEFAULTTOPRIMARY);
        else target.source.window = window;
        list.add(target.path, L"game");
        AutoSession::Host host;
        host.context = [this] {
            AutoSession::Context ctx;
            ctx.recording = recording;
            ctx.source = &target;
            ctx.autoOn = ctx.readable = true;
            ctx.prompt = prompt;
            ctx.list = &list;
            return ctx;
        };
        host.start = [this](RecordingTarget) { return startWorks; };
        host.stop = [this] { ++stops; };
        host.status = [this](std::wstring text) { status = std::move(text); };
        host.add = [this](const std::wstring& path, const std::wstring&) { added = path; };
        host.name = [](const RecordingTarget& game) { return game.name; };
        host.shown = [this](const std::wstring& path, const std::wstring& stored) { shownFor = path; return shownName(lower(stored)); };
        host.decline = [this](const std::wstring& path) { declined = path; };
        host.refresh = [] {};
        host.hideFlyout = [] {};
        session.attach(window, std::move(host));
    }
    ~Rig() {
        session.unwatchGameExit();
        DestroyWindow(window);
    }
    void started() {
        recording = capture::RecorderState::Buffering;
        session.onRecorderEvent(true, recording, true, L"replay on");
    }
};
} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };

    // A manual window session clears once its own process exits, without the auto-capture prefix.
    {
        Rig rig;
        rig.started();
        check(!rig.session.clearExitedGameReplay() && rig.stops == 0, "a live manually captured game keeps its replay");
        exitProcess(rig.target.process);
        check(rig.session.clearExitedGameReplay() && rig.stops == 1, "a manual window session clears on process exit");
        check(rig.status == L"game closed · clearing replay", "a manual clear has no auto-capture prefix");
    }

    // An automatic session clears too, and its status keeps the auto-capture prefix.
    {
        Rig rig;
        rig.session.tryAutoCapture(rig.target);
        rig.started();
        check(rig.session.active(), "auto-capture started the session");
        exitProcess(rig.target.process);
        rig.recording = capture::RecorderState::Stopped; // WGC reported the window closing.
        const auto result = rig.session.onRecorderEvent(false, rig.recording, true, L"replay stopped");
        check(rig.stops == 1, "an automatic window session clears on process exit");
        check(result.status == L"auto-capture · game closed · clearing replay", "an automatic clear keeps the auto-capture prefix");
    }

    // Stop automatic replay for settings, restart it manually, then hand the next match back to auto-capture.
    {
        Rig rig;
        rig.session.tryAutoCapture(rig.target);
        rig.started();
        rig.session.onManualStop(rig.target, true);
        rig.recording = capture::RecorderState::Off;
        rig.session.onRecorderEvent(false, rig.recording, true, L"replay off");
        rig.started(); // The manual restart.
        check(!rig.session.active(), "the restart is a manual session");
        exitProcess(rig.target.process);
        check(rig.session.clearExitedGameReplay() && rig.stops == 1, "the manually restarted match clears on exit");
        rig.recording = capture::RecorderState::Off;
        rig.session.onRecorderEvent(false, rig.recording, true, L"replay off");
        rig.target.pid = 101; // The next match: a new game process.
        rig.target.process = fakeProcess();
        rig.session.tryAutoCapture(rig.target);
        check(rig.session.active(), "the next match auto-starts after the manual replay cleared");
    }

    // Sleep keeps the replay: a later process exit must not clear it.
    {
        Rig rig;
        rig.session.tryAutoCapture(rig.target);
        rig.started();
        rig.session.releaseForSleep();
        rig.recording = capture::RecorderState::Stopped;
        exitProcess(rig.target.process);
        check(!rig.session.clearExitedGameReplay() && rig.stops == 0, "a sleep-kept replay survives a later process exit");
    }

    // A manual stop detaches exit cleanup from the stopped session.
    {
        Rig rig;
        rig.started();
        rig.session.onManualStop(rig.target, true);
        exitProcess(rig.target.process);
        check(!rig.session.clearExitedGameReplay() && rig.stops == 0, "a manual stop detaches exit cleanup");
    }

    // A capture failure keeps the replay while the process lives; the process exiting clears it (decision 2026-10-28).
    {
        Rig rig;
        rig.started();
        rig.recording = capture::RecorderState::Stopped;
        rig.session.onRecorderEvent(false, rig.recording, false, L"encoder failed");
        check(!rig.session.clearExitedGameReplay() && rig.stops == 0, "a failure with a live process retains its replay");
        exitProcess(rig.target.process);
        check(rig.session.clearExitedGameReplay() && rig.stops == 1, "the retained replay clears once the process exits");
    }

    // A whole monitor has no game process to exit.
    {
        Rig rig(true);
        rig.started();
        exitProcess(rig.target.process);
        check(!rig.session.clearExitedGameReplay() && rig.stops == 0, "whole-monitor capture never clears on process exit");
    }

    // Replay off ends the watch: a later session start is needed before an exit clears anything.
    {
        Rig rig;
        rig.started();
        rig.recording = capture::RecorderState::Off;
        rig.session.onRecorderEvent(false, rig.recording, true, L"replay off");
        rig.recording = capture::RecorderState::Stopped;
        exitProcess(rig.target.process);
        check(!rig.session.clearExitedGameReplay() && rig.stops == 0, "an ended session no longer owns exit cleanup");
    }

    // Independent tester: a long game name in status text is lower case and cut to 48 characters.
    {
        Rig rig;
        rig.startWorks = false;
        auto target = rig.target;
        target.displayName = L"A Very Long Game Title That Goes On And On Past Forty Eight Characters";
        rig.session.tryAutoCapture(target);
        check(rig.status == L"auto-capture could not start a very long game title that goes on and on past…"
                           L" · check the replay settings",
              "a long game name in the status is lower case and cut");
    }

    // #169 review: a card re-shown while another app is in front still names, adds and declines the offered game.
    for (const auto answer : {OfferAnswer::Add, OfferAnswer::Decline}) {
        Rig rig;
        rig.prompt = true;
        RecordingTarget x, y;
        x.path = L"C:\\Games\\x.exe"; x.name = L"x.exe"; x.pid = 200; x.source.window = rig.window;
        y.path = L"C:\\Games\\y.exe"; y.name = L"y.exe"; y.pid = 300; y.source.window = rig.window;
        AutoSessionTestAccess::offer(rig.session, x);
        check(rig.shownFor == x.path, "the offer card is named from the offered game");
        rig.shownFor.clear();
        AutoSessionTestAccess::reshowOver(rig.session, y);
        check(rig.shownFor == x.path, "a re-shown card is named from the offered game, not the app in front");
        AutoSessionTestAccess::answer(rig.session, answer);
        check((answer == OfferAnswer::Add ? rig.added : rig.declined) == x.path, "the answer acts on the offered game");
        check(rig.added != y.path && rig.declined != y.path, "the app in front is left alone");
    }

    return failures ? 1 : 0;
}
