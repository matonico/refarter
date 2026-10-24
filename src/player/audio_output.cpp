#include "player/audio_output.h"

#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <avrt.h>
#include <algorithm>
#include <cmath>
#include <future>

namespace refarter::player {

namespace {
constexpr std::size_t ringFrames = AudioOutput::rate / 2; // Half a second written ahead at most.
constexpr REFERENCE_TIME bufferDuration = 1'000'000;     // 100 ms in Windows' shared buffer.
}

bool AudioOutput::open() {
    if (opened_) return true;
    ring_.assign(ringFrames * channels, 0.f);
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ready_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!stop_ || !wake_ || !ready_) { close(); return false; }
    std::promise<bool> result;
    auto started = result.get_future();
    thread_ = std::thread([this, &result] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        bool ok = false;
        try {
            winrt::com_ptr<IMMDeviceEnumerator> devices;
            winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator), devices.put_void()));
            winrt::com_ptr<IMMDevice> device;
            winrt::check_hresult(devices->GetDefaultAudioEndpoint(eRender, eConsole, device.put()));
            winrt::check_hresult(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.put_void()));
            WAVEFORMATEXTENSIBLE format{};
            format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            format.Format.nChannels = channels;
            format.Format.nSamplesPerSec = rate;
            format.Format.wBitsPerSample = 32;
            format.Format.nBlockAlign = channels * 4;
            format.Format.nAvgBytesPerSec = rate * channels * 4;
            format.Format.cbSize = sizeof(format) - sizeof(WAVEFORMATEX);
            format.Samples.wValidBitsPerSample = 32;
            format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
            format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            // Windows converts to the device's own format and rate.
            winrt::check_hresult(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                bufferDuration, 0, &format.Format, nullptr));
            winrt::check_hresult(client_->SetEventHandle(ready_));
            winrt::check_hresult(client_->GetBufferSize(&bufferFrames_));
            winrt::check_hresult(client_->GetService(__uuidof(IAudioRenderClient), render_.put_void()));
            winrt::check_hresult(client_->GetService(__uuidof(IAudioClock), clock_.put_void()));
            winrt::check_hresult(clock_->GetFrequency(&clockFrequency_));
            ok = clockFrequency_ != 0;
        } catch (...) {}
        opened_ = ok;
        result.set_value(ok);
        if (ok) run();
        {
            std::lock_guard lock(mutex_);
            if (started_ && client_) client_->Stop();
            started_ = false;
            clock_ = nullptr;
            render_ = nullptr;
            client_ = nullptr;
        }
        winrt::uninit_apartment();
    });
    if (!started.get()) { close(); return false; }
    return true;
}

void AudioOutput::close() {
    if (thread_.joinable()) {
        SetEvent(stop_);
        thread_.join();
    }
    opened_ = false;
    for (auto* event : {&stop_, &wake_, &ready_}) {
        if (*event) CloseHandle(*event);
        *event = nullptr;
    }
}

void AudioOutput::run() {
    DWORD task = 0;
    const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Playback", &task);
    const HANDLE events[] = {stop_, wake_, ready_};
    while (WaitForMultipleObjects(3, events, FALSE, INFINITE) != WAIT_OBJECT_0) {
        std::lock_guard lock(mutex_);
        try {
            if (flushPending_) {
                if (started_) client_->Stop();
                client_->Reset();
                started_ = false;
                written_ = 0;
                segments_.clear();
                flushPending_ = false;
            }
            if (!playing_ && started_) {
                client_->Stop(); // The position, and so the clock, holds here.
                started_ = false;
            }
            feed();
            // Start once something is queued, so the clock begins with real audio.
            if (playing_ && !started_ && written_ > 0) {
                winrt::check_hresult(client_->Start());
                started_ = true;
            }
        } catch (...) {
            opened_ = false; // Device gone: the engine falls back to the wall clock.
            break;
        }
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

void AudioOutput::feed() {
    UINT32 padding = 0;
    winrt::check_hresult(client_->GetCurrentPadding(&padding));
    const auto frames = static_cast<UINT32>(std::min<std::size_t>(bufferFrames_ - padding, count_));
    if (!frames) return;
    BYTE* data = nullptr;
    winrt::check_hresult(render_->GetBuffer(frames, &data));
    auto* out = reinterpret_cast<float*>(data);
    const float volume = volume_;
    for (UINT32 i = 0; i < frames; ++i) {
        const std::size_t at = (readAt_ + i) % ringFrames * channels;
        for (int c = 0; c < channels; ++c) out[i * channels + c] = ring_[at + c] * volume;
    }
    winrt::check_hresult(render_->ReleaseBuffer(frames, 0));
    // Extend the current run of samples, or start one after a reset.
    if (!segments_.empty() && segments_.back().start + segments_.back().frames == written_ &&
        std::abs(segments_.back().time + static_cast<double>(segments_.back().frames) / rate - readTime_) < 1e-3)
        segments_.back().frames += frames;
    else segments_.push_back({written_, frames, readTime_});
    written_ += frames;
    readAt_ = (readAt_ + frames) % ringFrames;
    count_ -= frames;
    readTime_ += static_cast<double>(frames) / rate;
}

void AudioOutput::flush(double time) {
    {
        std::lock_guard lock(mutex_);
        readAt_ = count_ = 0;
        readTime_ = time;
        heldClock_ = time;
        segments_.clear();
        flushPending_ = true;
    }
    SetEvent(wake_);
}

void AudioOutput::play(bool playing) {
    {
        std::lock_guard lock(mutex_);
        playing_ = playing;
    }
    SetEvent(wake_);
}

double AudioOutput::queued() const {
    std::lock_guard lock(mutex_);
    return static_cast<double>(count_) / rate;
}

void AudioOutput::write(const float* samples, std::size_t frames) {
    {
        std::lock_guard lock(mutex_);
        frames = std::min(frames, ringFrames - count_);
        for (std::size_t i = 0; i < frames; ++i) {
            const std::size_t at = (readAt_ + count_ + i) % ringFrames * channels;
            for (int c = 0; c < channels; ++c) ring_[at + c] = samples[i * channels + c];
        }
        count_ += frames;
    }
    SetEvent(wake_);
}

double AudioOutput::clock() const {
    std::lock_guard lock(mutex_);
    if (flushPending_ || segments_.empty() || !clock_) return heldClock_;
    UINT64 position = 0;
    if (FAILED(clock_->GetPosition(&position, nullptr))) return heldClock_;
    const auto frame = static_cast<std::uint64_t>(static_cast<double>(position) * rate / clockFrequency_);
    // Forget runs that have finished playing, keeping the one being heard.
    while (segments_.size() > 1 && segments_[1].start <= frame) segments_.pop_front();
    const auto& run = segments_.front();
    const auto into = frame > run.start ? std::min(frame - run.start, run.frames) : 0;
    heldClock_ = run.time + static_cast<double>(into) / rate;
    return heldClock_;
}

} // namespace refarter::player
