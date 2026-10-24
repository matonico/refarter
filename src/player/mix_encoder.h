#pragma once
// Shared AAC mixer for lossless trims and size-limited exports.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>
namespace refarter::player::detail {
// A new "mix" for a trim that leaves sources out: the kept tracks decoded, summed
// sample by sample from the cut start, and encoded as AAC like Refarter's own mix.
class MixEncoder {
public:
    using Write = std::function<bool(AVPacket*)>;
    static constexpr int rate = 48000;

    ~MixEncoder() {
        for (auto& source : sources_) {
            avcodec_free_context(&source.decoder);
            swr_free(&source.resampler);
        }
        avcodec_free_context(&encoder_);
        av_frame_free(&decoded_);
        av_frame_free(&frame_);
        av_packet_free(&packet_);
    }

    // Sets up `out` before the header is written. `start` and `length` are the cut in clip seconds.
    bool open(const AVFormatContext* input, const std::vector<int>& streams, AVFormatContext* output, AVStream* out,
              double origin, double start, double length, int bitrate = 192'000) {
        origin_ = origin;
        start_ = start;
        total_ = std::llround(length * rate);
        decoded_ = av_frame_alloc();
        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        if (!decoded_ || !frame_ || !packet_) return false;
        AVChannelLayout stereo{};
        av_channel_layout_default(&stereo, 2);
        for (const int stream : streams) {
            const auto* parameters = input->streams[stream]->codecpar;
            const auto* codec = avcodec_find_decoder(parameters->codec_id);
            Source source{stream, codec ? avcodec_alloc_context3(codec) : nullptr};
            sources_.push_back(source);
            auto& added = sources_.back();
            if (!added.decoder || avcodec_parameters_to_context(added.decoder, parameters) < 0 ||
                avcodec_open2(added.decoder, codec, nullptr) < 0) return false;
            added.decoder->pkt_timebase = input->streams[stream]->time_base;
            if (swr_alloc_set_opts2(&added.resampler, &stereo, AV_SAMPLE_FMT_FLT, rate, &added.decoder->ch_layout,
                                    added.decoder->sample_fmt, added.decoder->sample_rate, 0, nullptr) < 0 ||
                swr_init(added.resampler) < 0) return false;
        }
        const auto* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        encoder_ = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (!encoder_) return false;
        encoder_->sample_fmt = AV_SAMPLE_FMT_FLTP;
        encoder_->sample_rate = rate;
        av_channel_layout_copy(&encoder_->ch_layout, &stereo);
        encoder_->bit_rate = bitrate;
        encoder_->time_base = {1, rate};
        if (output->oformat->flags & AVFMT_GLOBALHEADER) encoder_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(encoder_, codec, nullptr) < 0 || avcodec_parameters_from_context(out->codecpar, encoder_) < 0) return false;
        out->time_base = encoder_->time_base;
        stream_ = out;
        frame_->format = AV_SAMPLE_FMT_FLTP;
        frame_->nb_samples = encoder_->frame_size;
        return av_channel_layout_copy(&frame_->ch_layout, &stereo) >= 0 && av_frame_get_buffer(frame_, 0) >= 0;
    }

    // One packet of a kept track, from shortly before the cut on; null flushes that decoder.
    bool feed(int stream, const AVPacket* packet, const Write& write) {
        for (auto& source : sources_) {
            if (source.stream != stream) continue;
            if (avcodec_send_packet(source.decoder, packet) < 0 && packet) return true; // A damaged packet: its sound is silence.
            while (avcodec_receive_frame(source.decoder, decoded_) == 0) add(source);
        }
        return encode(false, write);
    }

