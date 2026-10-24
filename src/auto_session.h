#pragma once

#include <windows.h>
#include <functional>
#include <string>
#include "auto_capture.h"
#include "auto_capture_card.h"
#include "auto_session_rules.h"
#include "recording_controller.h"
#include "tray_flyout.h"

namespace refarter {

// Auto-capture for listed games on the UI thread: the foreground and size WinEvent hooks, the card, the
// suppressed instances, the detected game, rearming and the exit wait for the game it started. The decisions are in
// auto_session_rules.h. The App keeps the window, tray and recorder, and answers through Host; the list
// and the two switches stay with the App's settings and are read through Context.
class AutoSession {
public:
    static constexpr UINT foregroundCheck = WM_APP + 3; // Check the app in front now (hook installed, game added).
    static constexpr UINT gameExited = WM_APP + 4;      // The automatically started game process exited.
    static constexpr UINT_PTR sizeWatchTimer = 1;       // Ends watching a windowed app for a switch to fullscreen.

    // What the App currently knows; read on every call, never stored.
    struct Context {
        bool closing = false, busy = false;
        capture::RecorderState recording = capture::RecorderState::Off;
        const RecordingTarget* source = nullptr; // The chosen source (game window or whole monitor).
        bool autoOn = false, prompt = false, readable = false;
        const AutoCaptureList* list = nullptr;
    };
    struct Host {
        std::function<Context()> context;
        std::function<bool(RecordingTarget)> start;    // Selects the target and starts replay; restores the source on failure.
        std::function<void()> stop;                    // Marks the recorder busy and stops it.
        std::function<void(std::wstring)> status;
        std::function<void(const std::wstring&, const std::wstring&)> add; // Puts a game on the list.
        std::function<void(const std::wstring&)> decline;                  // "Not this one", remembered.
        std::function<void()> refresh;                 // Controls and window after the detected game changed.
        std::function<void()> hideFlyout;              // The tray flyout, when the hook goes away.
    };
    // What a Started or Stopped recorder event means for auto-capture.
    struct EventResult {
        std::wstring status; // The event's wording, adjusted for an automatic session.
        bool quiet = false;  // An automatic start: a failure is a status only, never a dialog over the game.
    };

    AutoSession() = default;
    AutoSession(const AutoSession&) = delete;
    AutoSession& operator=(const AutoSession&) = delete;
    ~AutoSession() { if (hookOwner_ == this) hookOwner_ = nullptr; }

    void attach(HWND window, Host host) { window_ = window; host_ = std::move(host); }

    bool active() const { return autoSession_; } // The current replay was started by auto-capture.
    const RecordingTarget& detected() const { return detected_; } // Last listed or offerable app in front.
    bool detectedAlive() const;              // It still runs and still owns that window (not a whole-monitor source).
    void fillTray(TrayFlyoutModel& model) const; // The detected game and its auto-capture switch.

    void updateHook();                       // Installs or removes the foreground hook for the current switches.
    void onForeground(HWND window);
    void tryAutoCapture(RecordingTarget target);
    void unwatchSize();
    void shutdown();                         // Window destroyed: hooks, watch and card go.

    EventResult onRecorderEvent(bool started, capture::RecorderState state, bool success, std::wstring status, bool retryableStart = false);
    void onManualStop(const RecordingTarget& target, bool buffering); // A user stop; a clear of a retained replay is not one.
    bool clearExitedGameReplay();
    void releaseForSleep(); // A kept replay is no longer an auto session; a game exit never clears it.
    void recheckWhenIdle(); // Deferred rearm waits through sleep and checks the foreground once awake.
    void unwatchGameExit();

private:
    static void CALLBACK foregroundChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    static void CALLBACK sizeChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    void watchSize(HWND window, DWORD pid);
    void offer(RecordingTarget target);
    void showCard(const RecordingTarget& target, const std::wstring& label);
    void answerOffer(AutoCaptureCard::Answer answer);
    bool watchGameExit();
    void suppress(const RecordingTarget& target);

    static AutoSession* hookOwner_;
    HWND window_ = nullptr;
    Host host_;
    HWINEVENTHOOK foregroundHook_ = nullptr;
    HWINEVENTHOOK sizeHook_ = nullptr; // Scoped to the windowed app in front, for 15 s at most.
    HWND sizeWatch_ = nullptr;
    AutoCaptureCard card_;
    OfferTracker offers_;               // The card shown but unanswered, and the executables offered this session.
    RecordingTarget detected_;          // Last listed or offerable app in front, for the tray menu.
    bool autoSession_ = false;          // The current replay was started by auto-capture.
    SuppressedInstances suppressed_;    // Game instances stopped manually or failed; held until they exit.
    bool rearm_ = false;                // Recheck the foreground after an automatic session is cleared.
    ULONGLONG autoStartedAt_ = 0;
    HANDLE gameExitWait_ = nullptr;
    std::shared_ptr<void> gameExitProcess_; // Keeps the waited handle alive, even if selection changes.
};

} // namespace refarter
