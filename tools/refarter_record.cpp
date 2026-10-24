// Development harness for the capture pipeline: records one process's window,
// reports overhead, saves a clip and decodes it back to verify it.
#include "capture/audio_capture.h"
#include "capture/clip_folder.h"
#include "capture/codec_probe.h"
#include "capture/gpu.h"
#include "capture/recorder.h"
#include "clip_library.h"

#include <windows.h>
#include <mmsystem.h>
#include <d3d11_1.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <pdh.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winrt/base.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <cerrno>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace refarter::capture;
using Clock = std::chrono::steady_clock;

namespace {

void usage() {
    std::fputws(LR"(refarter-record: capture-pipeline test harness

  refarter-record (--process <name.exe|pid> | --monitor | --test-pattern) [options]

  --resolution native|2160|1440|1080|720   encoded height (default native, never upscales)
  --window <handle>                       exact window for --process (startup diagnostics)
  --fps <10-60>                            recording frame rate (default 60)
  --quality small|balanced|high            variable-bitrate preset sized to the picture (default balanced)
  --bitrate <2-250>                        constant bitrate in Mb/s instead of a preset
  --codec h264|hevc|av1                    video codec (default h264; see --probe-codecs)
  --replay <seconds>                       replay length kept in memory (default 30)
  --run <seconds>                          how long to record before saving (default replay + 5)
  --folder <path>                          clip folder (default Videos\Refarter)
  --no-verify                              skip decoding the saved clip
  --self-tone                              play a quiet 1 kHz tone from this process (like the save chime)
  --screenshot                             before saving, also take a screenshot into <folder>\screenshots
  --no-gpu-counters                        skip GPU sampling (it costs this process CPU time)
  --mic                                    also record the default microphone (adds mixed + mic tracks)
  --mic-device <endpoint id>               record this microphone only, never another (see --list-audio-devices)
  --no-desktop-audio                       do not record desktop audio
  --desktop-device <endpoint id>           record everything played on this output only, never another
  --desktop-volume <0-200>                 saved desktop volume percent (default 100)
  --mic-volume <0-200>                     saved microphone volume percent (default 100)
  --noise-suppression / --noise-gate       turn on a microphone filter (both off by default)
  --gate-open <dB> / --gate-close <dB>     noise gate levels (default -26 / -32)
  --monitor                                record the whole primary monitor (with --test-*: pattern on screen)
  --test-pattern                           record a generated moving pattern in a child process
  --test-noise                             same, with incompressible full-screen noise (worst case)
  --test-sync                              same, black with a white flash and a 200 ms tone every 2 s (audio/video offset)
  --test-sparse                            same, but a new picture only every 4 s (static/paused-source replay length)

  refarter-record --verify <clip.mp4>      decode a saved clip and report its tracks
  refarter-record --bench-mic-filters      time the microphone filters on 10 minutes of audio
  refarter-record --bench-audio-levels     time save-time gain on 3 minutes of stereo PCM
  refarter-record --probe-codecs           check which codecs this PC can record and play
  refarter-record --list-audio-devices     list active outputs and microphones with their endpoint ids
  refarter-record --bench-library <folder> time listing a clip folder and its first 12 thumbnails
)", stdout);
}

struct Options {
    std::wstring process;
    HWND window = nullptr;
    bool monitor = false;
    bool testPattern = false;
    bool noise = false;
    bool sync = false;
    bool sparse = false;
    bool gpuCounters = true;
    AudioSettings audio;
    VideoSettings video{Resolution::Native, 60, 50, 30};
    int runSeconds = -1;
    std::filesystem::path folder = defaultClipFolder();
    bool verify = true;
    bool selfTone = false;
    bool screenshot = false;
};

std::optional<Options> parse(int argc, wchar_t** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const auto value = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        const auto number = [&] { return _wtoi(value().c_str()); };
        if (arg == L"--process") o.process = value();
        else if (arg == L"--window") {
            const auto text = value();
            wchar_t* end = nullptr;
            errno = 0;
            const auto handle = std::wcstoull(text.c_str(), &end, 0);
            if (text.empty() || text.front() == L'-' || end == text.c_str() || *end || errno == ERANGE ||
                !handle || handle > std::numeric_limits<UINT_PTR>::max()) return std::nullopt;
            o.window = reinterpret_cast<HWND>(static_cast<UINT_PTR>(handle));
        }
        else if (arg == L"--monitor") o.monitor = true;
        else if (arg == L"--test-pattern") o.testPattern = true;
        else if (arg == L"--test-noise") o.testPattern = o.noise = true;
        else if (arg == L"--test-sync") o.testPattern = o.sync = true;
        else if (arg == L"--test-sparse") o.testPattern = o.sparse = true;
        else if (arg == L"--no-gpu-counters") o.gpuCounters = false;
        else if (arg == L"--mic") o.audio.microphone = {AudioDevice::Mode::SystemDefault};
        else if (arg == L"--mic-device") o.audio.microphone = {AudioDevice::Mode::Fixed, value()};
        else if (arg == L"--no-desktop-audio") o.audio.desktop = {};
        else if (arg == L"--desktop-device") o.audio.desktop = {AudioDevice::Mode::Fixed, value()};
        else if (arg == L"--desktop-volume") o.audio.levels.desktopPercent = number();
        else if (arg == L"--mic-volume") o.audio.levels.microphonePercent = number();
        else if (arg == L"--noise-suppression") o.audio.micFilters.noiseSuppression = true;
        else if (arg == L"--noise-gate") o.audio.micFilters.noiseGate = true;
        else if (arg == L"--gate-open") o.audio.micFilters.gateOpenDb = static_cast<float>(_wtof(value().c_str()));
        else if (arg == L"--gate-close") o.audio.micFilters.gateCloseDb = static_cast<float>(_wtof(value().c_str()));
        else if (arg == L"--fps") o.video.fps = number();
        else if (arg == L"--bitrate") { o.video.bitrateMbps = number(); o.video.quality = Quality::Custom; }
        else if (arg == L"--quality") {
            const auto q = value();
            if (q == L"small") o.video.quality = Quality::Small;
            else if (q == L"balanced") o.video.quality = Quality::Balanced;
            else if (q == L"high") o.video.quality = Quality::High;
            else return std::nullopt;
        } else if (arg == L"--codec") {
            const auto c = value();
            if (c == L"h264") o.video.codec = Codec::H264;
            else if (c == L"hevc") o.video.codec = Codec::HEVC;
            else if (c == L"av1") o.video.codec = Codec::AV1;
            else return std::nullopt;
        }
        else if (arg == L"--replay") o.video.replaySeconds = number();
        else if (arg == L"--run") o.runSeconds = number();
        else if (arg == L"--folder") o.folder = std::filesystem::absolute(value());
        else if (arg == L"--no-verify") o.verify = false;
        else if (arg == L"--self-tone") o.selfTone = true;
        else if (arg == L"--screenshot") o.screenshot = true;
        else if (arg == L"--resolution") {
            const auto r = value();
            if (r == L"native") o.video.resolution = Resolution::Native;
            else if (r == L"2160") o.video.resolution = Resolution::P2160;
            else if (r == L"1440") o.video.resolution = Resolution::P1440;
            else if (r == L"1080") o.video.resolution = Resolution::P1080;
            else if (r == L"720") o.video.resolution = Resolution::P720;
            else return std::nullopt;
        } else return std::nullopt;
    }
    if (o.monitor ? !o.process.empty() : o.process.empty() == !o.testPattern) return std::nullopt;
    if (o.window && (o.process.empty() || o.monitor || o.testPattern)) return std::nullopt;
    if (o.runSeconds < 0) o.runSeconds = o.video.replaySeconds + 5;
    return o;
}

