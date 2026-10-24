#pragma once

#include "capture/codec_probe.h"
#include "capture/recorder.h"
#include <condition_variable>
#include <deque>

namespace refarter {

// Window identity is checked again on the worker before opening capture.
// Holding the process handle prevents PID reuse from selecting another app.
struct RecordingTarget {
    capture::CaptureSource source;
    DWORD pid = 0;
    std::shared_ptr<void> process;
    std::wstring name;
    std::wstring title;
    std::wstring displayName; // Friendly menu label; process identity stays in the fields above.
    std::wstring path;        // Full executable path: the auto-capture list's identity.
};

struct RecordingEvent {
    enum class Kind { Started, Stopped, Saved, Folder, Codecs, Audio, Closed, Screenshot } kind;
    capture::RecorderState state = capture::RecorderState::Off;
    std::wstring message;
    std::filesystem::path path;
    bool success = true;
    std::vector<capture::CodecSupport> codecs; // Codecs events only.
    // Audio events only: an enabled source has no device open right now.
    bool desktopMissing = false, microphoneMissing = false;
    bool discordUnavailable = false; // Windows cannot split Discord out of desktop audio.
    bool sleeping = false;           // Stopped events only: the answer to pause(). State Stopped: the replay was kept.
    bool retryableStart = false;     // A temporary capture-source failure may retry on the next foreground event.
};

// Event-driven command queue. Device activation, folder I/O and shutdown never
// run on the window thread. The recorder is constructed only on first start.
class RecordingController {
public:
    using Notify = std::function<void(RecordingEvent)>;
    explicit RecordingController(Notify notify) : notify_(std::move(notify)) {}
    ~RecordingController();
    void start(RecordingTarget target, capture::VideoSettings video, capture::AudioSettings audio,
               std::filesystem::path folder);
    void stop();
    // Sleep: ends capture and keeps the buffered replay for saving (the Stopped state a closing game also leaves).
    // With nothing buffered it is a plain stop. Answered by a Stopped event with `sleeping` set.
    void pause();
    // `requestedAt` is capture::captureClock() when the save was asked for: the clip ends there.
    void save(std::filesystem::path folder, std::wstring name, std::int64_t requestedAt);
    // The next picture of the running capture as "<folder>\screenshots\<name> <date time>.png". Answered by a
    // Screenshot event.
    void screenshot(std::filesystem::path folder, std::wstring name);
    void folder(std::filesystem::path path);
    // Checks which codecs the primary GPU records. Opens hardware encoders
    // briefly, so the window asks only while replay is off.
    void probeCodecs();
    void close();
private:
    void enqueue(std::function<void()> job);
    void run();
    std::unique_ptr<capture::Recorder> recorder_;
    Notify notify_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> jobs_;
    std::thread worker_;
    bool closing_ = false;
    winrt::handle startupCancel_; // Created lazily; close/stop wake a pending startup.
    bool exit_ = false;
};
} // namespace refarter
