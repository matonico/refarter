#include "recorder.h"

#include "audio_capture.h"
#include "discord_audio.h"
#include "game_process.h"

#include "clip_timing.h"
#include "clip_writer.h"
#include "frame_converter.h"
#include "frame_pacer.h"
#include "frame_rate_budget.h"
#include "gpu.h"
#include "annexb.h"
#include "replay_buffer.h"
#include "surface_pool.h"
#include "graphics_capture.h"
#include "video_encoder.h"
#include "timing_trace.h"
#include "diagnostic_log.h"
#include "environment.h"
#include "startup_retry.h"
#include "screenshot.h"

#include <mfapi.h>
#include <algorithm>
#include <chrono>
#include <new>
#include <sstream>
#include <string_view>

namespace refarter::capture {

namespace {
constexpr std::int64_t second = 10'000'000;
// Enough for one surface being converted, one queued and two inside the encoder.
constexpr int surfaceCount = 4;
} // namespace

// The clock Windows Graphics Capture stamps frames with.
std::int64_t captureClock() {
    LARGE_INTEGER counter{}, frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return counter.QuadPart / frequency.QuadPart * second + counter.QuadPart % frequency.QuadPart * second / frequency.QuadPart;
}

namespace {

std::wstring describe(const winrt::hresult_error& e) {
    auto text = std::wstring(e.message());
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L'.')) text.pop_back();
    return text.empty() ? L"error " + std::to_wstring(e.code()) : text;
}

// Reads a CPU-readable copy back, without holding the device while the GPU finishes the copy, and writes it as a PNG.
std::optional<std::wstring> writePicture(ID3D11DeviceContext* context, ID3D11Texture2D* picture, const std::filesystem::path& path) {
    D3D11_TEXTURE2D_DESC description{};
    picture->GetDesc(&description);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    // Bounded by time, not tries: Sleep(1) can last a whole 15.6 ms timer tick, and stop() waits for this thread.
    HRESULT result = DXGI_ERROR_WAS_STILL_DRAWING;
    for (const auto deadline = GetTickCount64() + 2000; result == DXGI_ERROR_WAS_STILL_DRAWING;) {
        result = context->Map(picture, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (result != DXGI_ERROR_WAS_STILL_DRAWING) break;
        if (GetTickCount64() >= deadline) break;
        Sleep(1);
    }
    if (FAILED(result)) return L"screenshot not saved · the graphics card did not hand over the picture";
    std::vector<std::uint8_t> pixels;
    try {
        const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData);
        pixels.assign(bytes, bytes + static_cast<std::size_t>(mapped.RowPitch) * description.Height);
    } catch (const std::bad_alloc&) {
        context->Unmap(picture, 0);
        return L"screenshot not saved · not enough memory";
    }
    context->Unmap(picture, 0); // Not held while the PNG is encoded.
    return writePng(path, pixels.data(), description.Width, description.Height, mapped.RowPitch);
}
} // namespace

// One screenshot request: the frame thread hands over a copy of the next picture; the shooter thread waits for it.
struct Recorder::Shot {
    std::mutex mutex;
    std::condition_variable ready;
    winrt::com_ptr<ID3D11Texture2D> picture; // CPU-readable copy of the recorded area; null if the copy failed.
    bool delivered = false, cancelled = false;
};

struct Recorder::Pipeline {
    explicit Pipeline(const VideoSettings& s) : settings(s), pacer(s.fps), budget(s.fps) {}
    ~Pipeline() {
        stopProducers();
    }
    void stopProducers(bool retain = false) {
        if (exitWait) UnregisterWaitEx(exitWait, INVALID_HANDLE_VALUE); // Waits for a running callback.
        exitWait = nullptr;
        discord.reset(); // First: its callbacks reopen the captures below.
        desktopCapture.reset();
        discordCapture.reset();
        gameCapture.reset();
        micCapture.reset();
        if (process) CloseHandle(process);
        process = nullptr;
        // Stop the producers first: capture feeds the encoder, which returns surfaces.
        capture.reset();
        if (retain && encoder) {
            try { sequenceHeader = encoder->sequenceHeader(); }
            catch (const std::bad_alloc&) {} // Saves can recover the in-band SPS/PPS.
        }
        encoder.reset();
        surfaces.reset();
        converter.reset();
        gpu = {};
    }

