#pragma once

#include <windows.h>
#include <audioclient.h>
#include <winrt/base.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace refarter::player {

// Plays the player's audio on the default output: 48 kHz stereo float through
// shared-mode WASAPI, event-driven on its own thread while a clip is open.
// The engine writes timed samples ahead; clock() reports the time of the
// sample being heard, which paces the video.
class AudioOutput {
public:
    static constexpr int rate = 48000;
    static constexpr int channels = 2;

    AudioOutput() = default;
    ~AudioOutput() { close(); }
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // False when no output device can be opened; the player then runs on the wall clock.
    bool open();
    void close();
    bool opened() const { return opened_; }

    // Drops queued audio; the next written sample plays at `time` seconds.
    void flush(double time);
    void play(bool playing);
    void setVolume(float volume) { volume_ = volume; }

    // Frames that fit in the queue now, and appending interleaved samples.
    void write(const float* samples, std::size_t frames);
    // Seconds of audio written but not yet handed to Windows.
    double queued() const;

    // Time of the sample being heard; holds still while paused or starved.
    double clock() const;

private:
    void run();
    void feed(); // With the mutex held: hands queued frames to Windows.

    struct Segment {
        std::uint64_t start;  // Device stream frame where this run of real samples begins.
        std::uint64_t frames;
        double time;          // Media time of its first sample.
    };

    mutable std::mutex mutex_;
    std::vector<float> ring_;
    std::size_t readAt_ = 0, count_ = 0; // In frames.
    double readTime_ = 0;                // Media time of the next queued frame.
    std::uint64_t written_ = 0;          // Frames handed to Windows since the last reset.
    mutable std::deque<Segment> segments_;
    mutable double heldClock_ = 0;
    bool playing_ = false, started_ = false, flushPending_ = false;
    std::atomic<float> volume_ = 1.f;
    std::atomic<bool> opened_ = false;
    HANDLE stop_ = nullptr, wake_ = nullptr, ready_ = nullptr;
    winrt::com_ptr<IAudioClient> client_;
    winrt::com_ptr<IAudioRenderClient> render_;
    winrt::com_ptr<IAudioClock> clock_;
    UINT32 bufferFrames_ = 0;
    std::uint64_t clockFrequency_ = 1;
    std::thread thread_;
};

} // namespace refarter::player
