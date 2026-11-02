#pragma once
// The resampler every decoded audio track goes through: 48 kHz interleaved stereo float, as the player and the
// trim mix take it.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}
#include "player/mono_matrix.h"
namespace refarter::player::detail {
// Allocates and opens `*resampler` for `decoder`'s output, decoded from `stream`. Refarter's microphone track (mono, named
// by its handler: see unityMonoMatrix) plays at its own level on both channels, as it sounded in the stereo mix;
// swresample's default would lower it by 3 dB. Every other track, mono ones included, keeps the default.
inline bool openStereoResampler(SwrContext** resampler, const AVCodecContext* decoder, const AVStream* stream, int rate) {
    AVChannelLayout stereo{};
    av_channel_layout_default(&stereo, 2);
    const int made = swr_alloc_set_opts2(resampler, &stereo, AV_SAMPLE_FMT_FLT, rate, &decoder->ch_layout,
                                         decoder->sample_fmt, decoder->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&stereo);
    if (made < 0) return false;
    const auto* handler = av_dict_get(stream->metadata, "handler_name", nullptr, 0);
    if (unityMonoMatrix(handler ? handler->value : "", decoder->ch_layout.nb_channels)) {
        const double unity[] = {1.0, 1.0}; // Left and right each take the one channel whole; kept by later swr_init calls.
        if (swr_set_matrix(*resampler, unity, 1) < 0) return false;
    }
    return swr_init(*resampler) >= 0;
}
} // namespace refarter::player::detail