    void onFrame(const CapturedFrame& frame) {
        const auto arrived = timing ? captureClock() : 0;
        const int foreground = timing && sourceWindow ? GetForegroundWindow() == sourceWindow : -1;
        const auto trace = [&](TimingKind kind, std::int64_t sampleTime = -1) {
            if (timing) timing->record({kind, frame.time, arrived, sampleTime,
                                       captureClock() - arrived, 0, foreground});
        };
        try {
            if (shotWanted.load(std::memory_order_relaxed)) takeShot(frame);
            ++captured;
            if (clockStart < 0) clockStart = frame.time;
            const auto slot = legacyPacing ? pacer.slotFor(frame.time) : std::nullopt;
            if (legacyPacing ? !slot : !budget.allows(frame.time)) {
                ++skipped; // Rate budget exhausted, or legacy slot already used.
                trace(TimingKind::PacerSkip);
                return;
            }
            const auto sampleTime = preserveCaptureTime ? frame.time - clockStart.load() : pacer.time(*slot);
            auto surface = surfaces->acquire();
            if (!surface) {
                ++dropped; // Every surface is still with the encoder.
                trace(TimingKind::SurfaceBusy, sampleTime);
                return;
            }
            if (!converter->convert(frame.texture, frame.region, surface->texture)) {
                ++dropped;
                trace(TimingKind::ConversionFailed, sampleTime);
                return;
            }
            if (legacyPacing) pacer.commit(*slot);
            else budget.commit();
            // Missing source updates hold the last picture; no duplicate encoding.
            surface->sample->SetSampleTime(sampleTime);
            surface->sample->SetSampleDuration(pacer.duration());
            const bool accepted = encoder->submit(std::move(surface->sample));
            if (!accepted) ++dropped; // The new sample displaced an older queued sample.
            trace(accepted ? TimingKind::Submitted : TimingKind::QueueDisplaced, sampleTime);
        } catch (...) {
            trace(TimingKind::CaptureError);
            throw;
        }
    }

    // The first frame after a screenshot request, before admission (a frame the rate budget skips will do): a GPU copy
    // of the recorded area into a texture the CPU can read, handed to the waiting shooter thread.
    void takeShot(const CapturedFrame& frame) {
        std::shared_ptr<Shot> request;
        {
            std::lock_guard lock(shotMutex);
            request = std::move(shot);
            shotWanted = false;
        }
        if (!request) return;
        {
            std::lock_guard lock(request->mutex);
            if (request->cancelled) return; // Timed out already: copy nothing.
        }
        winrt::com_ptr<ID3D11Texture2D> picture;
        D3D11_TEXTURE2D_DESC source{};
        frame.texture->GetDesc(&source);
        // Clamped to the texture, as the converter does: while a resize settles the region can extend past it.
        const auto clamped = clampToTexture(frame.region, source.Width, source.Height);
        const RECT area = clamped.value_or(RECT{});
        D3D11_TEXTURE2D_DESC copy{};
        copy.Width = static_cast<UINT>(area.right - area.left);
        copy.Height = static_cast<UINT>(area.bottom - area.top);
        copy.MipLevels = copy.ArraySize = 1;
        copy.Format = source.Format;
        copy.SampleDesc.Count = 1;
        copy.Usage = D3D11_USAGE_STAGING;
        copy.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (clamped && copy.Width && copy.Height && SUCCEEDED(gpu.device->CreateTexture2D(&copy, nullptr, picture.put()))) {
            const D3D11_BOX box{static_cast<UINT>(area.left), static_cast<UINT>(area.top), 0,
                                static_cast<UINT>(area.right), static_cast<UINT>(area.bottom), 1};
            gpu.context->CopySubresourceRegion(picture.get(), 0, 0, 0, 0, frame.texture, 0, &box);
            gpu.context->Flush(); // Starts the copy now; the read-back then waits for nothing else.
        } else {
            picture = nullptr;
        }
        {
            std::lock_guard lock(request->mutex);
            if (request->cancelled) return;
            request->picture = std::move(picture);
            request->delivered = true;
        }
        request->ready.notify_one();
    }

    VideoSettings settings;
    Gpu gpu;
    Size output;
    Bitrate bitrate;
    std::unique_ptr<ReplayBuffer> buffer; // Sized once the output size is known.
    std::unique_ptr<FrameConverter> converter;
    std::unique_ptr<SurfacePool> surfaces;
    std::unique_ptr<VideoEncoder> encoder;
    std::unique_ptr<GraphicsCapture> capture;
    std::atomic<std::uint64_t> captured = 0, encoded = 0, skipped = 0, dropped = 0;
    FramePacer pacer; // Legacy comparison; both selectors are frame-thread state.
    FrameRateBudget budget;
    std::atomic<std::int64_t> clockStart = -1; // Capture time of packet time zero.
    HANDLE process = nullptr;
    HANDLE exitWait = nullptr;
    // Audio shares the capture clock (QPC); clockStart maps it to packet time.
    // The game window's own process tree (process loopback, include mode), recorded beside desktop audio.
    std::unique_ptr<AudioTrack> desktopAudio, micAudio, discordAudio, gameAudio;
    std::unique_ptr<AudioCapture> desktopCapture, micCapture, discordCapture, gameCapture;
    // Discord's process tree, split out of system-default desktop audio; 0 while it is not running.
    std::atomic<DWORD> discordProcess = 0;
    std::unique_ptr<DiscordWatch> discord;
    AudioCapture::ProcessTarget gameProcess; // The game process bound at the start while it runs; 0 once it is gone.
    std::atomic<GameOutput> gameOutput = GameOutput::Unknown; // Where the game last verifiably played, for saves after it has gone.
    std::wstring desktopEndpoint; // A chosen desktop output; empty for the system default.
    MicFilters micFilters;
    AudioLevels audioLevels;
    std::vector<std::uint8_t> sequenceHeader;
    HWND sourceWindow = nullptr;
    std::unique_ptr<TimingTrace> timing;
    bool preserveCaptureTime = true;
    bool legacyPacing = false;
    // A pending screenshot; the frame thread only reads the flag until one is asked for.
    std::mutex shotMutex;
    std::shared_ptr<Shot> shot;
    std::atomic<bool> shotWanted = false;
};

