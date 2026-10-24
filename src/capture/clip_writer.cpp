#include "clip_writer.h"
#include "clip_folder.h"
#include "clip_timing.h"
#include "audio_track.h"
#include "video_encoder.h"
#include "save_errors.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <winrt/base.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <string_view>

namespace refarter::capture {

namespace {
constexpr std::int64_t second = 10'000'000;
constexpr std::size_t audioChunkFrames = AudioTrack::rate / 10; // 100 ms per sample.
constexpr UINT32 audioBytesPerSecond = 24'000;               // 192 kb/s AAC, as OBS uses.

// Presents a buffered packet to Media Foundation without copying it.
struct PacketBuffer : winrt::implements<PacketBuffer, IMFMediaBuffer> {
    PacketPtr packet;
    explicit PacketBuffer(PacketPtr p) : packet(std::move(p)) {}

    STDMETHODIMP Lock(BYTE** data, DWORD* maxLength, DWORD* currentLength) noexcept override {
        *data = const_cast<BYTE*>(packet->data.data()); // The MP4 sink only reads.
        if (maxLength) *maxLength = static_cast<DWORD>(packet->data.size());
        if (currentLength) *currentLength = static_cast<DWORD>(packet->data.size());
        return S_OK;
    }
    STDMETHODIMP Unlock() noexcept override { return S_OK; }
    STDMETHODIMP GetCurrentLength(DWORD* length) noexcept override {
        *length = static_cast<DWORD>(packet->data.size());
        return S_OK;
    }
    STDMETHODIMP SetCurrentLength(DWORD length) noexcept override {
        return length == packet->data.size() ? S_OK : E_INVALIDARG;
    }
    STDMETHODIMP GetMaxLength(DWORD* length) noexcept override {
        *length = static_cast<DWORD>(packet->data.size());
        return S_OK;
    }
};

std::uint32_t be32(const std::uint8_t* p) { return (std::uint32_t{p[0]} << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// Children of a box payload as (offset, size, type), offsets relative to `data`.
template <typename Visit>
void forEachBox(const std::vector<std::uint8_t>& data, std::size_t begin, std::size_t end, Visit&& visit) {
    while (begin + 8 <= end) {
        const std::size_t size = be32(&data[begin]);
        if (size < 8 || begin + size > end) return;
        visit(begin, size, std::string_view(reinterpret_cast<const char*>(&data[begin + 4]), 4));
        begin += size;
    }
}

// Media Foundation marks every audio track enabled with no alternate group, so
// some players play all of them at once. Put audio tracks in one alternate
// group and enable only the first (the mix), as ffmpeg does. Each track's
// name replaces Media Foundation's "SoundHandler" handler name, which has room
// for 12 characters. Patches the index in place; the file size does not change.
void markAudioTracks(const std::filesystem::path& path, const std::vector<std::string>& names, bool alternates) {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    std::uint64_t offset = 0;
    std::uint8_t header[16];
    while (file.seekg(static_cast<std::streamoff>(offset)).read(reinterpret_cast<char*>(header), 8)) {
        std::uint64_t size = be32(header);
        if (size == 1 && file.read(reinterpret_cast<char*>(header + 8), 8)) {
            size = (std::uint64_t{be32(header + 8)} << 32) | be32(header + 12);
        }
        if (size < 8) return;
        if (std::string_view(reinterpret_cast<const char*>(header + 4), 4) == "moov") {
            std::vector<std::uint8_t> moov(static_cast<std::size_t>(size));
            if (!file.seekg(static_cast<std::streamoff>(offset)).read(reinterpret_cast<char*>(moov.data()), size)) return;
            std::size_t index = 0; // Of the audio track, in writing order.
            forEachBox(moov, 8, moov.size(), [&](std::size_t trak, std::size_t trakSize, std::string_view type) {
                if (type != "trak") return;
                std::size_t tkhd = 0, hdlr = 0, hdlrSize = 0;
                forEachBox(moov, trak + 8, trak + trakSize, [&](std::size_t at, std::size_t boxSize, std::string_view child) {
                    if (child == "tkhd") tkhd = at;
                    if (child != "mdia") return;
                    forEachBox(moov, at + 8, at + boxSize, [&](std::size_t h, std::size_t s, std::string_view grandchild) {
                        if (grandchild == "hdlr" && s >= 32 && std::string_view(reinterpret_cast<const char*>(&moov[h + 16]), 4) == "soun") {
                            hdlr = h;
                            hdlrSize = s;
                        }
                    });
                });
                if (!hdlr || !tkhd) return;
                const auto& name = index < names.size() ? names[index] : std::string{};
                if (!name.empty() && name.size() < hdlrSize - 32) { // Keeps the terminating zero.
                    std::fill(moov.begin() + static_cast<std::ptrdiff_t>(hdlr + 32), moov.begin() + static_cast<std::ptrdiff_t>(hdlr + hdlrSize), std::uint8_t{0});
                    std::copy(name.begin(), name.end(), moov.begin() + static_cast<std::ptrdiff_t>(hdlr + 32));
                }
                if (alternates) {
                    const std::size_t group = tkhd + 8 + (moov[tkhd + 8] == 1 ? 46 : 34); // alternate_group
                    moov[group] = 0;
                    moov[group + 1] = 1;
                    if (index == 0) moov[tkhd + 11] |= 0x01; else moov[tkhd + 11] &= ~0x01; // track_enabled
                }
                ++index;
            });
            file.seekp(static_cast<std::streamoff>(offset)).write(reinterpret_cast<const char*>(moov.data()), size);
            return;
        }
        offset += size;
    }
}

// `stage` names the step in progress, so a failed save says where it stopped.
void write(const std::filesystem::path& path, const std::vector<PacketPtr>& packets, const ClipFormat& format,
           std::wstring& stage) {
    stage = L"opening the file";
    winrt::com_ptr<IMFAttributes> attributes;
    winrt::check_hresult(MFCreateAttributes(attributes.put(), 2));
    winrt::check_hresult(attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));
    winrt::check_hresult(attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
    winrt::com_ptr<IMFSinkWriter> writer;
    winrt::check_hresult(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attributes.get(), writer.put()));

    stage = L"setting up the video track";
    winrt::com_ptr<IMFMediaType> type;
    winrt::check_hresult(MFCreateMediaType(type.put()));
    winrt::check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    winrt::check_hresult(type->SetGUID(MF_MT_SUBTYPE, mediaSubtype(format.codec)));
    winrt::check_hresult(MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, format.size.width, format.size.height));
    winrt::check_hresult(MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE, format.fps, 1));
    winrt::check_hresult(MFSetAttributeRatio(type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    winrt::check_hresult(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    winrt::check_hresult(type->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(format.averageKbps) * 1000u));
    winrt::check_hresult(type->SetUINT32(MF_MT_MPEG2_PROFILE, mediaProfile(format.codec)));
    if (!format.sequenceHeader.empty()) {
        winrt::check_hresult(type->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, format.sequenceHeader.data(),
                                           static_cast<UINT32>(format.sequenceHeader.size())));
    }
    DWORD stream = 0;
    winrt::check_hresult(writer->AddStream(type.get(), &stream));
    winrt::check_hresult(writer->SetInputMediaType(stream, type.get(), nullptr)); // Same type: pass-through.

    // Audio: PCM in, AAC out; the sink writer inserts Windows' AAC encoder.
    std::vector<DWORD> audioStreams;
    for (std::size_t i = 0; i < format.audio.size(); ++i) {
        stage = L"setting up audio track " + std::to_wstring(i + 1);
        winrt::com_ptr<IMFMediaType> aac;
        winrt::check_hresult(MFCreateMediaType(aac.put()));
        winrt::check_hresult(aac->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
        winrt::check_hresult(aac->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
        winrt::check_hresult(aac->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AudioTrack::rate));
        winrt::check_hresult(aac->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AudioTrack::channels));
        winrt::check_hresult(aac->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
        winrt::check_hresult(aac->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, audioBytesPerSecond));
        winrt::com_ptr<IMFMediaType> pcm;
        winrt::check_hresult(MFCreateMediaType(pcm.put()));
        winrt::check_hresult(pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
        winrt::check_hresult(pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
        winrt::check_hresult(pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AudioTrack::rate));
        winrt::check_hresult(pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AudioTrack::channels));
        winrt::check_hresult(pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
        winrt::check_hresult(pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, AudioTrack::channels * 2));
        winrt::check_hresult(pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AudioTrack::rate * AudioTrack::channels * 2));
        DWORD index = 0;
        winrt::check_hresult(writer->AddStream(aac.get(), &index));
        winrt::check_hresult(writer->SetInputMediaType(index, pcm.get(), nullptr));
        audioStreams.push_back(index);
    }
    stage = L"starting to write";
    winrt::check_hresult(writer->BeginWriting());

