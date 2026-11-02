#include "meter_rules.h"
#include <iostream>

using namespace refarter;

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };

    MeterHookFacts shown; // The Recording page, visible, a source on, replay off.
    shown.windowVisible = true;
    shown.recordingPage = true;
    shown.sourceEnabled = true;
    check(meterHookWanted(shown), "the Recording page with a source on watches the foreground");

    auto buffering = shown;
    buffering.recordingPage = false;
    check(!meterHookWanted(buffering), "another page with replay off shows no meter");
    buffering.buffering = true;
    check(meterHookWanted(buffering), "the sidebar's meters watch the foreground while replay buffers, on any page");
    buffering.recordingPage = true;
    check(meterHookWanted(buffering), "buffering on the Recording page watches too");

    auto covered = shown;
    covered.covered = true;
    check(meterHookWanted(covered), "a covered window keeps the hook, so it can see the cover go");

    auto hidden = shown;
    hidden.windowVisible = false;
    check(!meterHookWanted(hidden), "a hidden window removes the hook");
    auto minimised = shown;
    minimised.minimised = true;
    check(!meterHookWanted(minimised), "a minimised window removes the hook");
    auto closing = shown;
    closing.closing = true;
    check(!meterHookWanted(closing), "a closing app removes the hook");

    auto playback = buffering;
    playback.fullscreenPlayback = true;
    check(!meterHookWanted(playback), "fullscreen playback removes the hook, even while replay buffers");

    auto silent = buffering;
    silent.sourceEnabled = false;
    check(!meterHookWanted(silent), "no source on removes the hook");

    check(!meterHookWanted({}), "nothing on screen needs no hook");
    return failures ? 1 : 0;
}