Recorder::Recorder() {
    // Keeps a process-wide MTA alive so the free-threaded capture and Media
    // Foundation objects work no matter which thread calls in.
    winrt::check_hresult(CoIncrementMTAUsage(&mta_));
    try {
        winrt::check_hresult(MFStartup(MF_VERSION, MFSTARTUP_LITE));
        try { cleanup_ = std::thread([this] { cleanupLoop(); }); }
        catch (...) { MFShutdown(); throw; }
    } catch (...) { CoDecrementMTAUsage(mta_); throw; }
}

Recorder::~Recorder() {
    stop();
    {
        std::lock_guard lock(stateMutex_);
        exiting_ = true;
    }
    cleanupReady_.notify_one();
    cleanup_.join();
    MFShutdown();
    CoDecrementMTAUsage(mta_);
}

std::optional<std::wstring> Recorder::start(CaptureSource source, const VideoSettings& settings,
                                           const AudioSettings& audio, Notify onChange, Notify onAudio, HANDLE startupCancel, HANDLE targetProcess) {
    std::lock_guard control(mutex_);
    startRetryable_ = false;
    if (pipeline_) return L"replay is already running";
    if (auto error = validate(settings)) return error;
    if (auto error = validate(audio.micFilters)) return error;
    if (auto error = validate(audio.levels)) return error;
    if (!source.window == !source.monitor) return L"choose a game window or a monitor to record";
    if (source.window && !IsWindow(source.window)) return L"the selected window no longer exists";

    const bool diagnostics = environmentIs(L"REFARTER_CAPTURE_DIAGNOSTICS", L"1");
    const wchar_t* startupStage = L"graphics device";
    unsigned stageId = 1;
    const auto stage = [&](const wchar_t* name, unsigned id) {
        startupStage = name;
        stageId = id;
        diagnosticLog().write(DiagnosticEvent::StartupStage, S_OK, id);
    };
    Size initialSurface{}, initialRegion{}, encodedSize{};
    RECT initialClient{};
    const bool minimised = source.window && IsIconic(source.window);
    const bool foreground = diagnostics && source.window && GetForegroundWindow() == source.window;
    if (diagnostics && source.window) GetClientRect(source.window, &initialClient);

    try {
        auto pipeline = std::make_unique<Pipeline>(settings);
        auto* p = pipeline.get();
        p->audioLevels = audio.levels;
        stage(L"graphics device", 1);
        if (source.window) {
            DWORD pid = 0;
            GetWindowThreadProcessId(source.window, &pid);
            if (targetProcess) {
                if (!DuplicateHandle(GetCurrentProcess(), targetProcess, GetCurrentProcess(), &p->process,
                        0, FALSE, DUPLICATE_SAME_ACCESS)) winrt::throw_last_error();
            } else p->process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!p->process) winrt::throw_last_error();
        }
        p->gpu = source.window ? createGpuForWindow(source.window) : createGpuForMonitor(source.monitor);
        stage(L"capture source", 2);
        // Throttle DWM at 45% of a frame interval: at least two candidate frames
        // per slot keeps motion even, yet a 240 Hz game recorded at 60 fps costs
        // about 120 copies a second instead of 240.
        p->capture = std::make_unique<GraphicsCapture>(source, p->gpu.device.get(), second * 45 / 100 / settings.fps,
            [this, p](const CapturedFrame& frame) {
                try {
                    p->onFrame(frame);
                } catch (const winrt::hresult_error& e) {
                    ++p->dropped;
                    const auto removed = p->gpu.device->GetDeviceRemovedReason();
                    if (FAILED(removed)) stopped(L"the graphics device was reset (" + describe(winrt::hresult_error(removed)) + L")");
                    else if (e.code() == E_OUTOFMEMORY) stopped(L"out of video memory");
                }
            },
            [this, p, game = source.window != nullptr](HRESULT cause) {
                // A failure inside frame handling is a lost device as much as a closed window: say which.
                if (FAILED(cause)) {
                    HRESULT lost = p->gpu.device->GetDeviceRemovedReason();
                    if (SUCCEEDED(lost) && (cause == DXGI_ERROR_DEVICE_REMOVED || cause == DXGI_ERROR_DEVICE_RESET)) lost = cause;
                    if (FAILED(lost)) {
                        stopped(L"the graphics device was reset (" + describe(winrt::hresult_error(lost)) + L")");
                        return;
                    }
                }
                stopped(game ? L"the game window closed" : L"the monitor was disconnected");
            },
            startupCancel, p->process);
        p->sourceWindow = source.window;
        initialSurface = p->capture->initialSurfaceSize();
        initialRegion = p->capture->size();
        // Capture timestamps are the default; explicit zero is a developer comparison override.
        p->preserveCaptureTime = !environmentIs(L"REFARTER_CAPTURE_TIMESTAMPS", L"0");
        // Grid timestamps require grid admission to avoid duplicate sample PTS.
        p->legacyPacing = !p->preserveCaptureTime || environmentIs(L"REFARTER_FRAME_PACING", L"legacy");
        if (diagnostics) {
            try { p->timing = std::make_unique<TimingTrace>(); }
            catch (const std::bad_alloc&) {} // Optional tracing must not prevent recording.
        }
        RECT client{};
        const bool emptyClient = source.window && GetClientRect(source.window, &client) &&
            (client.right < 2 || client.bottom < 2);
        if (source.window && (minimised || IsIconic(source.window) || emptyClient)) {
            stage(L"waiting for restored game frame", 3);
            const auto ready = p->capture->awaitRestoredFrame(10'000, p->process, startupCancel);
            if (ready == GraphicsCapture::StartupResult::Cancelled) return L"starting replay cancelled";
            if (ready == GraphicsCapture::StartupResult::Closed) return L"the selected game closed before recording could start";
            if (ready == GraphicsCapture::StartupResult::Timeout)
                return L"the selected game is minimised or not providing frames · return to the game and enable replay again";
        }
        p->output = capture::outputSize(settings.resolution, p->capture->size());
        encodedSize = p->output;
        if (p->output.width == 0) return L"the selected window has no visible area";
        // Presets scale with the encoded picture, so memory is checked at the
        // peak rate for this window's size: the worst case a replay can hold.
        p->bitrate = bitrateFor(settings, p->output);
        // The buffer holds the replay plus the audio ring's headroom, so a save that starts late
        // still finds the whole clip that ended at the press.
        const int retainSeconds = AudioTrack::ringSeconds(settings.replaySeconds);
        const auto worstCase = replayBytes(p->bitrate.peakKbps, retainSeconds);
        // The PCM rings count too, one per track that will open (see below; the same numbers as the RAM estimate).
        const bool desktopOn = audio.desktop.enabled(), followDiscordRing = !endpointLoopbackForced();
        const std::uint64_t rings = (desktopOn ? 1 : 0) + (audio.microphone.enabled() ? 1 : 0) +
            (desktopOn && followDiscordRing && !audio.hideDiscord ? 1 : 0) + (desktopOn && followDiscordRing && source.window ? 1 : 0);
        const auto audioBytes = rings * AudioTrack::ringBytes(settings.replaySeconds);
        MEMORYSTATUSEX memory{sizeof(memory)};
        if (GlobalMemoryStatusEx(&memory) && worstCase + audioBytes > memory.ullTotalPhys / 4) {
            return L"this quality and replay length need more memory than is safe to reserve; lower one of them";
        }
        p->buffer = std::make_unique<ReplayBuffer>(retainSeconds * second, worstCase * 3 / 2);
        stage(L"colour converter", 4);
        p->converter = std::make_unique<FrameConverter>(p->gpu.device.get(), p->gpu.context.get(), p->output, settings.fps);
        stage(L"encoder surfaces", 5);
        p->surfaces = std::make_unique<SurfacePool>(*p->converter, surfaceCount);
        stage(L"video encoder", 6);
        p->encoder = std::make_unique<VideoEncoder>(p->gpu.device.get(), p->gpu.luid,
            EncoderConfig{p->output, settings.fps, settings.codec, p->bitrate},
            [p](PacketPtr packet) {
                if (p->timing) p->timing->record({TimingKind::Encoded, 0, captureClock(), packet->time, 0, packet->data.size()});
                ++p->encoded;
                p->buffer->push(std::move(packet));
            },
            [this](HRESULT hr) { stopped(L"the video encoder stopped (" + describe(winrt::hresult_error(hr)) + L")"); });
        {
            std::lock_guard lock(stateMutex_);
            state_ = RecorderState::Buffering;
            stoppedAt_ = 0;
            cleanupRequested_ = false;
            message_.clear();
            onChange_ = std::move(onChange);
            onAudio_ = std::move(onAudio);
        }
        try {
            stage(L"start capture", 7);
            p->capture->start();
            // Window events are not reliable when a game crashes or is killed;
            // a kernel wait on the process (opened above for every window source) costs nothing until it exits.
            if (p->process) {
                RegisterWaitForSingleObject(&p->exitWait, p->process, [](PVOID context, BOOLEAN) {
                    static_cast<Recorder*>(context)->stopped(L"the game closed");
                }, this, INFINITE, WT_EXECUTEONLYONCE);
            }
            const int audioSeconds = AudioTrack::ringSeconds(settings.replaySeconds);
            // An empty ID follows the Windows default; a fixed one waits for that endpoint.
            const auto endpoint = [](const AudioDevice& device) {
                return device.mode == AudioDevice::Mode::Fixed ? device.id : std::wstring{};
            };
            if (audio.desktop.enabled()) {
                p->desktopAudio = std::make_unique<AudioTrack>(audioSeconds);
                // Discord records on its own track unless hidden; process loopback reaches it on
                // any output. System-default desktop audio leaves it out. A chosen output records
                // that device, so Discord stays in it only if Discord plays there (checked on save).
                const bool followDiscord = !endpointLoopbackForced();
                const bool excludeDiscord = followDiscord && audio.desktop.mode == AudioDevice::Mode::SystemDefault;
                AudioCapture::ProcessTarget discordProcess;
                if (followDiscord) {
                    p->discordProcess = findDiscordProcess();
                    discordProcess = [p] { return p->discordProcess.load(); };
                }
                if (audio.desktop.mode == AudioDevice::Mode::Fixed) p->desktopEndpoint = audio.desktop.id;
                p->desktopCapture = std::make_unique<AudioCapture>(AudioCapture::Device::Desktop,
                    endpoint(audio.desktop), p->desktopAudio.get(), [this] { audioChanged(); },
                    excludeDiscord ? discordProcess : AudioCapture::ProcessTarget{}, audio.desktopMeter);
                if (followDiscord && !audio.hideDiscord) {
                    p->discordAudio = std::make_unique<AudioTrack>(audioSeconds);
                    p->discordCapture = std::make_unique<AudioCapture>(AudioCapture::Device::Process,
                        std::wstring{}, p->discordAudio.get(), [this] { audioChanged(); }, discordProcess);
                }
                // A game window also gets its own process tree on a track named "game": the mix and the desktop track
                // are unchanged (the game is in both). A whole monitor has no single process to follow. The window's
                // process is read again whenever the stream opens, so a stream reopened after a device change follows it.
                if (source.window && !endpointLoopbackForced()) {
                    p->gameAudio = std::make_unique<AudioTrack>(audioSeconds);
                    // The target is the process validated at the top of start(), held by a handle the callback shares
                    // (the saver's copy outlives stopProducers). Windows recycles window handles, so the window is never
                    // asked again: the target is that process while it runs, else 0 (capture waits), never another one.
                    const auto bound = BoundProcess::ofProcess(p->process);
                    p->gameProcess = [bound] { return bound.running(); };
                    p->gameCapture = std::make_unique<AudioCapture>(AudioCapture::Device::Process, std::wstring{}, p->gameAudio.get(),
                        [this] { audioChanged(); }, p->gameProcess, nullptr,
                        // A chosen output: note where the game plays once it makes sound on this stream, for saves made after it is gone.
                        [p] {
                            if (p->desktopEndpoint.empty()) return;
                            const auto root = p->gameProcess ? p->gameProcess() : 0;
                            if (const auto seen = gamePlaysOn(p->desktopEndpoint, root); seen != GameOutput::Unknown) p->gameOutput = seen;
                        });
                }
                if (excludeDiscord || p->discordCapture) {
                    p->discord = std::make_unique<DiscordWatch>([p, excludeDiscord](DWORD process) {
                        if (p->discordProcess.exchange(process) == process) return;
                        if (excludeDiscord) p->desktopCapture->reopen();
                        if (p->discordCapture) p->discordCapture->reopen();
                    });
                }
            }
            if (audio.microphone.enabled()) {
                p->micAudio = std::make_unique<AudioTrack>(audioSeconds);
                p->micFilters = audio.micFilters;
                p->micCapture = std::make_unique<AudioCapture>(AudioCapture::Device::Microphone,
                    endpoint(audio.microphone), p->micAudio.get(), [this] { audioChanged(); }, AudioCapture::ProcessTarget{},
                    audio.microphoneMeter);
            }
        } catch (...) {
            std::lock_guard lock(stateMutex_);
            state_ = RecorderState::Off;
            cleanupRequested_ = false;
            onChange_ = nullptr;
            onAudio_ = nullptr;
            throw;
        }
        output_ = p->output;
        adapterName_ = p->gpu.name;
        encoderName_ = p->encoder->name();
        bitrate_ = p->bitrate;
        bitrate_.constant = !p->encoder->variableBitrate();
        sourceThrottled_ = p->capture->sourceThrottled();
        pipeline_ = std::move(pipeline);
        return std::nullopt;
    } catch (const winrt::hresult_error& e) {
        const auto hr = e.code();
        startRetryable_ = source.window && std::wstring_view(startupStage) == L"capture source" &&
            (retryableWindowCreation(hr) || hr == HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE) ||
             hr == HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED) || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED));
        diagnosticLog().write(DiagnosticEvent::CaptureFailed, hr, stageId);
        auto message = L"could not start recording: " + describe(e);
        if (diagnostics) {
            std::wostringstream details;
            details << L"\nstartup stage=" << startupStage << L"; hresult=0x" << std::hex << static_cast<unsigned long>(e.code()) << std::dec
                    << L"; minimised=" << minimised << L"; foreground=" << foreground
                    << L"; client=" << initialClient.right << L"x" << initialClient.bottom
                    << L"; surface=" << initialSurface.width << L"x" << initialSurface.height
                    << L"; region=" << initialRegion.width << L"x" << initialRegion.height
                    << L"; output=" << encodedSize.width << L"x" << encodedSize.height;
            message += details.str();
        }
        return message;
    } catch (const std::bad_alloc&) {
        return L"could not start recording: out of memory";
    }
}

