#include "codec_probe.h"

#include "annexb.h"
#include "clip_writer.h"
#include "frame_converter.h"
#include "gpu.h"
#include "surface_pool.h"
#include "video_encoder.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <algorithm>
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

CodecSupport probe(Codec codec, const Gpu& gpu, const std::filesystem::path& file) {
    CodecSupport result{codec};
    FrameConverter converter(gpu.device.get(), gpu.context.get(), probeSize, probeFps);
    SurfacePool surfaces(converter, 4);
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<PacketPtr> packets;
    HRESULT failure = S_OK;
    // Declared after what its callbacks use, so it is destroyed first.
    VideoEncoder encoder(gpu.device.get(), gpu.luid,
        EncoderConfig{probeSize, probeFps, codec, Bitrate{2'000, 4'000, false}},
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
        if (auto surface = surfaces.acquire()) {
            surface->sample->SetSampleTime(frame * second / probeFps);
            surface->sample->SetSampleDuration(second / probeFps);
            encoder.submit(std::move(surface->sample));
            ++frame;
        }
        std::unique_lock lock(mutex);
        changed.wait_for(lock, std::chrono::milliseconds(5));
        if (packets.size() >= wanted || FAILED(failure) || std::chrono::steady_clock::now() > deadline) {
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

    // Decoding one frame shows whether this PC can play such clips.
    winrt::com_ptr<IMFMediaType> nv12;
    winrt::check_hresult(MFCreateMediaType(nv12.put()));
    nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (SUCCEEDED(reader->SetCurrentMediaType(video, nullptr, nv12.get()))) {
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
} // namespace

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
    wchar_t temp[MAX_PATH + 1]{};
    GetTempPathW(MAX_PATH + 1, temp);
    for (auto codec : codecs) {
        CodecSupport result{codec};
        const auto file = std::filesystem::path(temp) /
            (L"refarter-probe-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(static_cast<int>(codec)) + L".mp4");
        if (!gpu) result.problem = gpuProblem;
        else {
            try { result = probe(codec, *gpu, file); }
            catch (const winrt::hresult_error& e) { result.problem = describe(e); }
            catch (const std::exception&) { result.problem = L"the check ran out of memory"; }
        }
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        results.push_back(std::move(result));
    }
    gpu.reset();
    MFShutdown();
    return results;
}

} // namespace refarter::capture
