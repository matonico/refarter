#include "audio_capture.h"
#include "audio_device_state.h"
#include "environment.h"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <algorithm>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <winrt/base.h>
#include <initguid.h> // Defines PKEY_Device_FriendlyName here instead of needing a library.
#include <functiondiscoverykeys_devpkey.h>

namespace refarter::capture {

namespace {
// Shared-mode buffer: room for scheduling hiccups; events still arrive every engine period.
constexpr REFERENCE_TIME bufferDuration = 1'000'000; // 100 ms.

std::wstring endpointId(IMMDevice* endpoint) {
    LPWSTR id = nullptr;
    if (FAILED(endpoint->GetId(&id)) || !id) return {};
    std::wstring result = id;
    CoTaskMemFree(id);
    return result;
}

struct ActivationDone : winrt::implements<ActivationDone, IActivateAudioInterfaceCompletionHandler> {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~ActivationDone() { if (done) CloseHandle(done); }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) noexcept override {
        SetEvent(done);
        return S_OK;
    }
};

// Everything one process tree plays (`include`), or everything the system plays
// except that tree. Excluding Refarter keeps the save chime out of later replays.
// Throws where process loopback is unavailable.
winrt::com_ptr<IAudioClient> activateProcessLoopback(DWORD pid, bool include, HANDLE stop) {
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = include ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE :
        PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT activation{};
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(params);
    activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    const auto handler = winrt::make_self<ActivationDone>();
    if (!handler->done) winrt::throw_last_error();
    winrt::com_ptr<IActivateAudioInterfaceAsyncOperation> operation;
    winrt::check_hresult(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                                     &activation, handler.get(), operation.put()));
    const HANDLE waits[] = {handler->done, stop};
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) winrt::throw_hresult(E_ABORT);
    HRESULT result = E_FAIL;
    winrt::com_ptr<IUnknown> client;
    winrt::check_hresult(operation->GetActivateResult(&result, client.put()));
    winrt::check_hresult(result);
    return client.as<IAudioClient>();
}

// Whether an activation error says outright that this Windows has no process loopback. Any other failure (a target
// that is gone, an invalid process, a busy audio service) is retryable; the backoff keeps retries cheap on a
// genuinely unsupported system.
bool loopbackUnsupported(HRESULT code) {
    return code == E_NOTIMPL || code == E_NOINTERFACE || code == REGDB_E_CLASSNOTREG ||
           code == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) || code == HRESULT_FROM_WIN32(ERROR_CALL_NOT_IMPLEMENTED);
}

std::wstring friendlyName(IMMDevice* endpoint) {
    winrt::com_ptr<IPropertyStore> properties;
    if (FAILED(endpoint->OpenPropertyStore(STGM_READ, properties.put()))) return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
        name = value.pwszVal;
    PropVariantClear(&value);
    return name;
}

// Active endpoints of one direction with the default marked, for resolveDevice. Names only for the device menu:
// opening a stream asks without them.
std::vector<AudioEndpoint> activeEndpoints(IMMDeviceEnumerator* enumerator, EDataFlow flow, bool named = false) {
    std::vector<AudioEndpoint> endpoints;
    std::wstring defaultId;
    winrt::com_ptr<IMMDevice> standard;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, standard.put()))) defaultId = endpointId(standard.get());
    winrt::com_ptr<IMMDeviceCollection> collection;
    UINT count = 0;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put())) || FAILED(collection->GetCount(&count))) return endpoints;
    for (UINT i = 0; i < count; ++i) {
        winrt::com_ptr<IMMDevice> endpoint;
        if (FAILED(collection->Item(i, endpoint.put()))) continue;
        AudioEndpoint entry{endpointId(endpoint.get()), named ? friendlyName(endpoint.get()) : std::wstring{}};
        if (entry.id.empty()) continue;
        entry.isDefault = entry.id == defaultId;
        endpoints.push_back(std::move(entry));
    }
    return endpoints;
}

} // namespace

// Signals the capture thread when the device it records may have changed:
// a new default for our role, or the fixed endpoint appearing or going away.
struct AudioCapture::DeviceWatcher : winrt::implements<DeviceWatcher, IMMNotificationClient> {
    EDataFlow flow;
    std::wstring fixed;
    HANDLE changed;
    DeviceWatcher(EDataFlow f, std::wstring id, HANDLE c) : flow(f), fixed(std::move(id)), changed(c) {}

    void ours(LPCWSTR id) const {
        if (!fixed.empty() && id && _wcsicmp(id, fixed.c_str()) == 0) SetEvent(changed);
    }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow f, ERole role, LPCWSTR) noexcept override {
        if (fixed.empty() && f == flow && role == eConsole) SetEvent(changed);
        return S_OK;
    }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR id, DWORD) noexcept override { ours(id); return S_OK; }
    STDMETHODIMP OnDeviceAdded(LPCWSTR id) noexcept override { ours(id); return S_OK; }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR id) noexcept override { ours(id); return S_OK; }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) noexcept override { return S_OK; }
};

