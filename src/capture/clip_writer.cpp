#include "clip_writer.h"
#include "clip_folder.h"
#include "clip_timing.h"
#include "audio_track.h"
#include "video_encoder.h"
#include "save_errors.h"
#include "mp4_tracks.h"
#include "track_workers.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <winrt/base.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>

namespace refarter::capture {

namespace {
constexpr std::int64_t second = 10'000'000;
constexpr std::size_t audioChunkFrames = AudioTrack::rate / 10; // 100 ms per sample.
constexpr UINT32 aacBytesPerChannel = 12'000; // A second: 192 kb/s stereo AAC, as OBS uses; 96 kb/s mono.

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

// Windows' AAC encoder, driven on the calling thread: the save thread, at below-normal priority. Left to the sink
// writer it ran on Media Foundation's own threads at normal priority (about 0.5 s of CPU per track for a 180 s clip,
// measured), competing with the game. The sink writer then only stores the finished AAC, as it stores the video.
class AacEncoder {
public:
    explicit AacEncoder(UINT32 channels) {
        winrt::check_hresult(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(mft_.put())));
        winrt::com_ptr<IMFMediaType> pcm;
        winrt::check_hresult(MFCreateMediaType(pcm.put()));
        winrt::check_hresult(pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
        winrt::check_hresult(pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
        winrt::check_hresult(pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AudioTrack::rate));
        winrt::check_hresult(pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));
        winrt::check_hresult(pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
        winrt::check_hresult(pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2));
        winrt::check_hresult(pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AudioTrack::rate * channels * 2));
        winrt::com_ptr<IMFMediaType> aac;
        winrt::check_hresult(MFCreateMediaType(aac.put()));
        winrt::check_hresult(aac->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
        winrt::check_hresult(aac->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
        winrt::check_hresult(aac->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AudioTrack::rate));
        winrt::check_hresult(aac->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));
        winrt::check_hresult(aac->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
        winrt::check_hresult(aac->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, aacBytesPerChannel * channels));
        winrt::check_hresult(aac->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0)); // Raw AAC, as MP4 stores it.
        // The encoder's documentation sets the output type first; the other order is tried if it refuses.
        if (FAILED(mft_->SetOutputType(0, aac.get(), 0))) {
            winrt::check_hresult(mft_->SetInputType(0, pcm.get(), 0));
            winrt::check_hresult(mft_->SetOutputType(0, aac.get(), 0));
        } else {
            winrt::check_hresult(mft_->SetInputType(0, pcm.get(), 0));
        }
        // The type as the encoder completed it: MF_MT_USER_DATA carries the decoder configuration the MP4 needs.
        winrt::check_hresult(mft_->GetOutputCurrentType(0, type_.put()));
        MFT_OUTPUT_STREAM_INFO info{};
        winrt::check_hresult(mft_->GetOutputStreamInfo(0, &info));
        outputBytes_ = std::max<DWORD>(info.cbSize, 8'192);
        providesSamples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        winrt::check_hresult(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
        winrt::check_hresult(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    }

    IMFMediaType* type() const { return type_.get(); }

    // One piece of PCM in; `write` receives each finished AAC sample, in order.
    template <typename Write> void encode(IMFSample* pcm, const Write& write) {
        for (;;) {
            const HRESULT hr = mft_->ProcessInput(0, pcm, 0);
            if (hr != MF_E_NOTACCEPTING) { winrt::check_hresult(hr); break; }
            if (take(write).count == 0) winrt::throw_hresult(hr); // Full, yet nothing to take: it would never accept more.
        }
        take(write);
    }
    // The sound still inside the encoder, after the last piece. The drain is over when the encoder asks for more input;
    // an empty answer in between is tolerated a few times in a row, so the tail is kept but the save cannot spin.
    template <typename Write> void finish(const Write& write) {
        winrt::check_hresult(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0));
        winrt::check_hresult(mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0));
        constexpr int emptyAnswers = 8;
        for (int empty = 0; empty < emptyAnswers;) {
            const Taken taken = take(write);
            if (taken.needInput) return;
            empty = taken.count > 0 ? 1 : empty + 1; // The answer that ended the call was empty too.
        }
    }

private:
    // What a take() found: how many samples it wrote, and whether it ended because the encoder wants more input (as
    // opposed to an empty answer, which says nothing about whether more is coming).
    struct Taken {
        std::size_t count = 0;
        bool needInput = false;
    };

