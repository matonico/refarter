#include "codec_probe.h"

#include "annexb.h"
#include "clip_writer.h"
#include "frame_converter.h"
#include "gpu.h"
#include "surface_pool.h"
#include "video_encoder.h"
#include "diagnostic_log.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>

namespace refarter::capture {

namespace {
constexpr std::int64_t second = 10'000'000;
constexpr Size probeSize{640, 360};
constexpr int probeFps = 30;

std::wstring describe(const winrt::hresult_error& e) {
    if (e.code() == MF_E_TOPO_CODEC_NOT_FOUND) return L"this gpu has no hardware encoder for it";
    auto text = std::wstring(e.message());
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L'.')) text.pop_back();
    return text.empty() ? L"error " + std::to_wstring(e.code()) : text;
}

struct TenBitState {
    LUID gpu;
    Codec codec;
    std::optional<bool> works;   // Nothing until a check ran (or after an early encoder failure, until the next check).
    int encoderFailures = 0;     // 10-bit encoders that failed before their first packet.
};
std::mutex tenBitMutex;
std::vector<TenBitState> tenBitStates; // At most one per GPU and codec.

TenBitState& stateFor(Codec codec, LUID gpu) { // tenBitMutex held.
    for (auto& s : tenBitStates)
        if (s.codec == codec && sameLuid(s.gpu, gpu)) return s;
    return tenBitStates.emplace_back(TenBitState{gpu, codec});
}

std::optional<bool> tenBitResult(Codec codec, LUID gpu) {
    std::lock_guard lock(tenBitMutex);
    return stateFor(codec, gpu).works;
}

void setTenBitResult(Codec codec, LUID gpu, bool works) {
    std::lock_guard lock(tenBitMutex);
    auto& state = stateFor(codec, gpu);
    state.works = state.works.value_or(true) && works; // A refusal is final; a pass never undoes it.
}