void Recorder::stopped(std::wstring reason) {
    {
        std::lock_guard lock(stateMutex_);
        if (state_ != RecorderState::Buffering || cleanupRequested_) return;
        stoppedAt_ = captureClock();
        message_ = std::move(reason);
        cleanupRequested_ = true;
    }
    // Callbacks cannot destroy their own capture/encoder or join themselves.
    cleanupReady_.notify_one();
}

void Recorder::audioChanged() {
    Notify notify;
    {
        std::lock_guard lock(stateMutex_);
        if (state_ != RecorderState::Buffering) return; // Retained or stopping: devices are closing anyway.
        notify = onAudio_;
    }
    if (notify) notify();
}

void Recorder::cleanupLoop() {
    for (;;) {
        {
            std::unique_lock lock(stateMutex_);
            cleanupReady_.wait(lock, [this] { return exiting_ || cleanupRequested_; });
            if (exiting_) return;
        }
        Notify notify;
        {
            std::lock_guard control(mutex_);
            {
                std::lock_guard lock(stateMutex_);
                if (!cleanupRequested_ || state_ != RecorderState::Buffering) continue;
            }
            if (pipeline_) pipeline_->stopProducers(true);
            cancelShot(); // No frame will come now: a pending screenshot fails at once, not after its 3 s wait.
            {
                std::lock_guard lock(stateMutex_);
                cleanupRequested_ = false;
                state_ = RecorderState::Stopped;
                notify = onChange_;
            }
        }
        if (notify) notify();
    }
}