// --- Target selection -------------------------------------------------------

std::vector<DWORD> processIds(const std::wstring& nameOrId) {
    if (!nameOrId.empty() && std::all_of(nameOrId.begin(), nameOrId.end(), iswdigit)) return {static_cast<DWORD>(_wtoi(nameOrId.c_str()))};
    std::vector<DWORD> ids;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, nameOrId.c_str()) == 0) ids.push_back(entry.th32ProcessID);
    }
    CloseHandle(snapshot);
    return ids;
}

// The process's largest visible, uncloaked top-level window: its main window.
HWND mainWindow(const std::vector<DWORD>& ids, std::wstring* exeName = nullptr) {
    struct Search { const std::vector<DWORD>* ids; HWND best; long long area; } search{&ids, nullptr, 0};
    EnumWindows([](HWND window, LPARAM param) -> BOOL {
        auto& s = *reinterpret_cast<Search*>(param);
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (std::find(s.ids->begin(), s.ids->end(), pid) == s.ids->end()) return TRUE;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (!IsWindowVisible(window) || cloaked || GetWindow(window, GW_OWNER) ||
            (GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
        RECT r{};
        GetClientRect(window, &r);
        const long long area = static_cast<long long>(r.right) * r.bottom;
        if (area > s.area) { s.area = area; s.best = window; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    if (search.best && exeName) {
        DWORD pid = 0;
        GetWindowThreadProcessId(search.best, &pid);
        if (const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t path[MAX_PATH];
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, path, &size)) *exeName = std::filesystem::path(path).filename().wstring();
            CloseHandle(process);
        }
    }
    return search.best;
}

// --- Test pattern (runs in a child process so its cost is not measured) -----

// A 1 kHz sine as an in-memory 48 kHz stereo 16-bit WAV; by default one quiet second (-30 dBFS).
std::vector<std::uint8_t> toneWav(int amplitude = 1036, std::uint32_t milliseconds = 1000) {
    constexpr std::uint32_t rate = 48'000;
    const std::uint32_t frames = rate * milliseconds / 1000, bytes = frames * 4;
    std::vector<std::uint8_t> wav(44 + bytes);
    const auto put = [&](std::size_t at, std::uint32_t value, int size) {
        for (int i = 0; i < size; ++i) wav[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    std::memcpy(wav.data(), "RIFF", 4);
    put(4, 36 + bytes, 4);
    std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
    put(16, 16, 4); put(20, 1, 2); put(22, 2, 2); put(24, rate, 4); put(28, rate * 4, 4); put(32, 4, 2); put(34, 16, 2);
    std::memcpy(wav.data() + 36, "data", 4);
    put(40, bytes, 4);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const auto sample = static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(amplitude * std::sin(2 * 3.14159265358979 * 1000 * i / rate))));
        put(44 + i * 4, sample, 2);
        put(46 + i * 4, sample, 2);
    }
    return wav;
}

int renderPattern(bool noise, bool sync, bool sparse) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = [](HWND w, UINT m, WPARAM wp, LPARAM lp) -> LRESULT {
        if (m == WM_DESTROY) PostQuitMessage(0);
        return DefWindowProcW(w, m, wp, lp);
    };
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RefarterTestPattern";
    RegisterClassExW(&wc);
    RECT bounds{0, 0, 1920, 1080};
    AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);
    const HWND window = CreateWindowExW(0, wc.lpszClassName, L"refarter test pattern", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        40, 40, bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, wc.hInstance, nullptr);

    // Render on the GPU driving the monitor, like a game would.
    auto gpu = createGpuForWindow(window);
    auto device = gpu.device;
    auto context = gpu.context;
    auto factory = device.as<IDXGIDevice>();
    winrt::com_ptr<IDXGIAdapter> adapter;
    factory->GetAdapter(adapter.put());
    winrt::com_ptr<IDXGIFactory2> dxgi;
    adapter->GetParent(IID_PPV_ARGS(dxgi.put()));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    winrt::com_ptr<IDXGISwapChain1> swapChain;
    winrt::check_hresult(dxgi->CreateSwapChainForHwnd(device.get(), window, &desc, nullptr, nullptr, swapChain.put()));
    winrt::com_ptr<ID3D11Texture2D> back;
    swapChain->GetBuffer(0, IID_PPV_ARGS(back.put()));
    winrt::com_ptr<ID3D11RenderTargetView> target;
    device->CreateRenderTargetView(back.get(), nullptr, target.put());
    auto context1 = context.as<ID3D11DeviceContext1>();

    // Noise: random windows into a large random texture each frame. Motion
    // search finds nothing, so this is the encoder's worst case.
    constexpr UINT noiseSize = 4096;
    winrt::com_ptr<ID3D11Texture2D> noiseTexture;
    if (noise) {
        std::vector<std::uint32_t> pixels(noiseSize * noiseSize);
        std::uint32_t state = 0x9e3779b9;
        for (auto& p : pixels) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            p = state | 0xff000000;
        }
        D3D11_TEXTURE2D_DESC noiseDesc{noiseSize, noiseSize, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE};
        const D3D11_SUBRESOURCE_DATA data{pixels.data(), noiseSize * 4, 0};
        winrt::check_hresult(device->CreateTexture2D(&noiseDesc, &data, noiseTexture.put()));
    }

    // Full-screen colour shifts plus fast-moving bars: hard, high-motion content.
    const auto start = Clock::now();
    std::uint32_t random = 12345;
    const auto clickWav = toneWav(8000, 200); // About -12 dBFS.
    long long lastFlash = -1;
    for (MSG msg{};;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            DispatchMessageW(&msg);
        }
        if (sync) {
            // Audio/video offset check: black, with one white frame and a click every 2 s.
            // White lasts 100 ms so the recording-rate cap cannot skip it; its first frame marks the click.
            const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            const bool white = elapsed % 2000 < 100;
            const bool click = white && elapsed / 2000 != lastFlash;
            if (click) lastFlash = elapsed / 2000;
            const float level = white ? 1.f : 0.f;
            const float colour[4] = {level, level, level, 1};
            context1->ClearView(target.get(), colour, nullptr, 0);
            swapChain->Present(1, 0);
            if (click) PlaySoundW(reinterpret_cast<LPCWSTR>(clickWav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
            continue;
        }
        if (noise) {
            random = random * 1664525 + 1013904223;
            const UINT x = (random >> 8) % (noiseSize - 1920);
            const UINT y = (random >> 20) % (noiseSize - 1080);
            const D3D11_BOX box{x, y, 0, x + 1920, y + 1080, 1};
            context->CopySubresourceRegion(back.get(), 0, 0, 0, 0, noiseTexture.get(), 0, &box);
            swapChain->Present(1, 0);
            continue;
        }
        const float t = std::chrono::duration<float>(Clock::now() - start).count();
        const float background[4] = {0.5f + 0.5f * std::sin(t), 0.5f + 0.5f * std::sin(t * 1.3f + 2), 0.5f + 0.5f * std::sin(t * 0.7f + 4), 1};
        context1->ClearView(target.get(), background, nullptr, 0);
        for (int bar = 0; bar < 12; ++bar) {
            const LONG x = static_cast<LONG>(std::fmod(t * (400 + bar * 90) + bar * 160, 1920.f));
            const D3D11_RECT rect{x, bar * 90, x + 60, bar * 90 + 80};
            const float colour[4] = {bar % 2 ? 1.f : 0.f, bar % 3 ? 1.f : 0.f, 1.f, 1.f};
            context1->ClearView(target.get(), colour, &rect, 1);
        }
        swapChain->Present(1, 0);
        // Sparse: hold each picture, as a paused game or static screen would; still answer messages.
        if (sparse) MsgWaitForMultipleObjects(0, nullptr, FALSE, 4000, QS_ALLINPUT);
    }
}