// `timedOut` is set when the encoder gave too few packets within the deadline (a busy GPU, not a 10-bit refusal).
CodecSupport probe(Codec codec, const Gpu& gpu, const std::filesystem::path& file, bool tenBit, HANDLE cancel,
                   bool& timedOut) {
    CodecSupport result{codec};
    FrameConverter converter(gpu.device.get(), gpu.context.get(), probeSize, probeFps, tenBit);
    SurfacePool surfaces(converter, 4);
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<PacketPtr> packets;
    HRESULT failure = S_OK;
    // Declared after what its callbacks use, so it is destroyed first.
    VideoEncoder encoder(gpu.device.get(), gpu.luid,
        EncoderConfig{probeSize, probeFps, codec, Bitrate{2'000, 4'000, false}, tenBit},
        [&](PacketPtr packet) {
            std::lock_guard lock(mutex);
            packets.push_back(std::move(packet));
            changed.notify_one();
        },
        [&](HRESULT hr) {
            std::lock_guard lock(mutex);
            failure = hr;
            changed.notify_one();
        });
    result.encoder = encoder.name();

    // A little over one keyframe interval: proves keyframes arrive every second,
    // which the replay buffer needs to drop old video and start clips cleanly.
    constexpr std::size_t wanted = probeFps + 4;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::vector<PacketPtr> encoded;
    for (int frame = 0;;) {
        if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        if (auto surface = surfaces.acquire()) {
            surface->sample->SetSampleTime(frame * second / probeFps);
            surface->sample->SetSampleDuration(second / probeFps);
            encoder.submit(std::move(surface->sample));
            ++frame;
        }
        std::unique_lock lock(mutex);
        changed.wait_for(lock, std::chrono::milliseconds(5));
        if (packets.size() >= wanted || FAILED(failure) || std::chrono::steady_clock::now() > deadline) {
            timedOut = packets.size() < wanted && SUCCEEDED(failure);
            encoded = packets;
            break;
        }
    }
    if (FAILED(failure)) winrt::throw_hresult(failure);
    if (encoded.empty() || !encoded.front()->keyframe) {
        result.problem = L"the encoder produced no keyframe";
        return result;
    }
    if (encoded.size() < wanted || std::count_if(encoded.begin(), encoded.end(), [](const auto& p) { return p->keyframe; }) < 2) {
        result.problem = L"the encoder does not keep a one-second keyframe interval";
        return result;
    }

    ClipFormat format{probeSize, probeFps, 2'000, codec, encoder.sequenceHeader(),
                      encoded.back()->time + encoded.back()->duration};
    if (auto inBand = annexb::parameterSets(codec, encoded.front()->data); !inBand.empty()) format.sequenceHeader = std::move(inBand);
    format.tenBit = tenBit;
    if (auto error = writeClip(file, encoded, format)) {
        result.problem = L"windows could not write it as mp4";
        return result;
    }

    winrt::com_ptr<IMFSourceReader> reader;
    constexpr auto video = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    winrt::com_ptr<IMFMediaType> native;
    GUID subtype{};
    if (FAILED(MFCreateSourceReaderFromURL(file.c_str(), nullptr, reader.put())) ||
        FAILED(reader->GetNativeMediaType(video, 0, native.put())) ||
        FAILED(native->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != mediaSubtype(codec)) {
        result.problem = L"windows could not read the mp4 back";
        return result;
    }
    result.records = true;

    // Decoding one frame shows whether this PC can play such clips. A 10-bit decoder may only give P010.
    const auto decodesTo = [&](const GUID& format) {
        winrt::com_ptr<IMFMediaType> type;
        winrt::check_hresult(MFCreateMediaType(type.put()));
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, format);
        return SUCCEEDED(reader->SetCurrentMediaType(video, nullptr, type.get()));
    };
    if (decodesTo(MFVideoFormat_NV12) || (tenBit && decodesTo(MFVideoFormat_P010))) {
        for (int attempt = 0; attempt < 8 && !result.plays; ++attempt) {
            DWORD flags = 0;
            winrt::com_ptr<IMFSample> sample;
            if (FAILED(reader->ReadSample(video, 0, nullptr, &flags, nullptr, sample.put())) ||
                (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) break;
            result.plays = sample != nullptr;
        }
    }
    return result;
}

// One check run. `error` is why it did not record: the thrown HRESULT, ERROR_TIMEOUT when the encoder was too slow,
// or E_FAIL when the encoder or writer gave an unusable clip without one.
struct Attempt { CodecSupport support; HRESULT error = S_OK; };

Attempt attempt(Codec codec, const Gpu& gpu, const std::filesystem::path& file, bool tenBit, HANDLE cancel) {
    Attempt result{CodecSupport{codec}};
    std::error_code ignored;
    std::filesystem::remove(file, ignored); // The writer never replaces a file: clear one a crashed run left.
    bool timedOut = false;
    try { result.support = probe(codec, gpu, file, tenBit, cancel, timedOut); }
    catch (const winrt::hresult_error& e) { result.error = e.code(); result.support.problem = describe(e); }
    catch (const std::bad_alloc&) { result.error = E_OUTOFMEMORY; result.support.problem = L"the check ran out of memory"; }
    catch (const std::exception&) { result.error = E_FAIL; result.support.problem = L"the check failed"; }
    if (!result.support.records && SUCCEEDED(result.error)) result.error = timedOut ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : E_FAIL;
    std::filesystem::remove(file, ignored); // The next check needs it gone.
    return result;
}

std::optional<std::filesystem::path> tempFolder() {
    wchar_t temp[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH + 1, temp);
    if (length == 0 || length > MAX_PATH) return std::nullopt;
    return std::filesystem::path(temp);
}
} // namespace

void refuseTenBit(Codec codec, LUID gpu) { setTenBitResult(codec, gpu, false); }
bool tenBitRefused(Codec codec, LUID gpu) { return tenBitResult(codec, gpu) == false; }