    // Writes every sample the encoder has ready.
    template <typename Write> Taken take(const Write& write) {
        for (std::size_t taken = 0;;) {
            winrt::com_ptr<IMFSample> sample;
            if (!providesSamples_) {
                winrt::com_ptr<IMFMediaBuffer> buffer;
                winrt::check_hresult(MFCreateMemoryBuffer(outputBytes_, buffer.put()));
                winrt::check_hresult(MFCreateSample(sample.put()));
                winrt::check_hresult(sample->AddBuffer(buffer.get()));
            }
            MFT_OUTPUT_DATA_BUFFER output{0, sample.get(), 0, nullptr};
            DWORD status = 0;
            const HRESULT hr = mft_->ProcessOutput(0, 1, &output, &status);
            if (output.pEvents) output.pEvents->Release();
            if (providesSamples_ && output.pSample) sample.attach(output.pSample); // Ours to release.
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return {taken, true};
            winrt::check_hresult(hr);
            DWORD length = 0;
            // S_OK without a sample, or with an empty one, means nothing more for now: asking again would get the same
            // answer for ever and the save (which stop() waits for) would never end.
            if (!sample || FAILED(sample->GetTotalLength(&length)) || length == 0) return {taken, false};
            // An AAC frame is 1024 samples; times the encoder leaves out follow from the frames made so far.
            LONGLONG time = 0;
            if (FAILED(sample->GetSampleTime(&time))) {
                winrt::check_hresult(sample->SetSampleTime(static_cast<LONGLONG>(made_) * aacFrame * second / AudioTrack::rate));
                winrt::check_hresult(sample->SetSampleDuration(static_cast<LONGLONG>(aacFrame) * second / AudioTrack::rate));
            }
            ++made_;
            ++taken;
            write(sample.get());
        }
    }

    static constexpr int aacFrame = 1024;
    winrt::com_ptr<IMFTransform> mft_;
    winrt::com_ptr<IMFMediaType> type_;
    DWORD outputBytes_ = 0;
    bool providesSamples_ = false;
    std::uint64_t made_ = 0;
};

// The thread's COM apartment while a clip is written (the AAC encoder is a COM object): the process's
// multi-threaded one, unless the thread already has one.
struct ComApartment {
    HRESULT joined = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComApartment() { if (SUCCEEDED(joined)) CoUninitialize(); }
};

// The clip's audio, AAC from one encoder per track. Each encoder runs on a below-normal thread of its own, a second
// at a time, while the save thread makes the next second (reading, filtering, mixing): every encoder gets the same
// pieces in the same order as one after another, so the file is the same, but a save takes about as long as its
// slowest step instead of all of them added up. Only the save thread calls the sink writer and `format.audio`.
class AudioWriter {
    struct Track {
        explicit Track(UINT32 count) : channels(count) {}
        UINT32 channels;
        std::optional<AacEncoder> encoder; // Made on this track's worker (see the constructor), used only there.
        DWORD stream = 0;
        std::array<std::vector<std::int16_t>, 2> pcm;  // One second being encoded, the next being made.
        std::vector<winrt::com_ptr<IMFSample>> aac;    // Encoded, to be written by the save thread.
    };
    // Collects a track's finished AAC on its worker.
    struct Keep {
        Track& track;
        void operator()(IMFSample* aac) const { track.aac.emplace_back().copy_from(aac); }
    };

public:
    AudioWriter(IMFSinkWriter* writer, const ClipFormat& format, std::wstring& stage)
        : writer_(writer), format_(format), stage_(stage) {
        const std::size_t tracks = format.audio ? format.audioNames.size() : 0;
        tracks_.reserve(tracks);
        for (std::size_t t = 0; t < tracks; ++t)
            tracks_.emplace_back(t < format.audioChannels.size() ? static_cast<UINT32>(format.audioChannels[t]) : AudioTrack::channels);
        if (tracks_.empty()) return;
        // Disk work must never compete with the game for CPU time, the encoders included.
        workers_.emplace(tracks_.size(), [] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            CoInitializeEx(nullptr, COINIT_MULTITHREADED); // A new thread: always joins.
        }, [] { CoUninitialize(); });
        // Each encoder is made on the worker that uses it, in the multi-threaded apartment its `enter` joined: the save
        // thread's own apartment may be a single-threaded one (ComApartment), whose objects must not be used from here.
        stage = L"setting up the audio encoders";
        try {
            workers_->start([this](std::size_t t) { tracks_[t].encoder.emplace(tracks_[t].channels); });
            workers_->wait();
            for (std::size_t t = 0; t < tracks_.size(); ++t) {
                stage = L"setting up audio track " + std::to_wstring(t + 1);
                auto& track = tracks_[t];
                winrt::check_hresult(writer->AddStream(track.encoder->type(), &track.stream));
                winrt::check_hresult(writer->SetInputMediaType(track.stream, track.encoder->type(), nullptr)); // Pass-through.
            }
        } catch (...) {
            releaseEncoders(); // The destructor does not run for a constructor that throws.
            throw;
        }
    }

    ~AudioWriter() { releaseEncoders(); }
    AudioWriter(const AudioWriter&) = delete;
    AudioWriter& operator=(const AudioWriter&) = delete;

    // Writes the audio that plays before `until` (clip time, 100 ns units).
    void writeUntil(std::int64_t until) {
        while (!tracks_.empty() && written_ < format_.audioFrames && static_cast<std::int64_t>(written_) * second / AudioTrack::rate < until) {
            if (made_ == 0) make(0); // The first second.
            const int slot = next_;
            workers_->start([this, slot](std::size_t t) { encode(tracks_[t], slot); });
            const bool more = made_ < format_.audioFrames;
            if (more) make(1 - slot); // While the encoders work.
            stage_ = L"encoding audio";
            workers_->wait();
            writeEncoded();
            written_ += frames_[slot];
            next_ = 1 - slot;
        }
    }

    // Everything left, then the sound still inside the encoders.
    void finish() {
        writeUntil(std::numeric_limits<std::int64_t>::max());
        if (tracks_.empty()) return;
        stage_ = L"encoding audio";
        workers_->start([this](std::size_t t) { tracks_[t].encoder->finish(Keep{tracks_[t]}); });
        workers_->wait();
        writeEncoded();
        releaseEncoders();
    }