void Recorder::stop() {
    std::lock_guard control(mutex_);
    {
        std::lock_guard lock(stateMutex_);
        state_ = RecorderState::Off;
        cleanupRequested_ = false;
    }
    if (saver_.joinable()) saver_.join(); // A save reads the audio tracks.
    cancelShot();
    if (shooter_.joinable()) shooter_.join(); // A screenshot reads from the device.
    pipeline_.reset();
    std::lock_guard lock(stateMutex_);
    state_ = RecorderState::Off;
    message_.clear();
    onChange_ = nullptr;
    onAudio_ = nullptr;
}

bool Recorder::retain(std::wstring reason) {
    std::lock_guard control(mutex_);
    {
        std::lock_guard lock(stateMutex_);
        if (state_ != RecorderState::Buffering || !pipeline_ || !pipeline_->buffer || pipeline_->buffer->bytes() == 0) return false;
        // Stopped first, so a callback racing with the teardown below (window closed, encoder error) changes nothing.
        stoppedAt_ = captureClock();
        message_ = std::move(reason);
        cleanupRequested_ = false;
        state_ = RecorderState::Stopped;
    }
    pipeline_->stopProducers(true); // Not under stateMutex_: it waits for running callbacks, which take that lock.
    cancelShot(); // No frame will come now.
    return true;
}

