#pragma once

#include "graphics_capture.h"
#include "audio_device.h"
#include "audio_levels.h"
#include "mic_filters.h"
#include "video_settings.h"

#include <windows.h>
#include <combaseapi.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace refarter::capture {

enum class RecorderState { Off, Buffering, Stopped };

// The clock frames and audio are stamped with: QPC in 100 ns units. Read where a save is asked for.
std::int64_t captureClock();

// Audio sources, each its own track. With two or more, saved clips start with
// all of them mixed (what players play by default), then desktop, microphone
// and Discord. Discord gets its own track where Windows can split it out of
// system-default desktop audio (build 20348+); otherwise it stays in desktop.
struct AudioSettings {
    AudioDevice desktop{AudioDevice::Mode::SystemDefault}; // Everything played on that output.
    AudioDevice microphone;                                 // Off by default.
    MicFilters micFilters;   // Applied to the microphone when saving.
    AudioLevels levels;      // Applied after filters, before mixing, when saving.
    bool hideDiscord = false; // Leaves Discord out of the recording entirely, where it can be split out.
    // Optional live level meters, fed by the desktop and microphone captures while buffering. Owned by
    // the caller, which keeps them alive until the recorder has stopped.
    LevelMeter* desktopMeter = nullptr;
    LevelMeter* microphoneMeter = nullptr;
};

struct RecorderStats {
    std::uint64_t framesCaptured = 0; // Delivered by Windows.
    std::uint64_t framesEncoded = 0;  // Kept for the replay.
    std::uint64_t framesSkipped = 0;  // Faster than the recording rate (expected).
    std::uint64_t framesDropped = 0;  // Surface/conversion failure or an older queued sample displaced.
    std::size_t bufferedBytes = 0;
    double bufferedSeconds = 0;
    double desktopAudioSeconds = 0; // Captured, including inserted silence.
    double micAudioSeconds = 0;
    bool desktopAudioActive = false; // A device is open right now.
    bool desktopExcludesSelf = false; // Refarter's own sounds (the save chime) are left out.
    bool desktopExcludesDiscord = false; // Discord is left out (its own track, or hidden); Refarter's sounds are in.
    bool discordUnavailable = false; // Windows refused process loopback: Discord stays in desktop audio.
    bool micAudioActive = false;
};

// Instant replay of exactly one user-chosen source, a game window or a monitor:
//   source --WGC--> BGRA texture --video processor--> NV12 texture
//          --hardware H.264/HEVC/AV1--> compressed packets in a rolling memory buffer
// Video never leaves the GPU until it is compressed, nothing polls, and saving
// copies compressed packets to disk without re-encoding.
class Recorder {
public:
    using Notify = std::function<void()>;
    using SaveDone = std::function<void(std::optional<std::wstring> error)>;
    // `saved` is the name the clip finished under: the requested one unless that was taken meanwhile (never replaced).
    using ClipDone = std::function<void(std::optional<std::wstring> error, const std::filesystem::path& saved)>;

    Recorder();
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Starts replay buffering. Takes roughly 100 ms to open the encoder; call
    // it away from the UI thread. `onChange` runs on an internal thread when
    // capture stops by itself (window closed or encoder failure). `onAudio`
    // runs on an audio thread once each source has tried its device and when
    // a device opens or goes away while buffering; read stats() for the state.
    // A minimised/empty window waits up to 10 s for a usable restored frame;
    // an optional caller-owned event cancels that wait and must outlive start().
    // Window creation retries selected failures for up to three seconds first.
    // targetProcess is duplicated for pipeline ownership; if omitted the window's
    // process is opened here. The supplied handle must allow query and synchronise.
    std::optional<std::wstring> start(CaptureSource source, const VideoSettings& settings,
                                      const AudioSettings& audio = {}, Notify onChange = {},
                                      Notify onAudio = {}, HANDLE startupCancel = nullptr, HANDLE targetProcess = nullptr);
    // Ends capture and releases the GPU, encoder and buffer.
    void stop();
    // Ends capture but keeps the replay already buffered, exactly as when the game closes: the state becomes
    // Stopped and saves keep working. False (nothing changed) when not buffering or nothing is buffered yet.
    bool retain(std::wstring reason);
    // Saves the latest replay on a background thread while capture continues.
    // `done` runs on that thread. The clip ends at `requestedAt` (captureClock(),
    // read when the save was asked for; 0: now), however late this call comes.
    std::optional<std::wstring> saveClip(const std::filesystem::path& path, ClipDone done = {},
                                         std::int64_t requestedAt = 0);
    // Where a screenshot goes: called on the screenshot thread once its picture is copied, so preparing a folder or
    // finding a free name never delays the picture. Returns the path, or an empty one with `error` set.
    using ShotTarget = std::function<std::filesystem::path(std::wstring& error)>;
    // Saves the next picture the source delivers, at its full size (the recorded area), as a PNG on a background
    // thread; `done` runs on that thread. Only while buffering. Nothing is copied or kept until asked: the next frame
    // is copied on the GPU, then read back and written. A source that delivers no new picture for three seconds (a
    // paused game, a still desktop) fails with a note instead of waiting on.
    std::optional<std::wstring> screenshot(ShotTarget target, SaveDone done = {});

    RecorderState state() const;
    std::wstring message() const;
    RecorderStats stats() const;
    Size outputSize() const { return output_; }
    const std::wstring& adapterName() const { return adapterName_; }
    const std::wstring& encoderName() const { return encoderName_; }
    // The rate in use; `constant` is set when the encoder fell back to CBR.
    Bitrate bitrate() const { return bitrate_; }
    bool sourceThrottled() const { return sourceThrottled_; }
    bool startRetryable() const { return startRetryable_; } // Read by the startup caller only.

private:
    struct Pipeline;
    struct Shot;
    void stopped(std::wstring reason);
    void audioChanged();
    void cleanupLoop();
    void cancelShot(); // Wakes a screenshot still waiting for a frame that will not come. Called with mutex_ held.

    mutable std::mutex mutex_;      // Serialises start, stop and save.
    mutable std::mutex stateMutex_; // Never held while calling into the pipeline.
    std::unique_ptr<Pipeline> pipeline_;
    std::thread saver_;
    std::atomic<bool> saving_ = false;
    std::thread shooter_;           // Writes one screenshot at a time.
    std::atomic<bool> shooting_ = false;
    std::atomic<std::int64_t> stoppedAt_ = 0; // QPC time capture stopped by itself; 0 while running.
    RecorderState state_ = RecorderState::Off;
    std::wstring message_;
    Notify onChange_;
    Notify onAudio_;
    std::condition_variable cleanupReady_;
    bool cleanupRequested_ = false; // Protected by stateMutex_.
    bool exiting_ = false;
    std::thread cleanup_;
    Size output_;
    std::wstring adapterName_;
    std::wstring encoderName_;
    Bitrate bitrate_;
    bool sourceThrottled_ = false;
    bool startRetryable_ = false;
    CO_MTA_USAGE_COOKIE mta_ = nullptr;
};

} // namespace refarter::capture
