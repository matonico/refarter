#pragma once

// The level meters' foreground-hook rule without any window or hook: plain inputs, plain answer.
// App::syncMeters gathers the facts from Windows and applies the answer; tests/meter_rules_test.cpp
// exercises the rule directly, and its cases describe the behaviour it encodes.

namespace refarter {

struct MeterHookFacts {
    bool windowVisible = false;      // The window exists and is shown (not hidden to the tray).
    bool closing = false;            // The app is quitting.
    bool minimised = false;
    bool fullscreenPlayback = false; // The fullscreen player covers both the sidebar and the page.
    bool buffering = false;          // Replay is running: the sidebar shows its meters.
    bool recordingPage = false;      // The Recording page is open: its sliders show meters.
    bool sourceEnabled = false;      // Desktop audio or the microphone is on.
    bool covered = false;            // Another app's window covers ours; it does not matter here, see below.
};

// Whether the foreground hook should exist: while a meter could be on screen and a source is on. A covered window
// keeps the hook, because the hook is how the meters learn that the covering window has gone.
constexpr bool meterHookWanted(const MeterHookFacts& facts) {
    const bool visible = facts.windowVisible && !facts.closing && !facts.minimised;
    return visible && !facts.fullscreenPlayback && (facts.buffering || facts.recordingPage) && facts.sourceEnabled;
}

} // namespace refarter