void Recorder::cancelShot() {
    if (!pipeline_) return;
    std::shared_ptr<Shot> request;
    {
        std::lock_guard lock(pipeline_->shotMutex);
        request = std::move(pipeline_->shot);
        pipeline_->shotWanted = false;
    }
    if (!request) return;
    {
        std::lock_guard lock(request->mutex);
        request->cancelled = true;
    }
    request->ready.notify_one();
}

std::optional<std::wstring> Recorder::screenshot(ShotTarget target, SaveDone done) {
    std::lock_guard control(mutex_);
    if (!pipeline_ || !pipeline_->capture || state() != RecorderState::Buffering) return L"screenshots need replay running";
    if (shooting_.exchange(true)) return L"a screenshot is already being taken";
    try {
        auto request = std::make_shared<Shot>();
        auto context = pipeline_->gpu.context; // Kept alive by the shooter; stop() joins it before the device goes.
        if (shooter_.joinable()) shooter_.join(); // The previous one has finished (shooting_ was clear).
        shooter_ = std::thread([this, request, context, target = std::move(target), done = std::move(done)]() mutable {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            std::optional<std::wstring> error;
            winrt::com_ptr<ID3D11Texture2D> picture;
            {
                std::unique_lock lock(request->mutex);
                if (!request->ready.wait_for(lock, std::chrono::seconds(3), [&] { return request->delivered || request->cancelled; })) {
                    request->cancelled = true; // A frame arriving later copies nothing.
                    error = L"no new picture in 3 s · try again while something moves";
                } else if (!request->delivered) {
                    error = L"replay stopped before the screenshot";
                } else if (!request->picture) {
                    error = L"screenshot not saved · the graphics card could not copy the picture";
                }
                picture = request->picture;
            }
            if (!error) {
                // The picture is in hand: only now is the folder prepared and a free name chosen.
                std::wstring problem;
                const auto path = target ? target(problem) : std::filesystem::path{};
                if (path.empty()) error = problem.empty() ? L"screenshot not saved · no place to save it" : problem;
                else error = writePicture(context.get(), picture.get(), path);
            }
            shooting_ = false;
            if (done) done(std::move(error));
        });
        {
            std::lock_guard lock(pipeline_->shotMutex);
            pipeline_->shot = std::move(request);
            pipeline_->shotWanted = true;
        }
        return std::nullopt;
    } catch (const std::exception&) {
        shooting_ = false;
        return L"could not take a screenshot: insufficient memory or worker resources";
    }
}

