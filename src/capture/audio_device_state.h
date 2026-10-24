#pragma once

#include "audio_device.h"

#include <algorithm>
#include <string>
#include <vector>

// The pure rules behind device changes: which endpoint a choice resolves to, when a source counts as missing or back,
// when a change is worth reporting and how long to wait before opening again. AudioCapture applies them to what
// Windows reports; nothing here talks to Windows, so they are unit-tested.
namespace refarter::capture {

// What a device choice means for the endpoints that are active right now.
struct DeviceResolution {
    enum class State {
        Off,      // The source is switched off.
        Open,     // An endpoint can be recorded: `id` is the one.
        Missing,  // A chosen device is unplugged or disabled. It is waited for; nothing else is used instead.
        NoDevice, // Following the default, but Windows has none right now. The next default is waited for.
    } state = State::Off;
    std::wstring id;
    bool operator==(const DeviceResolution&) const = default;
};

inline DeviceResolution resolveDevice(const AudioDevice& choice, const std::vector<AudioEndpoint>& active) {
    using State = DeviceResolution::State;
    switch (choice.mode) {
    case AudioDevice::Mode::Off: return {State::Off, {}};
    case AudioDevice::Mode::SystemDefault:
        for (const auto& endpoint : active) if (endpoint.isDefault) return {State::Open, endpoint.id};
        return {State::NoDevice, {}};
    case AudioDevice::Mode::Fixed:
        for (const auto& endpoint : active) if (endpoint.id == choice.id) return {State::Open, endpoint.id};
        return {State::Missing, {}};
    }
    return {State::Off, {}};
}

// The state of one source's device as the rest of the app sees it. `set` returns whether the owner is told: the
// first result always is, then only real changes, so switching between two present devices (the default moving
// from speakers to a headset) reopens without announcing a gap, while losing or regaining a device does.
class DeviceAvailability {
public:
    bool set(bool active) {
        const bool tell = !reported_ || active != active_;
        active_ = active;
        reported_ = true;
        return tell;
    }
    bool active() const { return active_; }
private:
    bool active_ = false;
    bool reported_ = false;
};

// A stream that fails again straight after opening must not spin. 0 for the first attempt, then 250 ms doubling to 5 s.
// Only failures with a device present wait this way; a device that is simply missing waits for Windows to say it came back.
inline int retryDelayMs(int consecutiveFailures) {
    if (consecutiveFailures <= 0) return 0;
    const int shift = std::min(consecutiveFailures - 1, 5);
    return std::min(5000, 250 << shift);
}

// How long to wait after a stream could not be opened. Windows refusing process loopback (older Windows, or a policy)
// will not change by itself: wait for a device change like a missing device, never retry on a timer. A device that is
// there but would not open is tried again after a growing delay; with nothing to open, wait for news.
inline constexpr unsigned long infiniteWaitMs = 0xFFFFFFFFul; // INFINITE.
inline unsigned long reopenWaitMs(bool openable, bool loopbackRefused, int consecutiveFailures) {
    if (loopbackRefused || !openable) return infiniteWaitMs;
    return static_cast<unsigned long>(retryDelayMs(consecutiveFailures));
}

// A stream that ran this long before failing counts as healthy: the next failure starts the delays again.
inline constexpr unsigned long long healthyStreamMs = 2000;

} // namespace refarter::capture