bool tenBitEncoderFailed(Codec codec, LUID gpu) {
    std::lock_guard lock(tenBitMutex);
    auto& state = stateFor(codec, gpu);
    if (state.works == false) return true;
    if (++state.encoderFailures >= 2) {
        state.works = false;
        return true;
    }
    state.works.reset(); // The next 10-bit start runs the write check again.
    return false;
}

bool notTenBitsFault(HRESULT error, ID3D11Device* device) {
    return error == E_OUTOFMEMORY || error == DXGI_ERROR_DEVICE_REMOVED || error == DXGI_ERROR_DEVICE_RESET ||
           error == HRESULT_FROM_WIN32(ERROR_CANCELLED) || error == HRESULT_FROM_WIN32(ERROR_TIMEOUT) ||
           error == MF_E_HW_MFT_FAILED_START_STREAMING || FAILED(device->GetDeviceRemovedReason());
}

TenBitWriteCheck checkTenBitWrites(Codec codec, const Gpu& gpu, HANDLE cancel) {
    if (const auto known = tenBitResult(codec, gpu.luid)) return {*known, false, *known ? S_OK : E_FAIL};
    const auto temp = tempFolder();
    if (!temp) return {false, false, E_FAIL}; // Not 10-bit's fault: this start uses 8-bit, the next checks again.
    const auto file = *temp / (L"refarter-tenbit-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(static_cast<int>(codec)) + L".mp4");
    const auto result = attempt(codec, gpu, file, true, cancel);
    if (result.support.records) {
        setTenBitResult(codec, gpu.luid, true);
        return {true, false, S_OK};
    }
    return {false, !notTenBitsFault(result.error, gpu.device.get()), result.error};
}

std::vector<CodecSupport> probeCodecs(HMONITOR monitor) {
    std::vector<CodecSupport> results;
    const Codec codecs[] = {Codec::H264, Codec::HEVC, Codec::AV1};
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        for (auto codec : codecs) results.push_back({codec, false, false, {}, L"media foundation is unavailable"});
        return results;
    }
    std::optional<Gpu> gpu;
    std::wstring gpuProblem;
    try { gpu = createGpuForMonitor(monitor); }
    catch (const winrt::hresult_error& e) { gpuProblem = L"no usable gpu: " + describe(e); }
    const auto temp = tempFolder();
    for (auto codec : codecs) {
        CodecSupport result{codec};
        const auto file = temp.value_or(std::filesystem::path{}) /
            (L"refarter-probe-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(static_cast<int>(codec)) + L".mp4");
        if (!gpu) result.problem = gpuProblem;
        else if (!temp) result.problem = L"windows has no usable temporary folder";
        else {
            // As recording does: 10-bit first where the codec prefers it. If that fails anywhere (opening, encoding,
            // writing) but 8-bit records, later starts and checks use 8-bit, unless the device was lost or memory ran out.
            const bool tenBit = prefersTenBit(codec) && !tenBitRefused(codec, gpu->luid);
            auto checked = attempt(codec, *gpu, file, tenBit, nullptr);
            if (tenBit && checked.support.records) setTenBitResult(codec, gpu->luid, true); // The first start need not check again.
            if (tenBit && !checked.support.records) {
                auto eightBit = attempt(codec, *gpu, file, false, nullptr);
                if (eightBit.support.records && !notTenBitsFault(checked.error, gpu->device.get())) {
                    refuseTenBit(codec, gpu->luid);
                    diagnosticLog().write(DiagnosticEvent::TenBitFallback, checked.error, static_cast<unsigned>(codec), 0);
                }
                checked = std::move(eightBit); // When both fail, 8-bit's problem is the one that matters.
            }
            result = std::move(checked.support);
        }
        results.push_back(std::move(result));
    }
    gpu.reset();
    MFShutdown();
    return results;
}

} // namespace refarter::capture