std::optional<std::wstring> Recorder::saveClip(const std::filesystem::path& path, ClipDone done, std::int64_t requestedAt) {
    std::lock_guard control(mutex_);
    if (!pipeline_) return L"replay is off";
    if (saving_.exchange(true)) return L"a clip is already being saved";
    try {
        // Measure the replay back from the request, not from the last frame: on a static
        // screen or paused game the newest frame can be seconds old.
        const auto start = pipeline_->clockStart.load();
        const auto replay = pipeline_->settings.replaySeconds * second;
        auto now = clipEnd(requestedAt, captureClock(), stoppedAt_.load()) - start;
        auto packets = start < 0 || now <= 0 ? std::vector<PacketPtr>{} : pipeline_->buffer->snapshot(replay, now);
        if (packets.empty() && start >= 0 && pipeline_->buffer->evicted()) {
            // The worker was later than the headroom: the press has left the buffer. Saving the newest
            // replay is better than refusing with a full buffer.
            now = clipEnd(0, captureClock(), stoppedAt_.load()) - start;
            if (now > 0) packets = pipeline_->buffer->snapshot(replay, now);
        }
        if (packets.empty()) {
            saving_ = false;
            return L"nothing has been recorded yet";
        }
        const auto codec = pipeline_->settings.codec;
        ClipFormat format{pipeline_->output, pipeline_->settings.fps, pipeline_->bitrate.averageKbps, codec,
                          pipeline_->encoder ? pipeline_->encoder->sequenceHeader() : pipeline_->sequenceHeader, now};
        format.preserveCaptureTime = pipeline_->preserveCaptureTime;
        if (auto inBand = annexb::parameterSets(codec, packets.front()->data); !inBand.empty()) format.sequenceHeader = std::move(inBand);
        // Audio for exactly the clip's span, on the shared capture clock. Copied
        // and mixed on the save thread; stop() waits for it before freeing tracks.
        const AudioTrack* desktopTrack = pipeline_->desktopAudio.get();
        const AudioTrack* micTrack = pipeline_->micAudio.get();
        const AudioTrack* discordTrack = pipeline_->discordAudio.get();
        const AudioTrack* gameTrack = pipeline_->gameAudio.get();
        const auto desktopEndpoint = pipeline_->desktopEndpoint;
        const auto gameProcess = pipeline_->gameProcess;
        const auto* lastGameOutput = &pipeline_->gameOutput; // Like the tracks, outlived by stop() waiting for this save.
        const auto from = start + packets.front()->time;
        const auto to = start + now;
        const auto* timing = pipeline_->timing.get();
        const bool throttled = sourceThrottled_;
        const int frameBudget = pipeline_->legacyPacing ? 0 : FrameRateBudget::burstFrames;
        if (saver_.joinable()) saver_.join();
        saver_ = std::thread([this, packets = std::move(packets), format = std::move(format), path, done = std::move(done),
                              desktopTrack, micTrack, discordTrack, gameTrack, desktopEndpoint, gameProcess, lastGameOutput, from, to, start, timing, throttled, micFilters = pipeline_->micFilters,
                              levels = pipeline_->audioLevels, frameBudget]() mutable {
            // Disk work must never compete with the game for CPU time.
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            std::optional<std::wstring> error;
            std::filesystem::path placed = path; // The name the clip ends up with (see writeClip).
            try {
                auto desktop = desktopTrack ? desktopTrack->read(from, to) : std::vector<std::int16_t>{};
                auto mic = micTrack ? micTrack->read(from, to) : std::vector<std::int16_t>{};
                auto discord = discordTrack ? discordTrack->read(from, to) : std::vector<std::int16_t>{};
                auto game = gameTrack ? gameTrack->read(from, to) : std::vector<std::int16_t>{};
                const auto silent = [](const std::vector<std::int16_t>& pcm) {
                    return std::all_of(pcm.begin(), pcm.end(), [](std::int16_t sample) { return sample == 0; });
                };
                // Like Discord, the game track is saved only in clips where the game made a sound.
                if (silent(game)) game.clear();
                // Discord gets a track only in clips where it made a sound, and not when the
                // chosen desktop output already holds it (it would play twice in the mix).
                if (silent(discord) || (!desktopEndpoint.empty() && discordPlaysOn(desktopEndpoint))) discord.clear();
                applyMicFilters(mic, micFilters); // Here, not while capturing: no cost during play.
                // A chosen output carries the game unless it verifiably plays elsewhere (unknown counts as contained).
                // The game and its process are usually gone by now, so a live answer is only one of two sources: the
                // routing seen while recording applies when Windows cannot say.
                const GameOutput live = game.empty() || !gameProcess || desktopEndpoint.empty() ? GameOutput::Unknown : gamePlaysOn(desktopEndpoint, gameProcess());
                const bool gameInDesktop = gameInDesktopAudio(desktopEndpoint.empty(), knownGameOutput(live, lastGameOutput->load()));
                auto prepared = prepareAudioTracks(std::move(desktop), std::move(mic), levels, std::move(discord), std::move(game), gameInDesktop);
                format.audio = std::move(prepared.tracks);
                format.audioNames = std::move(prepared.names);
                error = writeClip(path, packets, format, &placed);
            } catch (const std::exception&) {
                error = L"could not save the clip: insufficient memory or storage";
            } catch (const winrt::hresult_error&) {
                error = L"windows could not finish saving the clip";
            }
            if (!error && timing) {
                try {
                    auto timingPath = placed;
                    timingPath += L".timing.csv";
                    timing->write(timingPath, start, from, to, format.fps, format.size.width,
                                  format.size.height, static_cast<int>(format.codec), throttled, format.preserveCaptureTime,
                                  packets.back()->time, packets.size(), frameBudget);
                } catch (...) {} // Diagnostics never turn a successful clip into a failed save.
            }
            saving_ = false;
            if (done) done(std::move(error), placed);
        });
        return std::nullopt;
    } catch (const std::exception&) {
        saving_ = false;
        return L"could not prepare the clip: insufficient memory or worker resources";
    }
}