    // Interleave audio with video by time so the sink never buffers a whole track.
    std::vector<std::size_t> audioDone(format.audio.size(), 0); // Frames written per track.
    const auto writeAudioUntil = [&](std::int64_t until) {
        for (std::size_t t = 0; t < format.audio.size(); ++t) {
            const auto& pcm = format.audio[t];
            const std::size_t total = pcm.size() / AudioTrack::channels;
            while (audioDone[t] < total && static_cast<std::int64_t>(audioDone[t]) * second / AudioTrack::rate < until) {
                stage = L"writing audio track " + std::to_wstring(t + 1);
                const std::size_t frames = std::min(audioChunkFrames, total - audioDone[t]);
                const DWORD bytes = static_cast<DWORD>(frames * AudioTrack::channels * sizeof(std::int16_t));
                winrt::com_ptr<IMFMediaBuffer> buffer;
                winrt::check_hresult(MFCreateMemoryBuffer(bytes, buffer.put()));
                BYTE* data = nullptr;
                winrt::check_hresult(buffer->Lock(&data, nullptr, nullptr));
                std::memcpy(data, pcm.data() + audioDone[t] * AudioTrack::channels, bytes);
                buffer->Unlock();
                winrt::check_hresult(buffer->SetCurrentLength(bytes));
                winrt::com_ptr<IMFSample> sample;
                winrt::check_hresult(MFCreateSample(sample.put()));
                winrt::check_hresult(sample->AddBuffer(buffer.get()));
                winrt::check_hresult(sample->SetSampleTime(static_cast<LONGLONG>(audioDone[t]) * second / AudioTrack::rate));
                winrt::check_hresult(sample->SetSampleDuration(static_cast<LONGLONG>(frames) * second / AudioTrack::rate));
                winrt::check_hresult(writer->WriteSample(audioStreams[t], sample.get()));
                audioDone[t] += frames;
            }
        }
    };

