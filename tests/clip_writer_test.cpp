#include "capture/annexb.h"
#include "capture/clip_writer.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <winrt/base.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>

using namespace refarter::capture;

namespace {
constexpr std::int64_t second = 10'000'000;
constexpr Size frameSize{256, 144};
constexpr int fps = 30;

int failures = 0;
void check(bool passes, const char* name) {
    if (!passes) { std::cerr << name << '\n'; ++failures; }
}

// One grey keyframe from Windows' software H.264 encoder, so the check needs no GPU. Every frame of the test clip is
// a copy of it: each decodes on its own, and the sink writer stores packets without looking inside them.
EncodedPacket keyframe(std::vector<std::uint8_t>& sequenceHeader) {
    winrt::com_ptr<IMFTransform> mft;
    winrt::check_hresult(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(mft.put())));
    winrt::com_ptr<IMFMediaType> h264;
    winrt::check_hresult(MFCreateMediaType(h264.put()));
    winrt::check_hresult(h264->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    winrt::check_hresult(h264->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
    winrt::check_hresult(MFSetAttributeSize(h264.get(), MF_MT_FRAME_SIZE, frameSize.width, frameSize.height));
    winrt::check_hresult(MFSetAttributeRatio(h264.get(), MF_MT_FRAME_RATE, fps, 1));
    winrt::check_hresult(h264->SetUINT32(MF_MT_AVG_BITRATE, 500'000));
    winrt::check_hresult(h264->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    winrt::check_hresult(mft->SetOutputType(0, h264.get(), 0));
    winrt::com_ptr<IMFMediaType> nv12;
    winrt::check_hresult(MFCreateMediaType(nv12.put()));
    winrt::check_hresult(nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    winrt::check_hresult(nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    winrt::check_hresult(MFSetAttributeSize(nv12.get(), MF_MT_FRAME_SIZE, frameSize.width, frameSize.height));
    winrt::check_hresult(MFSetAttributeRatio(nv12.get(), MF_MT_FRAME_RATE, fps, 1));
    winrt::check_hresult(nv12->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    winrt::check_hresult(mft->SetInputType(0, nv12.get(), 0));
    winrt::check_hresult(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));

    const DWORD bytes = frameSize.width * frameSize.height * 3 / 2;
    winrt::com_ptr<IMFMediaBuffer> buffer;
    winrt::check_hresult(MFCreateMemoryBuffer(bytes, buffer.put()));
    BYTE* data = nullptr;
    winrt::check_hresult(buffer->Lock(&data, nullptr, nullptr));
    std::memset(data, 128, bytes);
    buffer->Unlock();
    winrt::check_hresult(buffer->SetCurrentLength(bytes));
    winrt::com_ptr<IMFSample> frame;
    winrt::check_hresult(MFCreateSample(frame.put()));
    winrt::check_hresult(frame->AddBuffer(buffer.get()));
    winrt::check_hresult(frame->SetSampleTime(0));
    winrt::check_hresult(frame->SetSampleDuration(second / fps));
    winrt::check_hresult(mft->ProcessInput(0, frame.get(), 0));
    winrt::check_hresult(mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0));

    MFT_OUTPUT_STREAM_INFO info{};
    winrt::check_hresult(mft->GetOutputStreamInfo(0, &info));
    EncodedPacket packet;
    packet.keyframe = true;
    packet.duration = second / fps;
    for (;;) {
        winrt::com_ptr<IMFSample> sample;
        winrt::com_ptr<IMFMediaBuffer> out;
        winrt::check_hresult(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, bytes), out.put()));
        winrt::check_hresult(MFCreateSample(sample.put()));
        winrt::check_hresult(sample->AddBuffer(out.get()));
        MFT_OUTPUT_DATA_BUFFER output{0, sample.get(), 0, nullptr};
        DWORD status = 0;
        const HRESULT hr = mft->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) output.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        winrt::check_hresult(hr);
        winrt::com_ptr<IMFMediaBuffer> contiguous;
        winrt::check_hresult(sample->ConvertToContiguousBuffer(contiguous.put()));
        DWORD length = 0;
        winrt::check_hresult(contiguous->Lock(&data, nullptr, &length));
        packet.data.insert(packet.data.end(), data, data + length);
        contiguous->Unlock();
    }
    winrt::com_ptr<IMFMediaType> type;
    winrt::check_hresult(mft->GetOutputCurrentType(0, type.put()));
    UINT32 headerBytes = 0;
    if (SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &headerBytes)) && headerBytes) {
        sequenceHeader.resize(headerBytes);
        winrt::check_hresult(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequenceHeader.data(), headerBytes, nullptr));
    }
    if (auto inBand = annexb::parameterSets(Codec::H264, packet.data); !inBand.empty()) sequenceHeader = std::move(inBand);
    return packet;
}