private:
    // Destroys every encoder on the worker that made it, before the workers end: the workers leave their COM apartment
    // when they stop, and a COM object released after that, on the save thread, is released outside the apartment it
    // lives in. Safe to call more than once, and never throws (it runs from the destructor and from error paths).
    void releaseEncoders() noexcept {
        if (!workers_) return;
        try {
            workers_->wait(); // A round an exception left running must end first (start() is only for an idle pool).
        } catch (...) {
        }
        try {
            workers_->start([this](std::size_t t) { tracks_[t].encoder.reset(); });
            workers_->wait();
        } catch (...) {
        }
    }

    // The next second of every track into `slot`, a 100 ms piece at a time (the pieces a save has always made).
    void make(int slot) {
        stage_ = L"preparing audio";
        const std::size_t frames = std::min(audioBatchFrames, format_.audioFrames - made_);
        for (auto& track : tracks_) track.pcm[slot].clear();
        for (std::size_t done = 0; done < frames; done += audioChunkFrames) {
            format_.audio(std::min(audioChunkFrames, frames - done), pieces_);
            for (std::size_t t = 0; t < tracks_.size(); ++t) tracks_[t].pcm[slot].insert(tracks_[t].pcm[slot].end(), pieces_[t].begin(), pieces_[t].end());
        }
        first_[slot] = made_;
        frames_[slot] = frames;
        made_ += frames;
    }

    // On the track's worker: one second into its encoder as 100 ms samples, as the sink writer was given them before.
    void encode(Track& track, int slot) const {
        const auto& pcm = track.pcm[slot];
        for (std::size_t done = 0; done < frames_[slot]; done += audioChunkFrames) {
            const std::size_t frames = std::min(audioChunkFrames, frames_[slot] - done);
            const DWORD bytes = static_cast<DWORD>(frames * track.channels * sizeof(std::int16_t));
            winrt::com_ptr<IMFMediaBuffer> buffer;
            winrt::check_hresult(MFCreateMemoryBuffer(bytes, buffer.put()));
            BYTE* data = nullptr;
            winrt::check_hresult(buffer->Lock(&data, nullptr, nullptr));
            std::memcpy(data, pcm.data() + done * track.channels, bytes);
            buffer->Unlock();
            winrt::check_hresult(buffer->SetCurrentLength(bytes));
            winrt::com_ptr<IMFSample> sample;
            winrt::check_hresult(MFCreateSample(sample.put()));
            winrt::check_hresult(sample->AddBuffer(buffer.get()));
            winrt::check_hresult(sample->SetSampleTime(static_cast<LONGLONG>(first_[slot] + done) * second / AudioTrack::rate));
            winrt::check_hresult(sample->SetSampleDuration(static_cast<LONGLONG>(frames) * second / AudioTrack::rate));
            track.encoder->encode(sample.get(), Keep{track});
        }
    }

    void writeEncoded() {
        for (std::size_t t = 0; t < tracks_.size(); ++t) {
            stage_ = L"writing audio track " + std::to_wstring(t + 1);
            for (const auto& aac : tracks_[t].aac) winrt::check_hresult(writer_->WriteSample(tracks_[t].stream, aac.get()));
            tracks_[t].aac.clear();
        }
    }

    static constexpr std::size_t audioBatchFrames = AudioTrack::rate; // A second: a whole number of pieces.
    static_assert(audioBatchFrames % audioChunkFrames == 0);

    IMFSinkWriter* writer_;
    const ClipFormat& format_;
    std::wstring& stage_;
    std::vector<Track> tracks_;
    std::optional<TrackWorkers> workers_; // After tracks_: its threads end before the tracks they encode go.
    std::vector<std::vector<std::int16_t>> pieces_;
    std::size_t made_ = 0, written_ = 0; // Frames made into seconds, and written to the file.
    std::array<std::size_t, 2> first_{}, frames_{};
    int next_ = 0; // The slot to encode next.
};