AudioCapture::AudioCapture(Device device, std::wstring endpointId, AudioTrack* track, std::function<void()> onActive,
                           ProcessTarget target, LevelMeter* meter, std::function<void()> onSound)
    : device_(device), endpointId_(std::move(endpointId)), track_(track), meter_(meter), onActive_(std::move(onActive)), target_(std::move(target)), onSound_(std::move(onSound)),
      stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      changed_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
    if (!stop_ || !changed_) winrt::throw_last_error();
    thread_ = std::thread([this] { run(); });
}

AudioCapture::~AudioCapture() {
    SetEvent(stop_);
    thread_.join();
    CloseHandle(stop_);
    CloseHandle(changed_);
}

void AudioCapture::setActive(bool active) {
    // Report the first result, then only real changes: switching between
    // default devices reopens without a gap being announced.
    active_ = active;
    if (!availability_.set(active) || !onActive_) return;
    onActive_();
}

void AudioCapture::run() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    // MMCSS keeps a busy game from starving audio capture; the work per wakeup is tiny.
    DWORD taskIndex = 0;
    const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
    const bool desktop = device_ == Device::Desktop;
    const bool process = device_ == Device::Process;
    const EDataFlow flow = desktop || process ? eRender : eCapture;
    const HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    winrt::com_ptr<DeviceWatcher> watcher;
    try {
        enumerator = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
        watcher = winrt::make_self<DeviceWatcher>(flow, endpointId_, changed_);
        winrt::check_hresult(enumerator->RegisterEndpointNotificationCallback(watcher.get()));
    } catch (...) {
        enumerator = nullptr;
        setActive(false);
    }

    // Ask Windows for 48 kHz stereo 16-bit whatever the device runs at:
    // its converter does the resampling and channel mixing.
    const auto startStream = [&](IAudioClient* client, winrt::com_ptr<IAudioCaptureClient>& capture) {
        WAVEFORMATEX format{WAVE_FORMAT_PCM, AudioTrack::channels, AudioTrack::rate,
                            AudioTrack::rate * AudioTrack::channels * 2, AudioTrack::channels * 2, 16, 0};
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (desktop || process) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        winrt::check_hresult(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufferDuration, 0, &format, nullptr));
        winrt::check_hresult(client->SetEventHandle(ready));
        winrt::check_hresult(client->GetService(IID_PPV_ARGS(capture.put())));
        winrt::check_hresult(client->Start());
    };
    // System-default desktop audio leaves out Refarter's own sounds where Windows
    // allows it, or Discord while it has its own track (Windows leaves out one
    // process tree per stream). A chosen output stays endpoint loopback: it
    // records that device only, Discord included.
    bool excludeProcess = desktop && endpointId_.empty() && !endpointLoopbackForced();

    // Waits for stop or a device change, at most `timeoutMs` (INFINITE: until one comes). False when stopping.
    const auto pause = [&](DWORD timeoutMs) {
        const HANDLE waits[] = {stop_, changed_};
        return WaitForMultipleObjects(2, waits, FALSE, timeoutMs) != WAIT_OBJECT_0;
    };
    int failures = 0; // Consecutive streams that failed soon after (or while) opening: they back off instead of spinning.
    for (bool running = enumerator != nullptr; running;) {
        winrt::com_ptr<IAudioClient> client;
        winrt::com_ptr<IAudioCaptureClient> capture;
        bool openable = false; // Something should have opened: a failure now is worth trying again, not waiting for news.
        bool loopbackRefused = false; // Windows said no to process loopback: waiting for news beats a timer.
        try {
            excludesSelf_ = excludesDiscord_ = false;
            if (process) {
                // Nothing to record until the target runs; the catch below waits for reopen().
                const DWORD pid = target_ ? target_() : 0;
                if (!pid) winrt::throw_hresult(E_NOT_VALID_STATE);
                // With no output at all Windows cannot say whether process loopback works: not a refusal either.
                if (activeEndpoints(enumerator.get(), flow).empty()) winrt::throw_hresult(AUDCLNT_E_DEVICE_INVALIDATED);
                openable = true;
                try {
                    client = activateProcessLoopback(pid, true, stop_);
                    startStream(client.get(), capture);
                    processLoopbackFailed_ = false;
                } catch (const winrt::hresult_error& e) {
                    // Only a Windows that cannot do it is final. A target that just exited, or a busy device, fails
                    // with other codes and is an ordinary failure: tried again when it runs or reopens.
                    if (loopbackUnsupported(e.code())) processLoopbackFailed_ = loopbackRefused = true;
                    throw;
                }
            }
            if (excludeProcess) {
                // With no output at all Windows cannot say whether process loopback works: that is not a refusal.
                // Wait for an output instead of dropping to endpoint loopback for the rest of the session.
                if (activeEndpoints(enumerator.get(), flow).empty()) winrt::throw_hresult(AUDCLNT_E_DEVICE_INVALIDATED);
                const DWORD other = target_ ? target_() : 0;
                openable = true;
                try {
                    client = activateProcessLoopback(other ? other : GetCurrentProcessId(), false, stop_);
                    startStream(client.get(), capture);
                } catch (const winrt::hresult_error& e) {
                    client = nullptr;
                    capture = nullptr;
                    // Only a Windows without process loopback latches endpoint loopback for the session, which
                    // would double Discord in the mix. Any other failure (a busy device, a target that just
                    // exited) keeps the exclusion and takes the outer catch's backoff.
                    if (!loopbackUnsupported(e.code())) throw;
                    excludeProcess = false;
                    processLoopbackFailed_ = WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0;
                } catch (...) {
                    client = nullptr;
                    capture = nullptr;
                    throw;
                }
                excludesSelf_ = client && !other;
                excludesDiscord_ = client && other;
            }
            if (!client) {
                // The default endpoint, or only the chosen one: unplugged or disabled means waiting, never another device.
                AudioDevice choice{endpointId_.empty() ? AudioDevice::Mode::SystemDefault : AudioDevice::Mode::Fixed, endpointId_, {}};
                const auto resolved = resolveDevice(choice, activeEndpoints(enumerator.get(), flow));
                if (resolved.state != DeviceResolution::State::Open) winrt::throw_hresult(AUDCLNT_E_DEVICE_INVALIDATED);
                winrt::com_ptr<IMMDevice> endpoint;
                winrt::check_hresult(enumerator->GetDevice(resolved.id.c_str(), endpoint.put()));
                openable = true;
                winrt::check_hresult(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()));
                startStream(client.get(), capture);
            }
            setActive(true);
        } catch (...) {
            // No usable device right now: wait for Windows to report a change, without polling. A device that is
            // there but would not open (busy, mid format change) is tried again after a growing delay.
            setActive(false);
            const bool backOff = openable && !loopbackRefused;
            failures = backOff ? failures + 1 : 0;
            running = pause(static_cast<DWORD>(reopenWaitMs(openable, loopbackRefused, failures)));
            continue;
        }

        const ULONGLONG openedAt = GetTickCount64();
        const HANDLE waits[] = {stop_, changed_, ready};
        bool failed = false;
        bool heard = false; // onSound_ runs once per opened stream.
        for (bool open = true; open;) {
            const DWORD signalled = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
            if (signalled == WAIT_OBJECT_0) { running = false; break; }
            if (signalled == WAIT_OBJECT_0 + 1) break; // New default, or the fixed endpoint changed: reopen.
            UINT32 pending = 0;
            HRESULT hr = S_OK;
            while (SUCCEEDED(hr = capture->GetNextPacketSize(&pending)) && pending) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD bufferFlags = 0;
                UINT64 qpcTime = 0; // Already in 100 ns units, the clock video frames use.
                if (FAILED(hr = capture->GetBuffer(&data, &frames, &bufferFlags, nullptr, &qpcTime))) break;
                const bool silent = (bufferFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                const auto* samples = silent ? nullptr : reinterpret_cast<const std::int16_t*>(data);
                if (track_) track_->write(static_cast<std::int64_t>(qpcTime), samples, frames);
                // Once per opened process-loopback stream (never the endpoint fallback); it runs after the buffer is released.
                const bool firstSound = process && samples && !heard && onSound_ &&
                    std::any_of(samples, samples + static_cast<std::size_t>(frames) * AudioTrack::channels, [](std::int16_t s) { return s != 0; });
                if (auto* meter = meter_.load(std::memory_order_relaxed); meter && samples && meter->wanted())
                    meter->push(samples, static_cast<std::size_t>(frames) * AudioTrack::channels);
                capture->ReleaseBuffer(frames);
                if (firstSound) {
                    heard = true;
                    onSound_();
                }
            }
            if (FAILED(hr)) { open = false; failed = true; } // Device invalidated (unplugged, format change): reopen.
        }
        client->Stop();
        // Reopening reports its own result; stopping is not a device change.
        if (!running) active_ = false;
        if (running && !failed) failures = 0; // A device change asked for this reopen; the stream before it was fine.
        // A stream that dies again at once (a driver that opens but cannot deliver) must not make a busy loop.
        // A device change that merely asked for a reopen is not a failure.
        if (running && failed) {
            setActive(false); // The gap is real: meters and the missing-device note show it until the stream is back.
            if (GetTickCount64() - openedAt >= healthyStreamMs) failures = 0;
            if (!pause(static_cast<DWORD>(retryDelayMs(failures)))) running = false;
            ++failures;
        }
    }

    if (enumerator && watcher) enumerator->UnregisterEndpointNotificationCallback(watcher.get());
    watcher = nullptr;
    enumerator = nullptr; // Release COM objects before leaving the apartment.
    CloseHandle(ready);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    winrt::uninit_apartment();
}

bool endpointLoopbackForced() { return environmentIs(L"REFARTER_AUDIO_ENDPOINT_LOOPBACK", L"1"); }

std::vector<AudioEndpoint> listAudioEndpoints(AudioCapture::Device device) {
    try {
        const EDataFlow flow = device == AudioCapture::Device::Desktop ? eRender : eCapture;
        const auto enumerator = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
        return activeEndpoints(enumerator.get(), flow, true);
    } catch (...) {
        return {}; // Listing failed: the menu offers off and default only.
    }
}

} // namespace refarter::capture