    const auto start = packets.front()->time;
    for (std::size_t i = 0; i < packets.size(); ++i) {
        const auto& packet = packets[i];
        const auto* next = i + 1 < packets.size() ? packets[i + 1].get() : nullptr;
        writeAudioUntil(packet->time - start + second);
        stage = L"writing video frame " + std::to_wstring(i + 1) + L" of " + std::to_wstring(packets.size());
        winrt::com_ptr<IMFSample> sample;
        winrt::check_hresult(MFCreateSample(sample.put()));
        auto buffer = winrt::make<PacketBuffer>(packet);
        winrt::check_hresult(sample->AddBuffer(buffer.as<IMFMediaBuffer>().get()));
        winrt::check_hresult(sample->SetSampleTime(packet->time - start));
        const auto duration = clipPacketDuration(*packet, next, format.end, format.preserveCaptureTime);
        if (duration > 0) winrt::check_hresult(sample->SetSampleDuration(duration));
        if (packet->keyframe) winrt::check_hresult(sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE));
        winrt::check_hresult(writer->WriteSample(stream, sample.get()));
    }
    writeAudioUntil(std::numeric_limits<std::int64_t>::max());
    stage = L"finishing the file";
    winrt::check_hresult(writer->Finalize());
}

// Free bytes on the volume that will hold `path`, or the maximum when that cannot be read.
unsigned long long freeBytesFor(const std::filesystem::path& path) {
    ULARGE_INTEGER available{};
    const auto folder = path.has_parent_path() ? path.parent_path() : std::filesystem::path(L".");
    return GetDiskFreeSpaceExW(folder.c_str(), &available, nullptr, nullptr) ? available.QuadPart : ~0ull;
}
} // namespace

std::optional<std::wstring> writeClip(const std::filesystem::path& path,
                                      const std::vector<PacketPtr>& packets, const ClipFormat& format,
                                      std::filesystem::path* placed) {
    if (placed) *placed = path;
    if (packets.empty() || !packets.front()->keyframe) return L"nothing to save yet";
    auto partial = path;
    partial += L".partial";
    std::wstring stage = L"creating the folder";
    try {
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        write(partial, increasingTimes(packets), format, stage);
        stage = L"marking the audio tracks";
        if (!format.audio.empty()) markAudioTracks(partial, format.audioNames, format.audio.size() > 1);
        stage = L"renaming the finished clip";
        // Never replace a file: one that appeared under this name since it was chosen is somebody's clip. The next
        // free name is used instead (a few tries: another file can take that one too).
        auto target = path;
        for (int attempt = 0;; ++attempt) {
            if (MoveFileExW(partial.c_str(), target.c_str(), 0)) break;
            const auto code = GetLastError();
            if ((code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS) || attempt >= 8)
                winrt::throw_hresult(HRESULT_FROM_WIN32(code));
            target = nextFreePath(target);
            if (target.empty()) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
        }
        if (placed) *placed = target;
        return std::nullopt;
    } catch (const winrt::hresult_error& e) {
        // Measured before the partial file goes: removing it frees the space that ran out.
        const bool diskFull = failedForDiskSpace(static_cast<int32_t>(e.code()), freeBytesFor(path));
        DeleteFileW(partial.c_str());
        if (diskFull) return diskFullMessage();
        return L"could not write the clip while " + stage + L": " + std::wstring(e.message());
    } catch (const std::filesystem::filesystem_error& e) {
        const bool diskFull = isDiskFull(e.code()) || freeBytesFor(path) < lowDiskBytes;
        DeleteFileW(partial.c_str());
        if (diskFull) return diskFullMessage();
        return L"could not write the clip while " + stage;
    } catch (const std::exception&) {
        const bool diskFull = freeBytesFor(path) < lowDiskBytes;
        DeleteFileW(partial.c_str());
        if (diskFull) return diskFullMessage();
        return L"could not write the clip while " + stage;
    }
}

} // namespace refarter::capture
