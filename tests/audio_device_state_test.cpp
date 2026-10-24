#include "capture/audio_device_state.h"

#include <iostream>

using namespace refarter::capture;

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    using State = DeviceResolution::State;
    const std::vector<AudioEndpoint> speakers = {{L"A", L"Speakers", true}, {L"B", L"Headset", false}};
    const AudioDevice off{AudioDevice::Mode::Off};
    const AudioDevice follow{AudioDevice::Mode::SystemDefault};
    const AudioDevice headset{AudioDevice::Mode::Fixed, L"B", L"Headset"};
    const AudioDevice gone{AudioDevice::Mode::Fixed, L"Z", L"Old dock"};

    // Device choice resolution.
    check(resolveDevice(off, speakers).state == State::Off, "off stays off");
    check(resolveDevice(follow, speakers) == DeviceResolution{State::Open, L"A"}, "default follows the default endpoint");
    check(resolveDevice(headset, speakers) == DeviceResolution{State::Open, L"B"}, "a chosen device opens when present");
    check(resolveDevice(gone, speakers).state == State::Missing, "a chosen device that is gone is missing");
    check(resolveDevice(follow, {}).state == State::NoDevice, "no endpoints: waiting for a default");
    check(resolveDevice(headset, {}).state == State::Missing, "no endpoints: the chosen device is missing");
    auto moved = speakers;
    moved[0].isDefault = false;
    moved[1].isDefault = true;
    check(resolveDevice(follow, moved).id == L"B", "the default moving changes where the default choice records");
    check(resolveDevice(headset, moved).id == L"B" && resolveDevice(gone, moved).state == State::Missing, "a chosen device ignores the default");
    // Unplugging the chosen device never falls back to the default one.
    const std::vector<AudioEndpoint> unplugged = {{L"A", L"Speakers", true}};
    check(resolveDevice(headset, unplugged).state == State::Missing && resolveDevice(headset, unplugged).id.empty(),
          "unplugging a chosen device does not substitute another");
    // Reconnected: the same ID opens again.
    check(resolveDevice(headset, speakers).state == State::Open, "reconnecting reopens the chosen device");

    // Availability reports.
    {
        DeviceAvailability a;
        check(a.set(false), "the first result is reported even when nothing opened");
        check(!a.set(false), "no change, no report");
        check(a.set(true) && a.active(), "a device opening is reported");
        check(!a.set(true), "reopening another default is not announced");
        check(a.set(false) && !a.active(), "losing the device is reported");
        check(a.set(true), "regaining it is reported");
        DeviceAvailability b;
        check(b.set(true) && !b.set(true), "first result open: reported once");
    }

    // Retry delays back off and stay bounded.
    check(retryDelayMs(0) == 0, "the first attempt is immediate");
    check(retryDelayMs(1) == 250 && retryDelayMs(2) == 500 && retryDelayMs(3) == 1000, "delays double");
    check(retryDelayMs(6) == 5000 && retryDelayMs(100) == 5000, "delays are capped at five seconds");
    int previous = 0;
    bool grows = true;
    for (int n = 1; n < 50; ++n) { if (retryDelayMs(n) < previous) grows = false; previous = retryDelayMs(n); }
    check(grows && retryDelayMs(-3) == 0, "delays never shrink");
    check(reopenWaitMs(true, true, 3) == infiniteWaitMs, "a refused process loopback waits for a device change, not a timer");
    check(reopenWaitMs(true, false, 2) == 500 && reopenWaitMs(false, false, 2) == infiniteWaitMs, "a device that will not open backs off; nothing to open waits");
    return failures ? 1 : 0;
}