// --- Measurement -------------------------------------------------------------

std::uint64_t processCpu100ns() {
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const auto value = [](FILETIME f) { return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return value(kernel) + value(user);
}

// Per-engine GPU utilisation of one process, from the Windows "GPU Engine" counters.
class GpuCounters {
public:
    explicit GpuCounters(DWORD pid) {
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) return;
        const auto path = L"\\GPU Engine(pid_" + std::to_wstring(pid) + L"_*)\\Utilization Percentage";
        if (PdhAddEnglishCounterW(query_, path.c_str(), 0, &counter_) != ERROR_SUCCESS) counter_ = nullptr;
        PdhCollectQueryData(query_);
    }
    ~GpuCounters() { if (query_) PdhCloseQuery(query_); }
    // Engine type -> percentage since the previous call.
    std::map<std::wstring, double> sample() {
        std::map<std::wstring, double> engines;
        if (!counter_ || PdhCollectQueryData(query_) != ERROR_SUCCESS) return engines;
        DWORD bytes = 0, count = 0;
        PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
        std::vector<std::byte> storage(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
        if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return engines;
        for (DWORD i = 0; i < count; ++i) {
            std::wstring name = items[i].szName;
            const auto at = name.find(L"engtype_");
            if (at == std::wstring::npos || items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
            engines[name.substr(at + 8)] += items[i].FmtValue.doubleValue;
        }
        return engines;
    }

private:
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER counter_ = nullptr;
};

// --- Verification --------------------------------------------------------------

void verify(const std::filesystem::path& path, int fps) {
    constexpr auto video = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    winrt::com_ptr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, reader.put()))) {
        std::wprintf(L"verify: FAILED to open clip\n");
        return;
    }
    winrt::com_ptr<IMFMediaType> native;
    reader->GetNativeMediaType(video, 0, native.put());
    UINT32 width = 0, height = 0;
    MFGetAttributeSize(native.get(), MF_MT_FRAME_SIZE, &width, &height);
    PROPVARIANT duration{};
    reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration);
    // Decode every frame: proves the clip plays, not just that it parses.
    winrt::com_ptr<IMFMediaType> decoded;
    MFCreateMediaType(decoded.put());
    decoded->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    decoded->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    const bool decoding = SUCCEEDED(reader->SetCurrentMediaType(video, nullptr, decoded.get()));
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(video, TRUE);
    std::uint64_t frames = 0, errors = 0, gaps = 0;
    LONGLONG previous = -1;
    for (;;) {
        DWORD flags = 0;
        LONGLONG time = 0;
        winrt::com_ptr<IMFSample> sample;
        if (FAILED(reader->ReadSample(video, 0, nullptr, &flags, &time, sample.put()))) { ++errors; break; }
        if (flags & MF_SOURCE_READERF_ERROR) { ++errors; break; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;
        ++frames;
        if (previous >= 0 && time - previous > 15'000'000 / fps) ++gaps;
        previous = time;
    }
    const double seconds = duration.uhVal.QuadPart / 1e7;
    GUID subtype{};
    native->GetGUID(MF_MT_SUBTYPE, &subtype);
    const char fourcc[5] = {static_cast<char>(subtype.Data1), static_cast<char>(subtype.Data1 >> 8),
                            static_cast<char>(subtype.Data1 >> 16), static_cast<char>(subtype.Data1 >> 24), 0};
    // Coded sizes are padded to the codec's block size; players show the aperture.
    MFVideoArea aperture{};
    if (SUCCEEDED(native->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&aperture), sizeof(aperture), nullptr))) {
        std::wprintf(L"verify: coded %ux%u, display aperture %ldx%ld\n", width, height, aperture.Area.cx, aperture.Area.cy);
    }
    std::error_code sizeError;
    const auto bytes = std::filesystem::file_size(path, sizeError);
    std::wprintf(L"verify: %hs %ux%u, %.2f s, %.1f Mb/s overall, %llu frames %s (%.2f fps), %llu gaps > 1.5 frames, %llu errors\n",
                 fourcc, width, height, seconds, seconds > 0 ? bytes * 8 / seconds / 1e6 : 0.0, frames,
                 decoding ? L"decoded" : L"read (no decoder)", seconds > 0 ? frames / seconds : 0.0, gaps, errors);
    PropVariantClear(&duration);

    // Decode each audio track: length, and loudness to show sound was captured.
    for (DWORD stream = 0;; ++stream) {
        winrt::com_ptr<IMFMediaType> type;
        if (FAILED(reader->GetNativeMediaType(stream, 0, type.put()))) break;
        GUID major{};
        type->GetGUID(MF_MT_MAJOR_TYPE, &major);
        if (major != MFMediaType_Audio) continue;
        winrt::com_ptr<IMFSourceReader> audio;
        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, audio.put()))) continue;
        audio->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        audio->SetStreamSelection(stream, TRUE);
        winrt::com_ptr<IMFMediaType> pcm;
        MFCreateMediaType(pcm.put());
        pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        audio->SetCurrentMediaType(stream, nullptr, pcm.get());
        winrt::com_ptr<IMFMediaType> actual;
        audio->GetCurrentMediaType(stream, actual.put());
        const UINT32 rate = MFGetAttributeUINT32(actual.get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        const UINT32 channels = MFGetAttributeUINT32(actual.get(), MF_MT_AUDIO_NUM_CHANNELS, 2);
        std::uint64_t samples = 0, loud = 0;
        int peak = 0;
        for (;;) {
            DWORD flags = 0;
            winrt::com_ptr<IMFSample> sample;
            if (FAILED(audio->ReadSample(stream, 0, nullptr, &flags, nullptr, sample.put())) ||
                (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) break;
            if (!sample) continue;
            winrt::com_ptr<IMFMediaBuffer> buffer;
            sample->ConvertToContiguousBuffer(buffer.put());
            BYTE* data = nullptr;
            DWORD length = 0;
            buffer->Lock(&data, nullptr, &length);
            const auto* values = reinterpret_cast<const std::int16_t*>(data);
            for (DWORD i = 0; i < length / 2; ++i) {
                const int level = std::abs(static_cast<int>(values[i]));
                peak = std::max(peak, level);
                if (level > 328) ++loud; // Above -40 dBFS.
            }
            buffer->Unlock();
            samples += length / 2;
        }
        // The reader's stream order is not the file's track order; report the MP4 track ID.
        DWORD trackId = 0;
        winrt::com_ptr<IMFMediaSource> source;
        winrt::com_ptr<IMFPresentationDescriptor> presentation;
        winrt::com_ptr<IMFStreamDescriptor> descriptor;
        BOOL selected = FALSE;
        if (SUCCEEDED(audio->GetServiceForStream(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), GUID_NULL, IID_PPV_ARGS(source.put()))) &&
            SUCCEEDED(source->CreatePresentationDescriptor(presentation.put())) &&
            SUCCEEDED(presentation->GetStreamDescriptorByIndex(stream, &selected, descriptor.put()))) {
            descriptor->GetStreamIdentifier(&trackId);
        }
        std::wprintf(L"verify: audio track %lu%s: %.2f s, peak %.1f dBFS, %.1f%% of samples above -40 dBFS\n", trackId,
                     selected ? L" (selected by default)" : L"",
                     samples / static_cast<double>(channels) / rate, peak ? 20 * std::log10(peak / 32768.0) : -120.0,
                     samples ? 100.0 * loud / samples : 0.0);
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment();
    if (argc >= 2 && std::wstring(argv[1]) == L"--render-pattern") return renderPattern(argc == 3 && std::wstring(argv[2]) == L"noise",
        argc == 3 && std::wstring(argv[2]) == L"sync", argc == 3 && std::wstring(argv[2]) == L"sparse");
    if (argc == 2 && std::wstring(argv[1]) == L"--bench-audio-levels") {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        std::vector<std::int16_t> input(48'000 * 180 * 2);
        for (std::size_t i = 0; i < input.size(); ++i)
            input[i] = static_cast<std::int16_t>(static_cast<int>(i % 60001) - 30000);
        for (const int percent : {0, 50, 100, 200}) {
            for (int run = 1; run <= 3; ++run) {
                auto pcm = input; // Allocation/copy excluded: gain operates in place.
                const auto begin = Clock::now();
                applyAudioLevel(pcm, percent);
                const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
                std::int64_t sum = 0;
                for (const auto sample : pcm) sum += sample;
                std::wprintf(L"audio gain: %d%%, run %d, 180 s stereo, %.3f ms, checksum %lld\n", percent, run, elapsed, sum);
            }
        }
        return 0;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--bench-mic-filters") {
        // Ten minutes of noisy speech-level audio through both filters, as a save would.
        std::vector<std::int16_t> pcm(48'000 * 600 * 2);
        std::uint32_t seed = 1;
        for (auto& s : pcm) { seed = seed * 1664525 + 1013904223; s = static_cast<std::int16_t>((seed >> 20) - 2048); }
        const auto begin = Clock::now();
        MicFilters both; // Both filters on, whatever the defaults.
        both.noiseSuppression = both.noiseGate = true;
        applyMicFilters(pcm, both);
        std::wprintf(L"mic filters: 600 s of audio in %.0f ms\n", std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
        return 0;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"--bench-library") {
        // The library page's own worker: list the folder, then thumbnails for one screen of tiles.
        std::mutex mutex;
        std::condition_variable ready;
        bool waiting = false;
        refarter::ClipLibrary library([&] { std::lock_guard lock(mutex); waiting = true; ready.notify_one(); });
        const auto take = [&] {
            std::unique_lock lock(mutex);
            ready.wait(lock, [&] { return waiting; });
            waiting = false;
            return library.take();
        };
        auto begin = Clock::now();
        library.scan(argv[2]);
        std::vector<refarter::ClipFile> clips;
        for (auto results = take();; results = take()) if (results.clips) { clips = std::move(*results.clips); break; }
        std::wprintf(L"listed %zu clips in %.1f ms\n", clips.size(), std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
        std::wprintf(L"clips in folder: %s\n", refarter::formatClipBytes(refarter::clipBytes(clips)).c_str());
        std::vector<refarter::ClipFile> wanted;
        for (std::size_t i = 0; i < clips.size() && i < 12; ++i) wanted.push_back(clips[i]);
        begin = Clock::now();
        library.want(wanted, {232, 130});
        std::size_t done = 0, missing = 0;
        while (done < wanted.size()) {
            for (auto& thumbnail : take().thumbnails) {
                ++done;
                std::wprintf(L"  duration: %s\n", thumbnail.duration ? refarter::formatClipDuration(*thumbnail.duration).c_str() : L"unavailable");
                if (thumbnail.bitmap) DeleteObject(thumbnail.bitmap); else ++missing;
            }
        }
        std::wprintf(L"%zu thumbnails (%zu unavailable) in %.0f ms\n", done, missing, std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
        return 0;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--probe-codecs") {
        const auto begin = Clock::now();
        const auto results = probeCodecs(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
        const wchar_t* names[] = {L"h.264", L"hevc ", L"av1  "};
        for (const auto& r : results) {
            std::wprintf(L"%s  record: %s  play: %s  %s%s\n", names[static_cast<int>(r.codec)], r.records ? L"yes" : L"no ",
                         r.plays ? L"yes" : L"no ", r.encoder.c_str(), r.problem.empty() ? L"" : (L" (" + r.problem + L")").c_str());
        }
        std::wprintf(L"checked in %.0f ms\n", std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
        return 0;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--list-audio-devices") {
        for (const auto device : {AudioCapture::Device::Desktop, AudioCapture::Device::Microphone}) {
            std::wprintf(device == AudioCapture::Device::Desktop ? L"outputs:\n" : L"microphones:\n");
            const auto endpoints = listAudioEndpoints(device);
            const auto labels = endpointLabels(endpoints);
            for (size_t i = 0; i < endpoints.size(); ++i)
                std::wprintf(L"  %s%s\n    %s\n", labels[i].c_str(), endpoints[i].isDefault ? L" (default)" : L"", endpoints[i].id.c_str());
            if (endpoints.empty()) std::wprintf(L"  none active\n");
        }
        return 0;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"--verify") {
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
        verify(argv[2], 60);
        MFShutdown();
        return 0;
    }
    const auto options = parse(argc, argv);
    if (!options) { usage(); return 2; }
    if (auto error = validate(options->video)) { std::wprintf(L"%s\n", error->c_str()); return 2; }
    if (auto error = validate(options->audio.levels)) { std::wprintf(L"%s\n", error->c_str()); return 2; }
    if (auto error = prepareClipFolder(options->folder)) { std::wprintf(L"clip folder: %s\n", error->c_str()); return 2; }

    PROCESS_INFORMATION child{};
    std::vector<DWORD> ids;
    if (options->testPattern) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(self) + L"\" --render-pattern" +
            (options->noise ? L" noise" : options->sync ? L" sync" : options->sparse ? L" sparse" : L"");
        STARTUPINFOW startup{sizeof(startup)};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child)) return 1;
        ids = {child.dwProcessId};
    } else {
        ids = processIds(options->process);
    }
    HWND window = nullptr;
    std::wstring game = options->testPattern ? L"test pattern" : options->process;
    if (options->window) {
        DWORD pid = 0;
        GetWindowThreadProcessId(options->window, &pid);
        if (!IsWindow(options->window) || std::find(ids.begin(), ids.end(), pid) == ids.end()) {
            std::fputws(L"the window does not belong to the selected process\n", stderr);
            return 1;
        }
        window = options->window;
        RECT client{};
        GetClientRect(window, &client);
        std::wprintf(L"exact source: visible=%d; minimised=%d; foreground=%d; client=%ldx%ld\n",
                     IsWindowVisible(window), IsIconic(window), GetForegroundWindow() == window, client.right, client.bottom);
        std::fflush(stdout);
    }
    for (int attempt = 0; attempt < 50 && !window && !ids.empty(); ++attempt) {
        window = mainWindow(ids, options->testPattern ? nullptr : &game);
        if (!window) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!window && !options->monitor) { std::wprintf(L"no visible window found for %s\n", game.c_str()); return 1; }
    if (options->testPattern) std::this_thread::sleep_for(std::chrono::seconds(1)); // Let it present.

    CaptureSource source;
    if (options->monitor) {
        source.monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        game = L"Desktop";
    } else {
        source.window = window;
    }
    Recorder recorder;
    const auto opening = Clock::now();
    if (auto error = recorder.start(source, options->video, options->audio)) { std::wprintf(L"%s\n", error->c_str()); return 1; }
    const double openMs = std::chrono::duration<double, std::milli>(Clock::now() - opening).count();
    const auto size = recorder.outputSize();
    const auto rate = recorder.bitrate();
    const wchar_t* codecs[] = {L"h.264", L"hevc", L"av1"};
    std::wprintf(L"recording %s: %dx%d @ %d fps, %s, %s %.1f Mb/s (peak %.1f), %d s replay\n  gpu: %s\n  encoder: %s\n  source throttling: %s\n  started in %.0f ms\n",
                 game.c_str(), size.width, size.height, options->video.fps, codecs[static_cast<int>(options->video.codec)],
                 rate.constant ? L"cbr" : L"vbr", rate.averageKbps / 1000.0, rate.peakKbps / 1000.0, options->video.replaySeconds,
                 recorder.adapterName().c_str(), recorder.encoderName().c_str(),
                 recorder.sourceThrottled() ? L"yes" : L"no (needs SDK and OS support for MinUpdateInterval)", openMs);

    std::optional<GpuCounters> gpu;
    if (options->gpuCounters) gpu.emplace(GetCurrentProcessId());
    std::map<std::wstring, double> gpuTotals;
    int gpuSamples = 0;
    const auto cpuStart = processCpu100ns();
    const auto wallStart = Clock::now();
    // Played by this process, like the app's save chime: desktop audio should leave it out where Windows allows.
    const auto tone = options->selfTone ? toneWav() : std::vector<std::uint8_t>{};
    if (!tone.empty()) PlaySoundW(reinterpret_cast<LPCWSTR>(tone.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_LOOP | SND_NODEFAULT);
    for (int s = 1; s <= options->runSeconds && recorder.state() == RecorderState::Buffering; ++s) {
        std::this_thread::sleep_until(wallStart + std::chrono::seconds(s));
        const auto stats = recorder.stats();
        const auto engines = gpu ? gpu->sample() : std::map<std::wstring, double>{};
        if (s > 2) { // Skip warm-up.
            for (const auto& [engine, value] : engines) gpuTotals[engine] += value;
            ++gpuSamples;
        }
        PROCESS_MEMORY_COUNTERS_EX now{sizeof(now)};
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&now), sizeof(now));
        std::wprintf(L"  %3ds  captured %6llu  encoded %6llu  skipped %6llu  dropped %4llu  buffer %5.1f s %7.1f MB  other %5.1f MB  audio %5.1f s%s\n", s,
                     stats.framesCaptured, stats.framesEncoded, stats.framesSkipped, stats.framesDropped,
                     stats.bufferedSeconds, stats.bufferedBytes / 1e6, (now.PrivateUsage - stats.bufferedBytes) / 1e6,
                     stats.desktopAudioSeconds, options->audio.desktop.enabled() && !stats.desktopAudioActive ? L" (no device)" :
                     options->audio.microphone.enabled() && !stats.micAudioActive ? L" (no microphone)" : L"");
    }
    if (!tone.empty()) PlaySoundW(nullptr, nullptr, 0);
    const double wall = std::chrono::duration<double>(Clock::now() - wallStart).count();
    const double cpu = (processCpu100ns() - cpuStart) / 1e7;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    PROCESS_MEMORY_COUNTERS_EX memory{sizeof(memory)};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    const auto stats = recorder.stats();
    std::wprintf(L"\noverhead while buffering (%.1f s):\n  cpu: %.2f%% of one core, %.3f%% of all %lu logical cores\n",
                 wall, 100 * cpu / wall, 100 * cpu / wall / system.dwNumberOfProcessors, system.dwNumberOfProcessors);
    for (const auto& [engine, total] : gpuTotals) std::wprintf(L"  gpu %-16s %.2f%%\n", engine.c_str(), total / std::max(gpuSamples, 1));
    std::wprintf(L"  private memory: %.1f MB (buffer %.1f MB)\n  frames: %llu encoded, %llu dropped\n",
                 memory.PrivateUsage / 1e6, stats.bufferedBytes / 1e6, stats.framesEncoded, stats.framesDropped);
    if (options->audio.desktop.enabled())
        std::wprintf(L"  desktop audio: %s\n", stats.desktopExcludesSelf ? L"process loopback without this process" :
            stats.desktopExcludesDiscord ? L"process loopback without discord (includes this process)" :
            L"endpoint loopback (includes this process)");
    if (recorder.state() != RecorderState::Buffering) std::wprintf(L"  capture stopped: %s\n", recorder.message().c_str());

    std::mutex mutex;
    std::condition_variable saved;
    if (options->screenshot) {
        // The app's path: the next picture, read back and written as a PNG beside the clips.
        std::filesystem::path shot;
        bool shotDone = false;
        std::optional<std::wstring> shotError;
        const auto asked = Clock::now();
        const auto target = [&](std::wstring& problem) -> std::filesystem::path {
            const auto folder = options->folder / L"screenshots";
            if (auto error = prepareClipFolder(folder, ClipFolderKind::Screenshots)) { problem = *error; return {}; }
            return shot = nextClipPath(folder, game, L".png");
        };
        if (auto error = recorder.screenshot(target, [&](std::optional<std::wstring> e) {
                std::lock_guard lock(mutex);
                shotError = std::move(e);
                shotDone = true;
                saved.notify_one();
            })) {
            std::wprintf(L"screenshot: %s\n", error->c_str());
        } else {
            std::unique_lock lock(mutex);
            saved.wait(lock, [&] { return shotDone; });
            const double shotMs = std::chrono::duration<double, std::milli>(Clock::now() - asked).count();
            if (shotError) std::wprintf(L"screenshot failed: %s\n", shotError->c_str());
            else {
                // Width and height from the PNG's header chunk, big-endian at bytes 16 and 20.
                unsigned char header[24]{};
                if (FILE* file = nullptr; _wfopen_s(&file, shot.c_str(), L"rb") == 0 && file) { std::fread(header, 1, sizeof(header), file); std::fclose(file); }
                const auto be = [&](int at) { return (header[at] << 24) | (header[at + 1] << 16) | (header[at + 2] << 8) | header[at + 3]; };
                std::error_code ignored;
                std::wprintf(L"\nscreenshot %s\n  %dx%d, %.1f KB in %.0f ms\n", shot.c_str(), be(16), be(20),
                             std::filesystem::file_size(shot, ignored) / 1e3, shotMs);
            }
        }
    }

    const auto path = nextClipPath(options->folder, game);
    bool done = false;
    std::optional<std::wstring> saveError;
    std::filesystem::path savedPath;
    const auto saving = Clock::now();
    if (auto error = recorder.saveClip(path, [&](std::optional<std::wstring> e, const std::filesystem::path& published) {
            std::lock_guard lock(mutex);
            saveError = std::move(e);
            savedPath = published;
            done = true;
            saved.notify_one();
        })) {
        std::wprintf(L"save: %s\n", error->c_str());
        return 1;
    }
    {
        std::unique_lock lock(mutex);
        saved.wait(lock, [&] { return done; });
    }
    const double saveMs = std::chrono::duration<double, std::milli>(Clock::now() - saving).count();
    recorder.stop();
    if (child.hProcess) {
        TerminateProcess(child.hProcess, 0);
        CloseHandle(child.hProcess);
        CloseHandle(child.hThread);
    }
    if (saveError) { std::wprintf(L"save failed: %s\n", saveError->c_str()); return 1; }
    if (savedPath.empty()) { std::wprintf(L"save failed: no published path\n"); return 1; }
    std::error_code error;
    std::wprintf(L"\nsaved %s\n  %.1f MB in %.0f ms\n", savedPath.c_str(), std::filesystem::file_size(savedPath, error) / 1e6, saveMs);
    if (options->verify) {
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
        verify(savedPath, options->video.fps);
        MFShutdown();
    }
    return 0;
}