RecorderState Recorder::state() const {
    std::lock_guard lock(stateMutex_);
    return state_;
}

std::wstring Recorder::message() const {
    std::lock_guard lock(stateMutex_);
    return message_;
}

RecorderStats Recorder::stats() const {
    std::lock_guard control(mutex_);
    RecorderStats stats;
    if (!pipeline_) return stats;
    stats.framesCaptured = pipeline_->captured;
    stats.framesEncoded = pipeline_->encoded;
    stats.framesSkipped = pipeline_->skipped;
    stats.framesDropped = pipeline_->dropped;
    stats.bufferedBytes = pipeline_->buffer->bytes();
    // What a save would hold: the buffer keeps headroom beyond the replay length, which a clip never uses.
    stats.bufferedSeconds = std::min(static_cast<double>(pipeline_->buffer->span()) / second,
                                     static_cast<double>(pipeline_->settings.replaySeconds));
    if (pipeline_->desktopAudio) {
        stats.desktopAudioSeconds = static_cast<double>(pipeline_->desktopAudio->framesWritten()) / AudioTrack::rate;
        stats.desktopAudioActive = pipeline_->desktopCapture && pipeline_->desktopCapture->active();
        stats.desktopExcludesSelf = pipeline_->desktopCapture && pipeline_->desktopCapture->excludesSelf();
        stats.desktopExcludesDiscord = pipeline_->desktopCapture && pipeline_->desktopCapture->excludesDiscord();
        stats.discordUnavailable = (pipeline_->desktopCapture && pipeline_->desktopCapture->processLoopbackFailed()) ||
            (pipeline_->discordCapture && pipeline_->discordCapture->processLoopbackFailed());
    }
    if (pipeline_->micAudio) {
        stats.micAudioSeconds = static_cast<double>(pipeline_->micAudio->framesWritten()) / AudioTrack::rate;
        stats.micAudioActive = pipeline_->micCapture && pipeline_->micCapture->active();
    }
    return stats;
}

} // namespace refarter::capture
