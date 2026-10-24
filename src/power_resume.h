#pragma once

// Sleep and resume decisions, kept free of windows so they can be tested. When Windows suspends, the App ends capture
// (devices and the encoder do not survive sleep) but keeps an unsaved replay in the retained state; after a short
// delay on resume it decides what to do.
namespace refarter {

// WM_POWERBROADCAST wParam values (PBT_APMSUSPEND, PBT_APMRESUMEAUTOMATIC, PBT_APMRESUMESUSPEND).
enum class PowerEvent { Other, Suspend, Resume };
inline PowerEvent classifyPower(unsigned long long wParam) {
    switch (wParam) {
    case 0x0004: return PowerEvent::Suspend;
    case 0x0012: case 0x0007: return PowerEvent::Resume; // Automatic resume is always sent; resume-suspend only for a user wake.
    default: return PowerEvent::Other;
    }
}

// How long after the resume notification the devices get to settle before replay is restarted.
constexpr unsigned resumeDelayMs = 3000;
// The longest a suspend notification waits for the recorder to pause (Windows allows about two seconds in all).
constexpr unsigned suspendWaitMs = 1500;

// What was running when the machine went to sleep.
struct SleepMemory {
    bool wasBuffering = false; // Replay was buffering.
    bool automatic = false;    // ...and auto-capture had started it.
    bool kept = false;         // ...and its unsaved replay was kept (retained) for saving.
};

struct SuspendFacts {
    bool closing = false;     // Refarter is quitting.
    bool alreadyAsleep = false; // An earlier suspend is still being handled (back to sleep before the settle delay ended).
    bool buffering = false;   // Replay is buffering and the recorder is idle.
    bool busy = false;        // The recorder is starting, stopping or changing folder.
};

enum class SuspendAction {
    Nothing, // Replay is off, or the suspend was already handled.
    Pause,   // End capture now and keep the replay.
    Defer,   // The recorder is busy (a start may be in progress): remember the suspend, apply it when the answer arrives.
};

inline SuspendAction decideSuspend(const SuspendFacts& facts) {
    if (facts.closing || facts.alreadyAsleep) return SuspendAction::Nothing;
    if (facts.busy) return SuspendAction::Defer;
    return facts.buffering ? SuspendAction::Pause : SuspendAction::Nothing;
}

struct ResumeFacts {
    bool closing = false;      // Refarter is quitting.
    bool busy = false;         // The recorder is still starting or stopping, or a suspend is waiting to be applied.
    bool recordingOff = true;  // Nothing runs now (the user did not start something meanwhile).
    bool retained = false;     // A replay is retained now.
    bool sourceAlive = false;  // The chosen window is still there (a monitor counts as there while it is connected).
};

enum class ResumeAction {
    Nothing,            // Replay was off, or the user already acted.
    Wait,               // The recorder is not idle yet; ask again when it is.
    KeepRetained,       // The unsaved replay from before sleep stays; replay is not restarted (that would clear it).
    Restart,            // Start the manual session again.
    SourceGone,         // The chosen game or window closed while asleep; say so.
    LeaveToAutoCapture, // An automatic session resumes through the normal foreground check.
};

inline ResumeAction decideResume(const SleepMemory& memory, const ResumeFacts& facts) {
    if (!memory.wasBuffering || facts.closing) return ResumeAction::Nothing;
    if (facts.busy) return ResumeAction::Wait; // Includes the pause that sleep began, which may still be finishing.
    if (memory.kept) return facts.retained ? ResumeAction::KeepRetained : ResumeAction::Nothing; // Cleared meanwhile: stays off.
    if (!facts.recordingOff) return ResumeAction::Nothing;
    if (memory.automatic) return ResumeAction::LeaveToAutoCapture;
    return facts.sourceAlive ? ResumeAction::Restart : ResumeAction::SourceGone;
}

} // namespace refarter
