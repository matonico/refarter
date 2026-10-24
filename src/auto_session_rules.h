#pragma once

// The auto-capture rules without any window, hook or recorder: plain inputs, plain answers.
// AutoSession gathers the facts from Windows and applies the answers; tests/auto_session_test.cpp
// exercises the rules directly, and its cases describe the behaviour they encode.

#include <windows.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "auto_capture.h"

namespace refarter {

// The recorder's states, as far as these rules care (mirrors capture::RecorderState).
enum class ReplayState { Off, Buffering, Stopped };

// A replay that stops this soon after an automatic start, while its game runs on, was a splash or a replaced window.
inline constexpr ULONGLONG shortSessionMs = 60'000;

// What the user answered on the card ("TimedOut": nothing yet; "Closed": the X, not now).
enum class OfferAnswer { Add, Decline, Closed, TimedOut };

inline bool samePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

// ---- A listed game, or the app in front, coming to the front ------------------------------------

enum class ForegroundAction {
    Ignore,    // Nothing on screen changes.
    Listed,    // A listed game: it is the detected game, and starts if allowed.
    Offerable, // A fullscreen app that may be offered: detected, and offered once.
    Watch,     // Offerable but windowed: watch its window for a switch to fullscreen.
};

struct ForegroundFacts {
    bool wholeMonitor = false;  // A whole monitor is the chosen source: auto-capture neither asks nor starts.
    bool offerPending = false;  // A card was shown and not answered.
    bool cardVisible = false;
    bool offerReshown = false;
    DWORD offerPid = 0, pid = 0; // The pending offer's process and the app now in front.
    bool appWindow = false;     // An ordinary app window; overlays and tool windows never trigger anything.
    bool listed = false, fullscreen = false, offerable = false;
    bool prompt = false, declined = false, offeredBefore = false;
};

struct ForegroundPlan {
    bool reshowCard = false; // An unanswered card gets one more chance after the user switches to another app.
    ForegroundAction action = ForegroundAction::Ignore;
};

inline ForegroundPlan planForeground(const ForegroundFacts& facts) {
    ForegroundPlan plan;
    if (facts.wholeMonitor) return plan;
    plan.reshowCard = facts.offerPending && !facts.cardVisible && !facts.offerReshown && facts.pid != facts.offerPid;
    if (!facts.appWindow) return plan;
    if (facts.listed) plan.action = ForegroundAction::Listed;
    else if (facts.fullscreen && facts.offerable) plan.action = ForegroundAction::Offerable;
    else if (facts.prompt && facts.offerable && !facts.declined && !facts.offeredBefore) plan.action = ForegroundAction::Watch;
    return plan;
}

// ---- Starting ------------------------------------------------------------------------------------

enum class StartDecision {
    Ignore,            // Switch off, whole monitor, suppressed instance, or a replay already running or starting.
    BlockedByRetained, // An unsaved replay is retained: say so, never discard it.
    Start,
};

inline StartDecision decideAutoStart(bool autoOn, bool wholeMonitor, bool suppressed, ReplayState state, bool busy) {
    if (!autoOn || wholeMonitor || suppressed) return StartDecision::Ignore;
    if (state == ReplayState::Stopped) return StartDecision::BlockedByRetained;
    if (state != ReplayState::Off || busy) return StartDecision::Ignore;
    return StartDecision::Start;
}

// Stopping a running replay of a listed game, automatic or manual, suppresses that instance until it exits.
// Clearing a retained replay is not a stop.
inline bool suppressOnManualStop(bool buffering, bool autoSession, bool listed) { return buffering && (autoSession || listed); }

// Permanent failures suppress this process; a temporary capture-source failure
// or a minimised-window timeout may try again on the next foreground event.
inline bool suppressOnFailedStart(bool automatic, bool success, bool windowRestored, bool retryable = false) {
    return automatic && !success && windowRestored && !retryable;
}

// An automatic session whose window closed within a minute while the game runs on: discard it, follow the next window.
// Never while a stop or a sleep pause is in progress (busy): that Stopped is not the window closing, and a replay sleep kept stays.
inline bool rearmShortSession(ReplayState state, bool autoSession, bool closing, bool busy, bool processRunning, ULONGLONG startedAt, ULONGLONG now) {
    return state == ReplayState::Stopped && autoSession && !closing && !busy && processRunning && now - startedAt < shortSessionMs;
}

// The process of an automatically started game exited (exitedAutoCapture) while its replay runs or is
// retained and nothing else is in the way. Pending saves finish; only the unsaved buffer is cleared.
inline bool gameExitClears(bool closing, bool busy, ReplayState state, bool automaticGameExited) {
    return !closing && !busy && state != ReplayState::Off && automaticGameExited;
}

// A replay that sleep keeps (the sleep pause ended in the retained state) is the user's to clear, never auto-capture's:
// it is let go from the auto session, so neither a game exit nor the short-session rule can clear it afterwards.
inline bool sleepKeepReleasesAutoSession(bool fromSleepPause, bool retained) { return fromSleepPause && retained; }

// ---- Game instances stopped by hand or that failed to start --------------------------------------

// Held process handles stop a PID from being reused while an instance is remembered; exited ones drop out.
class SuppressedInstances {
public:
    void add(DWORD pid, std::shared_ptr<void> process) {
        if (!process || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) return;
        prune();
        held_.push_back({pid, std::move(process)});
    }
    bool contains(DWORD pid) {
        prune();
        return std::any_of(held_.begin(), held_.end(), [&](const auto& entry) { return entry.pid == pid; });
    }
    size_t size() const { return held_.size(); }

private:
    struct Entry { DWORD pid; std::shared_ptr<void> process; };
    void prune() {
        std::erase_if(held_, [](const auto& entry) { return WaitForSingleObject(entry.process.get(), 0) != WAIT_TIMEOUT; });
    }
    std::vector<Entry> held_;
};

// ---- The card: offered once per app and session, shown once more after an app switch ----------------

class OfferTracker {
public:
    struct Pending { std::wstring path, label; bool reshown = false; DWORD pid = 0; };
    struct Reply {
        enum class Do { Nothing, Add, Decline } what = Do::Nothing;
        Pending offer; // Filled for Add and Decline.
    };

    bool offeredBefore(const std::wstring& path) const {
        return std::any_of(offered_.begin(), offered_.end(), [&](const auto& earlier) { return samePath(earlier, path); });
    }
    // True when the card must be shown now; the app counts as offered from here on.
    bool begin(const std::wstring& path, std::wstring label, DWORD pid, bool prompt, bool declined, bool cardVisible) {
        if (!prompt || declined || cardVisible || offeredBefore(path)) return false;
        offered_.push_back(path);
        pending_ = Pending{path, std::move(label), false, pid};
        return true;
    }
    const Pending* pending() const { return pending_ ? &*pending_ : nullptr; }
    void markReshown() { if (pending_) pending_->reshown = true; }
    void clear() { pending_.reset(); }
    Reply answer(OfferAnswer answer) {
        Reply reply;
        if (!pending_) return reply;
        if (answer == OfferAnswer::TimedOut) {
            if (pending_->reshown) pending_.reset(); // Asked twice: leave it to the tray and recording page.
            return reply;
        }
        reply.offer = std::move(*pending_);
        pending_.reset();
        if (answer == OfferAnswer::Add) reply.what = Reply::Do::Add;
        else if (answer == OfferAnswer::Decline) reply.what = Reply::Do::Decline;
        return reply; // Closed: no re-show this session, nothing remembered.
    }

private:
    std::vector<std::wstring> offered_;
    std::optional<Pending> pending_;
};

} // namespace refarter