    // Flushes the decoders and the encoder; pads to the cut's length with silence.
    bool finish(const Write& write) {
        for (const auto& source : sources_) feed(source.stream, nullptr, write);
        if (!encode(true, write)) return false;
        if (avcodec_send_frame(encoder_, nullptr) < 0) return false;
        return drain(write);
    }

private:
    struct Source {
        int stream = -1;
        AVCodecContext* decoder = nullptr;
        SwrContext* resampler = nullptr;
        std::vector<float> pending; // Interleaved stereo from `at` on, starting at the cut.
        std::size_t at = 0;
        bool aligned = false;
        std::size_t available() const { return (pending.size() - at) / 2; }
    };

    // Resamples a decoded frame and lines it up with the cut start by its timestamp.
    void add(Source& source) {
        const auto pts = decoded_->best_effort_timestamp;
        const int capacity = swr_get_out_samples(source.resampler, decoded_->nb_samples);
        std::vector<float> converted(static_cast<std::size_t>(std::max(capacity, 0)) * 2);
        auto* out = reinterpret_cast<uint8_t*>(converted.data());
        const int made = swr_convert(source.resampler, &out, capacity, const_cast<const uint8_t**>(decoded_->extended_data),
                                     decoded_->nb_samples);
        av_frame_unref(decoded_);
        if (made <= 0) return;
        converted.resize(static_cast<std::size_t>(made) * 2);
        if (!source.aligned) {
            if (pts == AV_NOPTS_VALUE) return;
            const double time = pts * av_q2d(source.decoder->pkt_timebase) - origin_;
            const long long offset = std::llround((time - start_) * rate);
            if (offset + made <= 0) return; // Entirely before the cut.
            if (offset < 0) converted.erase(converted.begin(), converted.begin() + (-offset) * 2);
            else converted.insert(converted.begin(), static_cast<std::size_t>(std::min<long long>(offset, rate)) * 2, 0.f);
            source.aligned = true;
        }
        source.pending.erase(source.pending.begin(), source.pending.begin() + static_cast<std::ptrdiff_t>(source.at));
        source.at = 0;
        source.pending.insert(source.pending.end(), converted.begin(), converted.end());
    }

    // Encodes whole frames every source has decoded; at the end, the rest padded with silence.
    bool encode(bool last, const Write& write) {
        while (produced_ < total_) {
            const auto size = static_cast<std::size_t>(std::min<long long>(encoder_->frame_size, total_ - produced_));
            std::size_t ready = size;
            for (const auto& source : sources_) ready = std::min(ready, source.available());
            if (ready < size && !last) return true;
            if (av_frame_make_writable(frame_) < 0) return false;
            auto* left = reinterpret_cast<float*>(frame_->data[0]);
            auto* right = reinterpret_cast<float*>(frame_->data[1]);
            std::fill_n(left, size, 0.f);
            std::fill_n(right, size, 0.f);
            for (auto& source : sources_) {
                const auto take = std::min(size, source.available());
                const float* from = source.pending.data() + source.at;
                for (std::size_t i = 0; i < take; ++i) {
                    left[i] += from[i * 2];
                    right[i] += from[i * 2 + 1];
                }
                source.at += take * 2;
            }
            for (std::size_t i = 0; i < size; ++i) { // Saturates like the stored mix.
                left[i] = std::clamp(left[i], -1.f, 1.f);
                right[i] = std::clamp(right[i], -1.f, 1.f);
            }
            frame_->nb_samples = static_cast<int>(size);
            frame_->pts = produced_;
            produced_ += static_cast<long long>(size);
            if (avcodec_send_frame(encoder_, frame_) < 0 || !drain(write)) return false;
        }
        return true;
    }

    bool drain(const Write& write) {
        while (avcodec_receive_packet(encoder_, packet_) == 0) {
            packet_->stream_index = stream_->index;
            av_packet_rescale_ts(packet_, encoder_->time_base, stream_->time_base);
            if (!write(packet_)) return false;
        }
        return true;
    }

    std::vector<Source> sources_;
    AVCodecContext* encoder_ = nullptr;
    AVStream* stream_ = nullptr;
    AVFrame* decoded_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    double origin_ = 0, start_ = 0;
    long long produced_ = 0, total_ = 0;
};

} // namespace refarter::player::detail
