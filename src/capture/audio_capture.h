#pragma once

#include "audio_device.h"
#include "audio_device_state.h"
#include "audio_levels.h"
#include "audio_track.h"

#include <windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace refarter::capture {

// Captures an output (everything played on it, via WASAPI loopback), one
// process tree (Discord, via process loopback) or a microphone into an AudioTrack.
//
// Event-driven: the thread sleeps until Windows hands over audio. Loopback
// delivers nothing while nothing plays, so silence costs no wakeups. With an
// empty endpoint ID it follows the default device, reopening the new default
// when it changes or disappears (a wireless headset switched off). With a
// fixed ID it opens only that endpoint and, while it is missing, waits for
// Windows to report it again instead of switching to another device.
class AudioCapture {
public:
    enum class Device { Desktop, Microphone, Process };
    // The process tree a system-default Desktop capture leaves out instead of
    // Refarter, or the one a Process capture records; 0 for none. Read whenever
    // the stream opens; call reopen() after it changes.
    using ProcessTarget = std::function<DWORD()>;

    // `onActive` runs on the capture thread after the first open attempt and
    // whenever a device opens or goes away; read active() for the state.
    // `onSound` runs on the capture thread with the first audible packet of each opened stream; only Process captures
    // call it (no polling).
    // `track` may be null for a meter-only capture (the Recording page's level bars while replay is
    // off): nothing is stored. `meter`, if given and wanted(), sees every packet that is recorded.
    AudioCapture(Device device, std::wstring endpointId, AudioTrack* track,
                 std::function<void()> onActive = {}, ProcessTarget target = {}, LevelMeter* meter = nullptr,
                 std::function<void()> onSound = {});
    ~AudioCapture();
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // Whether a device is currently open (false while none is available).
    bool active() const { return active_; }
    // Desktop audio is open without Refarter's own sounds (process loopback).
    bool excludesSelf() const { return excludesSelf_; }
    // Desktop audio is open without Discord, which records on its own track (Refarter's sounds included).
    bool excludesDiscord() const { return excludesDiscord_; }
    // Windows refused process loopback (older builds): Discord cannot be split out.
    bool processLoopbackFailed() const { return processLoopbackFailed_; }
    // Opens the stream again, for a new process target. Any thread.
    void reopen() { SetEvent(changed_); }
    // Stops feeding the meter from now on (the thread may finish one packet, so the meter must still
    // outlive this capture). Called before a capture is closed on another thread.
    void detachMeter() { meter_.store(nullptr, std::memory_order_relaxed); }

private:
    struct DeviceWatcher;
    void run();
    void setActive(bool active);

    Device device_;
    std::wstring endpointId_;
    AudioTrack* track_;
    std::atomic<LevelMeter*> meter_;
    std::function<void()> onActive_;
    ProcessTarget target_;
    std::function<void()> onSound_;
    HANDLE stop_;
    HANDLE changed_;
    std::atomic<bool> active_ = false;
    std::atomic<bool> excludesSelf_ = false;
    std::atomic<bool> excludesDiscord_ = false;
    std::atomic<bool> processLoopbackFailed_ = false;
    DeviceAvailability availability_; // Capture thread only: first result, then real changes.
    std::thread thread_;
};

// The active endpoints for one direction, in Windows' order, with the current
// default marked. Empty if Windows cannot list them. Call when a menu opens,
// never on a timer; the calling thread must have COM initialised.
std::vector<AudioEndpoint> listAudioEndpoints(AudioCapture::Device device);

// REFARTER_AUDIO_ENDPOINT_LOOPBACK=1: desktop audio records the whole default
// output, Refarter and Discord included, for comparisons.
bool endpointLoopbackForced();

} // namespace refarter::capture
