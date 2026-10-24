#include "player/media.h"
#include "long_path.h"
#include "text.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswresample/swresample.h>
}

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace refarter::player {

namespace {
constexpr double epsilon = 1e-3;          // Timestamps arrive in stream time bases.
constexpr std::size_t maxQueuedPackets = 600; // Bounds reading ahead for the other stream.


std::wstring wide(const char* text) {
    if (!text) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring result(static_cast<std::size_t>(std::max(size - 1, 0)), L'\0');
    if (size > 1) MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), size);
    return result;
}

AVPixelFormat chooseFormat(AVCodecContext* context, const AVPixelFormat* formats) {
    const bool hardware = context->hw_device_ctx != nullptr;
    for (auto* f = formats; *f != AV_PIX_FMT_NONE; ++f)
        if (hardware && *f == AV_PIX_FMT_D3D11) return *f;
    // Software: the first format that is not a hardware surface.
    for (auto* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        const auto* descriptor = av_pix_fmt_desc_get(*f);
        if (descriptor && !(descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *f;
    }
    return AV_PIX_FMT_NONE;
}

void freePackets(std::deque<AVPacket*>& packets) {
    for (auto* packet : packets) av_packet_free(&packet);
    packets.clear();
}

// The demuxer reads through a handle of the player's own: FFmpeg's file protocol does not share delete access,
// which keeps an open clip from being renamed.
int readFile(void* file, uint8_t* buffer, int size) {
    DWORD read = 0;
    if (!ReadFile(static_cast<HANDLE>(file), buffer, static_cast<DWORD>(size), &read, nullptr)) return AVERROR(EIO);
    return read ? static_cast<int>(read) : AVERROR_EOF;
}

int64_t seekFile(void* file, int64_t offset, int whence) {
    LARGE_INTEGER to{}, at{};
    if (whence == AVSEEK_SIZE) return GetFileSizeEx(static_cast<HANDLE>(file), &at) ? at.QuadPart : AVERROR(EIO);
    whence &= ~AVSEEK_FORCE;
    const DWORD method = whence == SEEK_CUR ? FILE_CURRENT : whence == SEEK_END ? FILE_END : FILE_BEGIN;
    to.QuadPart = offset;
    return SetFilePointerEx(static_cast<HANDLE>(file), to, &at, method) ? at.QuadPart : AVERROR(EIO);
}
} // namespace

void FrameDeleter::operator()(AVFrame* frame) const { av_frame_free(&frame); }

// One decoded audio track, resampled to 48 kHz stereo float and aligned to the seek target.
struct MediaFile::AudioDecoder {
    int stream = -1;
    AVCodecContext* codec = nullptr;
    SwrContext* resampler = nullptr;
    std::deque<AVPacket*> packets;
    bool draining = false, ended = false, aligned = false;
    double time = 0;            // Position of the next sample handed out.
    std::vector<float> pending; // Decoded samples not yet handed out, from pendingAt.
    std::size_t pendingAt = 0;

    ~AudioDecoder() {
        freePackets(packets);
        avcodec_free_context(&codec);
        swr_free(&resampler);
    }
    std::size_t available() const { return (pending.size() - pendingAt) / 2; }
    void reset(double at) {
        avcodec_flush_buffers(codec);
        swr_init(resampler); // Drops samples buffered for resampling.
        freePackets(packets);
        draining = ended = aligned = false;
        time = at;
        pending.clear();
        pendingAt = 0;
    }
    // Hands out `frames` decoded frames, copied or added to `out`.
    void take(float* out, std::size_t frames, bool add) {
        const auto* from = pending.data() + pendingAt;
        if (add) for (std::size_t i = 0; i < frames * 2; ++i) out[i] += from[i];
        else std::copy_n(from, frames * 2, out);
        pendingAt += frames * 2;
        time += static_cast<double>(frames) / 48000;
    }
};

MediaFile::MediaFile() { scratch_ = av_frame_alloc(); }

MediaFile::~MediaFile() {
    freePackets(videoPackets_);
    peeked_ = {};
    closeAudio();
    avcodec_free_context(&video_);
    av_buffer_unref(&device_);
    avformat_close_input(&format_); // Leaves the custom reader to its owner.
    if (io_) av_freep(&io_->buffer);
    avio_context_free(&io_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    av_frame_free(&scratch_);
}

bool MediaFile::open(const std::filesystem::path& path, const Options& options) {
    options_ = options;
    constexpr int bufferSize = 32768; // FFmpeg's own for files.
    file_ = CreateFileW(extendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    auto* buffer = file_ != INVALID_HANDLE_VALUE ? static_cast<unsigned char*>(av_malloc(bufferSize)) : nullptr;
    if (buffer) io_ = avio_alloc_context(buffer, bufferSize, 0, file_, readFile, nullptr, seekFile);
    if (buffer && !io_) av_free(buffer);
    format_ = io_ ? avformat_alloc_context() : nullptr;
    if (format_) format_->pb = io_;
    // The name is still given: format probing and error messages use it.
    if (!format_ || avformat_open_input(&format_, utf8(path.wstring()).c_str(), nullptr, nullptr) < 0) {
        std::error_code missing; // This runs on the engine and thumbnail threads: the throwing overload must not.
        error_ = std::filesystem::exists(path, missing) ? L"this file could not be read as a video." : L"this clip was moved or deleted.";
        return false;
    }
    if (avformat_find_stream_info(format_, nullptr) < 0) {
        error_ = L"this file could not be read as a video.";
        return false;
    }
    videoStream_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStream_ < 0) {
        error_ = L"this file has no video refarter can play.";
        return false;
    }
    auto* stream = format_->streams[videoStream_];
    origin_ = stream->start_time != AV_NOPTS_VALUE ? stream->start_time * av_q2d(stream->time_base) : 0;
    duration_ = format_->duration != AV_NOPTS_VALUE ? format_->duration / static_cast<double>(AV_TIME_BASE) : 0;
    if (stream->duration != AV_NOPTS_VALUE) duration_ = std::max(duration_, stream->duration * av_q2d(stream->time_base));
    const auto rate = av_guess_frame_rate(format_, stream, nullptr);
    if (rate.num > 0 && rate.den > 0) frameDuration_ = av_q2d(av_inv_q(rate));
    for (int i = 0, n = avformat_index_get_entries_count(stream); i < n; ++i) {
        const auto* entry = avformat_index_get_entry(stream, i);
        if (entry && (entry->flags & AVINDEX_KEYFRAME)) keyframes_.push_back(seconds(entry->timestamp, videoStream_));
    }
    std::sort(keyframes_.begin(), keyframes_.end());

    for (unsigned i = 0; i < format_->nb_streams; ++i) {
        const auto* parameters = format_->streams[i]->codecpar;
        if (options.audio && parameters->codec_type == AVMEDIA_TYPE_AUDIO && avcodec_find_decoder(parameters->codec_id))
            audioStreams_.push_back(static_cast<int>(i));
    }
    // Start with the track players treat as default (Refarter's mix), else the first.
    int first = audioStreams_.empty() ? -1 : 0;
    for (int t = 0; t < audioTracks(); ++t)
        if (format_->streams[audioStreams_[t]]->disposition & AV_DISPOSITION_DEFAULT) { first = t; break; }
    for (unsigned i = 0; i < format_->nb_streams; ++i)
        format_->streams[i]->discard = static_cast<int>(i) == videoStream_ ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    if (first >= 0) selectAudio(first);

    // Prefer the GPU; fall back to software when it cannot decode the first picture.
    Picture opening;
    if (options.device && !options.software && openVideo(true)) opening = seek(0);
    if (!opening && openVideo(false)) opening = seek(0);
    if (!opening) {
        if (error_.empty()) error_ = L"this video format cannot be decoded.";
        return false;
    }
    // The index holds decoding times; with reordered frames (B-frames) a keyframe
    // shows a little later. The first keyframe packet tells by how much.
    if (keyframeDelay_ > 1e-4 && keyframeDelay_ < 1)
        for (auto& key : keyframes_) key += keyframeDelay_;
    return true;
}

bool MediaFile::openVideo(bool hardware) {
    avcodec_free_context(&video_);
    av_buffer_unref(&device_);
    peeked_ = {};
    hardware_ = false;
    const auto* parameters = format_->streams[videoStream_]->codecpar;
    const AVCodec* codec = nullptr;
    if (parameters->codec_id == AV_CODEC_ID_AV1)
        codec = avcodec_find_decoder_by_name(hardware ? "av1" : "libdav1d"); // The built-in AV1 decoder is GPU-only.
    if (!codec) codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) { error_ = L"this video format is not supported."; return false; }
    video_ = avcodec_alloc_context3(codec);
    if (!video_ || avcodec_parameters_to_context(video_, parameters) < 0) return false;
    video_->pkt_timebase = format_->streams[videoStream_]->time_base;
    video_->get_format = chooseFormat;
    if (hardware) {
        bool supported = false;
        for (int i = 0; const auto* config = avcodec_get_hw_config(codec, i); ++i)
            if (config->device_type == AV_HWDEVICE_TYPE_D3D11VA && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
                supported = true;
        if (!supported) return false;
        device_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!device_) return false;
        auto* context = reinterpret_cast<AVHWDeviceContext*>(device_->data);
        auto* d3d = static_cast<AVD3D11VADeviceContext*>(context->hwctx);
        options_.device->AddRef(); // Released by FFmpeg with the device context.
        d3d->device = options_.device;
        if (av_hwdevice_ctx_init(device_) < 0) { av_buffer_unref(&device_); return false; }
        video_->hw_device_ctx = av_buffer_ref(device_);
        video_->extra_hw_frames = 6; // Pictures held for display while the decoder runs ahead.
        video_->thread_count = 1;
    } else {
        video_->thread_count = 0; // One per core.
    }
    if (avcodec_open2(video_, codec, nullptr) < 0) {
        avcodec_free_context(&video_);
        error_ = L"this video format cannot be decoded.";
        return false;
    }
    hardware_ = hardware;
    decoderName_ = wide(codec->name);
    return true;
}

bool MediaFile::selectAudio(const std::vector<int>& tracks) {
    if (tracks == selection_) return true;
    closeAudio();
    for (const int track : tracks) {
        if (track < 0 || track >= audioTracks()) { closeAudio(); return false; }
        auto* stream = format_->streams[audioStreams_[track]];
        const auto* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        auto decoder = std::make_unique<AudioDecoder>();
        decoder->stream = audioStreams_[track];
        decoder->codec = avcodec_alloc_context3(codec);
        if (!decoder->codec || avcodec_parameters_to_context(decoder->codec, stream->codecpar) < 0 ||
            avcodec_open2(decoder->codec, codec, nullptr) < 0) {
            closeAudio();
            return false;
        }
        decoder->codec->pkt_timebase = stream->time_base;
        AVChannelLayout stereo{};
        av_channel_layout_default(&stereo, 2);
        const int made = swr_alloc_set_opts2(&decoder->resampler, &stereo, AV_SAMPLE_FMT_FLT, 48000,
            &decoder->codec->ch_layout, decoder->codec->sample_fmt, decoder->codec->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&stereo);
        if (made < 0 || swr_init(decoder->resampler) < 0) { closeAudio(); return false; }
        stream->discard = AVDISCARD_DEFAULT;
        decoders_.push_back(std::move(decoder));
        selection_.push_back(track);
    }
    return true;
}

void MediaFile::closeAudio() {
    for (const auto& decoder : decoders_) format_->streams[decoder->stream]->discard = AVDISCARD_ALL;
    decoders_.clear();
    selection_.clear();
}

std::wstring MediaFile::audioTrackName(int track) const {
    if (track < 0 || track >= audioTracks()) return {};
    // Refarter names its tracks in the handler name (Media Foundation's default is "SoundHandler").
    if (const auto* handler = av_dict_get(format_->streams[audioStreams_[track]]->metadata, "handler_name", nullptr, 0)) {
        for (const char* known : {"mix", "desktop", "microphone", "discord", "game", "game output"})
            if (std::strcmp(handler->value, known) == 0) return wide(known);
    }
    // Older Refarter clips: mix, desktop and microphone in that order when both sources record.
    if (audioTracks() == 3) return track == 0 ? L"mix" : track == 1 ? L"desktop" : L"microphone";
    if (audioTracks() == 1) return L"audio";
    const auto* title = av_dict_get(format_->streams[audioStreams_[track]]->metadata, "title", nullptr, 0);
    return title && *title->value ? wide(title->value) : L"track " + std::to_wstring(track + 1);
}

double MediaFile::seconds(long long pts, int stream) const {
    return pts * av_q2d(format_->streams[stream]->time_base) - origin_;
}

bool MediaFile::readPacket() {
    if (demuxEnded_) return false;
    AVPacket* packet = av_packet_alloc();
    while (true) {
        if (av_read_frame(format_, packet) < 0) {
            av_packet_free(&packet);
            demuxEnded_ = true;
            return false;
        }
        if (packet->stream_index == videoStream_) {
            if (keyframeDelay_ < 0 && (packet->flags & AV_PKT_FLAG_KEY) && packet->pts != AV_NOPTS_VALUE && packet->dts != AV_NOPTS_VALUE)
                keyframeDelay_ = (packet->pts - packet->dts) * av_q2d(format_->streams[videoStream_]->time_base);
            videoPackets_.push_back(packet);
            return true;
        }
        for (const auto& decoder : decoders_) {
            if (packet->stream_index != decoder->stream) continue;
            decoder->packets.push_back(packet);
            return true;
        }
        av_packet_unref(packet);
    }
}

Picture MediaFile::next() {
    if (peeked_) return std::move(peeked_);
    if (!video_) return {};
    while (true) {
        const int received = avcodec_receive_frame(video_, scratch_);
        if (received == 0) {
            Picture picture{Frame(av_frame_clone(scratch_)), 0};
            av_frame_unref(scratch_);
            const auto pts = picture.frame->best_effort_timestamp;
            picture.time = pts != AV_NOPTS_VALUE ? seconds(pts, videoStream_) : lastTime_ + frameDuration_;
            lastTime_ = picture.time;
            return picture;
        }
        if (received != AVERROR(EAGAIN)) return {}; // End of stream, or a decoding error.
        while (videoPackets_.empty() && readPacket()) {
            // Reading for video queues audio; stop before an audio-free stretch fills memory.
            for (const auto& decoder : decoders_)
                if (decoder->packets.size() > maxQueuedPackets) freePackets(decoder->packets);
        }
        if (videoPackets_.empty()) {
            if (videoDraining_) return {};
            videoDraining_ = true;
            avcodec_send_packet(video_, nullptr);
            continue;
        }
        AVPacket* packet = videoPackets_.front();
        videoPackets_.pop_front();
        const int sent = avcodec_send_packet(video_, packet);
        av_packet_free(&packet);
        if (sent < 0 && sent != AVERROR(EAGAIN)) return {};
    }
}

Picture MediaFile::seek(double time) {
    return seekPicture(time, false);
}

Picture MediaFile::previous(double time) {
    return seekPicture(time, true);
}

Picture MediaFile::seekPicture(double time, bool previous) {
    time = std::clamp(time, 0.0, std::max(0.0, duration_));
    // Moving forward inside the current group of pictures needs no demuxer seek,
    // which keeps frame steps and slow scrubbing cheap.
    const auto nextKey = std::upper_bound(keyframes_.begin(), keyframes_.end(), lastTime_ + epsilon);
    const bool forward = !previous && lastTime_ >= 0 && time > lastTime_ + epsilon && !videoDraining_ &&
        (nextKey == keyframes_.end() || *nextKey > time + epsilon) && (decoders_.empty() || audioTime_ <= time);
    if (!forward) {
        peeked_ = {};
        auto* stream = format_->streams[videoStream_];
        double seekTime = time;
        if (previous) {
            // Seek to a preceding GOP, including when the current picture is a
            // keyframe. Index entries use DTS; keyframes_ uses presentation time.
            const auto key = std::lower_bound(keyframes_.begin(), keyframes_.end(), time - epsilon);
            seekTime = key == keyframes_.begin() ? 0.0 : *(key - 1);
            seekTime -= std::max(0.0, keyframeDelay_);
        }
        const auto target = static_cast<long long>((seekTime + origin_) / av_q2d(stream->time_base));
        if (av_seek_frame(format_, videoStream_, target, AVSEEK_FLAG_BACKWARD) < 0)
            av_seek_frame(format_, videoStream_, target, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
        freePackets(videoPackets_);
        avcodec_flush_buffers(video_);
        for (const auto& decoder : decoders_) decoder->reset(time);
        demuxEnded_ = videoDraining_ = false;
        lastTime_ = -1;
        alignTo_ = audioTime_ = time;
    } else if (!decoders_.empty()) {
        // Same decoders: skip the audio up to the new position.
        float discard[2048];
        while (audioTime_ + 0.5 / 48000 < time) {
            const auto needed = static_cast<std::size_t>(std::ceil((time - audioTime_) * 48000));
            if (!audio(discard, std::min<std::size_t>(needed, std::size(discard) / 2))) break;
        }
    }
    Picture best;
    while (auto picture = next()) {
        if ((previous ? picture.time < time : picture.time <= time + epsilon) || !best) {
            best = std::move(picture);
            if (previous ? best.time >= time : best.time + epsilon >= time) break; // First picture, or exact seek target.
            continue;
        }
        peeked_ = std::move(picture); // The first picture after the target is shown next.
        break;
    }
    if (best) lastTime_ = std::max(lastTime_, best.time);
    return best;
}

Picture MediaFile::keyframe(double time) {
    auto* stream = format_->streams[videoStream_];
    const auto target = static_cast<long long>((time + origin_) / av_q2d(stream->time_base));
    peeked_ = {};
    av_seek_frame(format_, videoStream_, target, AVSEEK_FLAG_BACKWARD);
    freePackets(videoPackets_);
    for (const auto& decoder : decoders_) freePackets(decoder->packets);
    avcodec_flush_buffers(video_);
    demuxEnded_ = videoDraining_ = false;
    lastTime_ = -1;
    return next();
}

bool MediaFile::decodeAudioFrame(AudioDecoder& decoder) {
    while (true) {
        const int received = avcodec_receive_frame(decoder.codec, scratch_);
        if (received == 0) break;
        if (received != AVERROR(EAGAIN)) { decoder.ended = true; return false; }
        while (decoder.packets.empty() && videoPackets_.size() < maxQueuedPackets && readPacket()) {}
        if (decoder.packets.empty()) {
            if (!demuxEnded_) return false; // Video is far behind; try again later.
            if (decoder.draining) { decoder.ended = true; return false; }
            decoder.draining = true;
            avcodec_send_packet(decoder.codec, nullptr);
            continue;
        }
        AVPacket* packet = decoder.packets.front();
        decoder.packets.pop_front();
        avcodec_send_packet(decoder.codec, packet);
        av_packet_free(&packet);
    }
    const auto pts = scratch_->best_effort_timestamp;
    const int capacity = swr_get_out_samples(decoder.resampler, scratch_->nb_samples);
    std::vector<float> converted(static_cast<std::size_t>(std::max(capacity, 0)) * 2);
    auto* out = reinterpret_cast<uint8_t*>(converted.data());
    const int made = swr_convert(decoder.resampler, &out, capacity, const_cast<const uint8_t**>(scratch_->extended_data), scratch_->nb_samples);
    av_frame_unref(scratch_);
    if (made <= 0) return true;
    converted.resize(static_cast<std::size_t>(made) * 2);
    // Samples are counted from the seek target; frame timestamps only correct
    // the count where they disagree (the first frame, or audio that was lost).
    const double expected = decoder.aligned ? decoder.time + static_cast<double>(decoder.available()) / 48000 : alignTo_;
    const double start = pts != AV_NOPTS_VALUE ? seconds(pts, decoder.stream) : expected;
    const auto offset = static_cast<long long>(std::llround((expected - start) * 48000));
    if (!decoder.aligned || std::abs(offset) > 48000 / 20) {
        if (offset >= made) return true; // Entirely before the target.
        if (offset > 0) converted.erase(converted.begin(), converted.begin() + offset * 2);
        else if (offset < 0) converted.insert(converted.begin(), static_cast<std::size_t>(std::min(-offset, 48000LL)) * 2, 0.f);
        if (!decoder.aligned) decoder.time = alignTo_;
        decoder.aligned = true;
    }
    decoder.pending.erase(decoder.pending.begin(), decoder.pending.begin() + static_cast<std::ptrdiff_t>(decoder.pendingAt));
    decoder.pendingAt = 0;
    decoder.pending.insert(decoder.pending.end(), converted.begin(), converted.end());
    return true;
}

std::size_t MediaFile::audio(float* out, std::size_t frames) {
    if (decoders_.empty()) return 0;
    std::size_t done = 0;
    if (decoders_.size() == 1) {
        // One track plays as stored: copied, no mixing pass.
        auto& decoder = *decoders_.front();
        while (done < frames) {
            if (!decoder.available() && !decodeAudioFrame(decoder)) break;
            const std::size_t take = std::min(decoder.available(), frames - done);
            decoder.take(out + done * 2, take, false);
            done += take;
        }
    } else {
        // Several sources summed: hand out only what each has decoded, so they stay
        // aligned; a source that has ended adds silence.
        done = frames;
        bool playing = false;
        for (const auto& decoder : decoders_) {
            while (decoder->available() < frames && !decoder->ended && decodeAudioFrame(*decoder)) {}
            if (decoder->ended && !decoder->available()) continue;
            done = std::min(done, decoder->available());
            playing = true;
        }
        if (!playing) return 0;
        std::fill_n(out, done * 2, 0.f);
        for (const auto& decoder : decoders_) decoder->take(out, std::min(done, decoder->available()), true);
        for (std::size_t i = 0; i < done * 2; ++i) out[i] = std::clamp(out[i], -1.f, 1.f); // Saturates like the stored mix.
    }
    audioTime_ += static_cast<double>(done) / 48000;
    return done;
}

bool MediaFile::audioEnded() const {
    if (decoders_.empty()) return false;
    for (const auto& decoder : decoders_)
        if (!decoder->ended || decoder->available()) return false;
    return true;
}

} // namespace refarter::player