// Alternate group and names for the audio tracks (see markedMoov). Media
// Foundation writes the index after the media data, so the titles can extend
// the file; an index anywhere else is only patched in place.
void markAudioTracks(const std::filesystem::path& path, const std::vector<std::string>& names, bool alternates) {
    const auto length = std::filesystem::file_size(path);
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
            const auto put = [&](const std::vector<std::uint8_t>& index) {
                file.clear();
                return static_cast<bool>(file.seekp(static_cast<std::streamoff>(offset))
                                             .write(reinterpret_cast<const char*>(index.data()), static_cast<std::streamsize>(index.size()))
                                             .flush());
            };
            const bool atEnd = offset + size == length;
            const auto original = moov;
            if (put(markedMoov(std::move(moov), names, alternates, atEnd))) return;
            // The titles are cosmetic: if writing the larger index failed (a full disk), mark the tracks in place,
            // without titles, and cut the file back to its length, rather than losing the clip.
            if (atEnd) {
                file.close();
                std::error_code error;
                std::filesystem::resize_file(path, length, error);
                file.open(path, std::ios::in | std::ios::out | std::ios::binary);
            }
            if (!file || !put(markedMoov(original, names, alternates, false)))
                throw std::runtime_error("the audio tracks could not be marked"); // The index itself may be broken now.
            return;
        }
        offset += size;
    }
}

// `stage` names the step in progress, so a failed save says where it stopped.
void write(const std::filesystem::path& path, const std::vector<PacketPtr>& packets, const ClipFormat& format,
           std::wstring& stage) {
    const ComApartment apartment; // Outlives every COM object below.
    stage = L"opening the file";
    winrt::com_ptr<IMFAttributes> attributes;
    winrt::check_hresult(MFCreateAttributes(attributes.put(), 2));
    winrt::check_hresult(attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));
    // Not throttled: a throttled WriteSample waits for the MP4 sink to want more of that track, and the sink waits for
    // the other tracks to catch up. Where the video has a gap (a still screen gets no frames), the audio beside it
    // waited for video this thread could not write: a 300 s clip of a desktop took 25 minutes (2026-10-30). Both
    // tracks are passed through, so all the sink can queue is AAC (about 84 KB a second for four tracks) and packets
    // the snapshot holds anyway.
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

    AudioWriter audio(writer.get(), format, stage);
    stage = L"starting to write";
    winrt::check_hresult(writer->BeginWriting());

    // Interleave audio with video by time so the sink never buffers a whole track; the tracks are made a second at a
    // time, just before they are written.
    const auto start = packets.front()->time;
    for (std::size_t i = 0; i < packets.size(); ++i) {
        const auto& packet = packets[i];
        const auto* next = i + 1 < packets.size() ? packets[i + 1].get() : nullptr;
        audio.writeUntil(packet->time - start + second / 2); // A second at a time: 0.5 to 1.5 s ahead, about as before.
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
    audio.finish();
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
        if (format.audio && !format.audioNames.empty()) markAudioTracks(partial, format.audioNames, format.audioNames.size() > 1);
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