// A still screen, as Windows Graphics Capture delivers it: frames only while something changes. Here two seconds of
// frames, then `gap` seconds without any, repeated until `length`.
std::vector<PacketPtr> sparseVideo(const EncodedPacket& frame, std::int64_t length, std::int64_t gap) {
    std::vector<PacketPtr> packets;
    for (std::int64_t burst = 0; burst < length; burst += 2 * second + gap) {
        for (std::int64_t time = burst; time < std::min(burst + 2 * second, length); time += second / fps) {
            auto packet = std::make_shared<EncodedPacket>(frame);
            packet->time = time;
            packets.push_back(std::move(packet));
        }
    }
    return packets;
}

std::int64_t clipDuration(const std::filesystem::path& file, DWORD& streams) {
    winrt::com_ptr<IMFSourceReader> reader;
    winrt::check_hresult(MFCreateSourceReaderFromURL(file.c_str(), nullptr, reader.put()));
    streams = 0;
    for (BOOL selected = FALSE; SUCCEEDED(reader->GetStreamSelection(streams, &selected));) ++streams;
    PROPVARIANT duration;
    PropVariantInit(&duration);
    winrt::check_hresult(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration));
    const auto value = static_cast<std::int64_t>(duration.uhVal.QuadPart);
    PropVariantClear(&duration);
    return value;
}
} // namespace

int main() {
    winrt::check_hresult(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    winrt::check_hresult(MFStartup(MF_VERSION));
    const auto folder = std::filesystem::temp_directory_path() / L"refarter-clip-writer-test";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    try {
        std::vector<std::uint8_t> sequenceHeader;
        const auto frame = keyframe(sequenceHeader);
        check(!frame.data.empty() && !sequenceHeader.empty(), "the software encoder made a keyframe");

        // Gaps in the video must not hold up the audio beside them: a throttled sink writer waited for video that
        // was not coming, and a still desktop took minutes to save (2026-10-30). Four tracks as the recorder saves
        // them; 40 s with 8 s gaps.
        constexpr std::int64_t length = 40 * second;
        const auto packets = sparseVideo(frame, length, 8 * second);
        ClipFormat format{frameSize, fps, 500, Codec::H264, sequenceHeader, length};
        format.audioNames = {"Mix", "Desktop", "Mic", "Game"};
        format.audioChannels = {2, 2, 1, 2};
        format.audioFrames = static_cast<std::size_t>(length / second * 48'000);
        std::size_t made = 0;
        format.audio = [&](std::size_t frames, std::vector<std::vector<std::int16_t>>& tracks) {
            tracks.resize(format.audioNames.size());
            for (std::size_t t = 0; t < tracks.size(); ++t) {
                const auto channels = static_cast<std::size_t>(format.audioChannels[t]);
                tracks[t].resize(frames * channels);
                for (std::size_t i = 0; i < frames; ++i)
                    for (std::size_t c = 0; c < channels; ++c)
                        tracks[t][i * channels + c] = static_cast<std::int16_t>(8'000 * std::sin((made + i) * 440.0 * 6.283185 / 48'000));
            }
            made += frames;
        };
        const auto file = folder / L"gaps.mp4";
        const auto started = std::chrono::steady_clock::now();
        const auto error = writeClip(file, packets, format);
        const auto took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "40 s clip with 8 s video gaps saved in " << took << " s\n";
        check(!error, "a clip with gaps in its video saves");
        check(took < 10, "gaps in the video do not slow the save down");
        check(made == format.audioFrames, "every audio frame was asked for");
        if (!error) {
            DWORD streams = 0;
            const auto duration = clipDuration(file, streams);
            check(streams == 5, "the clip holds the video and four audio tracks");
            check(duration >= length - second / 10, "the clip lasts as long as its audio");
        }
    } catch (const winrt::hresult_error& e) {
        std::wcerr << L"Media Foundation failed: " << std::wstring(e.message()) << L'\n';
        ++failures;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    MFShutdown();
    CoUninitialize();
    return failures ? 1 : 0;
}
