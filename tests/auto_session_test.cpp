#include "auto_session_rules.h"
#include <iostream>

using namespace refarter;

namespace {
// A manual-reset event stands in for a game process: unsignalled while it runs, signalled once it has exited.
std::shared_ptr<void> fakeProcess() {
    return std::shared_ptr<void>(CreateEventW(nullptr, TRUE, FALSE, nullptr), [](void* handle) { CloseHandle(handle); });
}
void exitProcess(const std::shared_ptr<void>& process) { SetEvent(process.get()); }

ForegroundFacts inFront() {
    ForegroundFacts facts;
    facts.appWindow = true;
    facts.prompt = true;
    facts.pid = 100;
    return facts;
}
} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };

    // A listed game in front starts replay.
    {
        auto facts = inFront();
        facts.listed = true;
        facts.fullscreen = true;
        check(planForeground(facts).action == ForegroundAction::Listed, "a listed game in front is detected");
        check(decideAutoStart(true, false, false, ReplayState::Off, false) == StartDecision::Start, "a listed game starts when replay is off");
        check(decideAutoStart(false, false, false, ReplayState::Off, false) == StartDecision::Ignore, "the switch off starts nothing");
        check(decideAutoStart(true, false, false, ReplayState::Buffering, false) == StartDecision::Ignore, "a running replay is never interrupted");
        check(decideAutoStart(true, false, false, ReplayState::Off, true) == StartDecision::Ignore, "a busy recorder is never interrupted");
        check(decideAutoStart(true, false, false, ReplayState::Stopped, false) == StartDecision::BlockedByRetained,
              "a retained replay blocks the start and is kept");
        facts.appWindow = false;
        check(planForeground(facts).action == ForegroundAction::Ignore, "overlays and tool windows never trigger");
    }

    // An unlisted fullscreen game: one offer, shown once more after an app switch, never again.
    {
        auto facts = inFront();
        facts.fullscreen = facts.offerable = true;
        check(planForeground(facts).action == ForegroundAction::Offerable, "an unlisted fullscreen game is offered");
        facts.offerable = false;
        check(planForeground(facts).action == ForegroundAction::Ignore, "an app that may not be offered is ignored");

        OfferTracker offers;
        check(offers.begin(L"C:\\g\\game.exe", L"game", 100, true, false, false), "the card is shown the first time");
        check(!offers.begin(L"C:\\G\\GAME.exe", L"game", 100, true, false, false), "not offered twice (path compares without case)");
        auto same = inFront();
        same.offerPending = true;
        same.offerPid = 100;
        check(!planForeground(same).reshowCard, "no re-show while the same app is in front");
        auto other = same;
        other.pid = 200;
        check(planForeground(other).reshowCard, "re-shown after switching to another app");
        other.cardVisible = true;
        check(!planForeground(other).reshowCard, "not re-shown while the card is visible");
        other.cardVisible = false;
        offers.markReshown();
        other.offerReshown = offers.pending()->reshown;
        check(!planForeground(other).reshowCard, "re-shown only once");
        check(offers.answer(OfferAnswer::TimedOut).what == OfferTracker::Reply::Do::Nothing && !offers.pending(),
              "an unanswered second showing is dropped");
        check(!offers.begin(L"C:\\g\\game.exe", L"game", 100, true, false, false), "still not offered again this session");
        check(!offers.begin(L"C:\\h\\other.exe", L"other", 300, false, false, false), "no offer with the switch off");
        check(!offers.begin(L"C:\\h\\other.exe", L"other", 300, true, true, false), "no offer for a declined app");
        check(!offers.begin(L"C:\\h\\other.exe", L"other", 300, true, false, true), "no second card over a visible one");
    }

    // Windowed now, fullscreen later: watched until it covers its monitor.
    {
        auto facts = inFront();
        facts.offerable = true;
        check(planForeground(facts).action == ForegroundAction::Watch, "a windowed offerable app is watched");
        facts.declined = true;
        check(planForeground(facts).action == ForegroundAction::Ignore, "a declined app is not watched");
        facts.declined = false;
        facts.offeredBefore = true;
        check(planForeground(facts).action == ForegroundAction::Ignore, "an app offered before is not watched");
        facts.offeredBefore = false;
        facts.prompt = false;
        check(planForeground(facts).action == ForegroundAction::Ignore, "no watching with the offer switch off");
    }

    // "add" and "not this one", and the X.
    {
        OfferTracker offers;
        offers.begin(L"C:\\g\\game.exe", L"game", 100, true, false, false);
        auto reply = offers.answer(OfferAnswer::Add);
        check(reply.what == OfferTracker::Reply::Do::Add && reply.offer.path == L"C:\\g\\game.exe" && reply.offer.label == L"game" &&
              !offers.pending(), "add hands back the game and ends the offer");
        offers.begin(L"C:\\h\\other.exe", L"other", 101, true, false, false);
        reply = offers.answer(OfferAnswer::Decline);
        check(reply.what == OfferTracker::Reply::Do::Decline && reply.offer.path == L"C:\\h\\other.exe" && !offers.pending(),
              "not this one hands back the app and ends the offer");
        offers.begin(L"C:\\i\\third.exe", L"third", 102, true, false, false);
        check(offers.answer(OfferAnswer::Closed).what == OfferTracker::Reply::Do::Nothing && !offers.pending() &&
              offers.offeredBefore(L"C:\\i\\third.exe"), "the X remembers nothing yet is not shown again");
        check(offers.answer(OfferAnswer::Add).what == OfferTracker::Reply::Do::Nothing, "an answer without an offer does nothing");
        offers.begin(L"C:\\j\\fourth.exe", L"fourth", 103, true, false, false);
        check(offers.answer(OfferAnswer::TimedOut).what == OfferTracker::Reply::Do::Nothing && offers.pending(),
              "a first timeout keeps the offer for its second showing");
        offers.clear();
        check(!offers.pending(), "clearing drops the offer");
    }

    // Manual stop suppresses that instance until it exits; a new instance starts again.
    {
        check(suppressOnManualStop(true, true, false), "stopping an automatic session suppresses");
        check(suppressOnManualStop(true, false, true), "stopping a listed game started by hand suppresses");
        check(!suppressOnManualStop(true, false, false), "stopping an unlisted game does not");
        check(!suppressOnManualStop(false, true, true), "clearing a retained replay is not a stop");
        SuppressedInstances suppressed;
        const auto first = fakeProcess(), second = fakeProcess();
        suppressed.add(100, first);
        suppressed.add(200, second);
        check(suppressed.contains(100) && suppressed.contains(200) && !suppressed.contains(300), "several instances are suppressed at once");
        exitProcess(first);
        check(!suppressed.contains(100) && suppressed.contains(200) && suppressed.size() == 1, "an exited instance drops out");
        // The game restarts with a new process: it is free to auto-start although the old path is the same.
        check(!suppressed.contains(101), "a new instance auto-starts");
        const auto gone = fakeProcess();
        exitProcess(gone);
        suppressed.add(400, gone);
        suppressed.add(500, nullptr);
        check(suppressed.size() == 1, "an already exited or unopened process is not held");
    }

    // A failed start suppresses, unless the game was minimised meanwhile.
    check(suppressOnFailedStart(true, false, true), "a failed automatic start of a restored game is not retried");
    check(!suppressOnFailedStart(true, false, false), "a start that timed out on a minimised game is retried");
    check(!suppressOnFailedStart(true, true, true), "a successful start suppresses nothing");
    check(!suppressOnFailedStart(false, false, true), "a manual start failure suppresses nothing");
    check(!suppressOnFailedStart(true, false, true, true), "temporary window-creation failure can retry on foreground");
    check(suppressOnFailedStart(true, false, true, false), "permanent failure still suppresses the process");

    // A whole monitor as the source pauses auto-capture.
    {
        auto facts = inFront();
        facts.wholeMonitor = true;
        facts.listed = facts.fullscreen = facts.offerable = true;
        facts.offerPending = true;
        facts.pid = 200;
        const auto plan = planForeground(facts);
        check(plan.action == ForegroundAction::Ignore && !plan.reshowCard, "a whole monitor source neither asks nor starts");
        check(decideAutoStart(true, true, false, ReplayState::Off, false) == StartDecision::Ignore, "a whole monitor source never starts a game");
        check(decideAutoStart(true, true, false, ReplayState::Stopped, false) == StartDecision::Ignore, "nor mentions a retained replay");
    }

    // A short automatic session whose window closed while the game runs on is rearmed.
    {
        check(rearmShortSession(ReplayState::Stopped, true, false, false, true, 1'000, 1'000 + shortSessionMs - 1), "59 s: rearm");
        check(!rearmShortSession(ReplayState::Stopped, true, false, false, true, 1'000, 1'000 + shortSessionMs), "60 s: the replay is kept");
        check(!rearmShortSession(ReplayState::Stopped, true, false, false, false, 1'000, 2'000), "the game exited: nothing to follow");
        check(!rearmShortSession(ReplayState::Stopped, false, false, false, true, 1'000, 2'000), "a manual session is never rearmed");
        check(!rearmShortSession(ReplayState::Stopped, true, true, false, true, 1'000, 2'000), "not while closing");
        check(!rearmShortSession(ReplayState::Stopped, true, false, true, true, 1'000, 2'000), "not while a stop or sleep pause is in progress");
        check(!rearmShortSession(ReplayState::Buffering, true, false, false, true, 1'000, 2'000), "only a stopped session");
    }

    // Any automatically started game clears its replay once its own process has exited.
    {
        check(gameExitClears(false, false, ReplayState::Stopped, true), "an exited game clears its retained replay");
        check(gameExitClears(false, false, ReplayState::Buffering, true), "an exited game clears its running replay");
        check(!gameExitClears(false, false, ReplayState::Off, true), "nothing to clear without a replay");
        check(!gameExitClears(false, false, ReplayState::Stopped, false), "a running game, a manual session or no process keeps the replay");
        check(!gameExitClears(true, false, ReplayState::Stopped, true), "not while closing");
        check(!gameExitClears(false, true, ReplayState::Stopped, true), "not while the recorder is busy");
    }

    // A replay kept by sleep leaves the auto session: a later game exit cannot clear it.
    {
        check(sleepKeepReleasesAutoSession(true, true), "a sleep-kept replay is released from auto-capture");
        check(!sleepKeepReleasesAutoSession(true, false), "a sleep pause that kept nothing is an ordinary stop");
        check(!sleepKeepReleasesAutoSession(false, true), "a game closing is not a sleep keep");
        const bool autoSessionAfterRelease = false; // AutoSession::releaseForSleep clears autoSession_.
        check(!gameExitClears(false, false, ReplayState::Stopped, autoSessionAfterRelease), "the exited game finds no auto session to clear");
    }

    // The tray's detected game: a listed game, or a fullscreen app that may be offered, but not overlays or declined apps.
    {
        auto facts = inFront();
        facts.listed = true;
        check(planForeground(facts).action == ForegroundAction::Listed, "a listed game is the detected game even windowed");
        facts = inFront();
        facts.fullscreen = facts.offerable = true;
        check(planForeground(facts).action == ForegroundAction::Offerable, "a fullscreen offerable app is detected for the tray");
        facts.appWindow = false;
        check(planForeground(facts).action == ForegroundAction::Ignore, "an overlay is not detected");
        facts = inFront();
        check(planForeground(facts).action == ForegroundAction::Ignore, "an ordinary app is not detected");
    }

    return failures ? 1 : 0;
}
