#include "player/clip_export.h"
#include "player/export_plan.h"
#include "player/export_luma.h"
#include "player/mix_encoder.h"
#include "diagnostic_log.h"
#include <d3d11.h>
#include <dxgi.h>
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <limits>
#include <string_view>

namespace refarter::player {
namespace {
using ExportStage = CompressionStage;
thread_local ExportStage exportStage = ExportStage::Prepare;
void mark(ExportStage stage) {
    exportStage = stage;
    diagnosticLog().write(DiagnosticEvent::CompressionStage, S_OK, static_cast<unsigned>(stage));
}
void failure(int code = 0) {
    diagnosticLog().write(DiagnosticEvent::CompressionError, static_cast<HRESULT>(code), static_cast<unsigned>(exportStage));
}
unsigned number(std::uint64_t value) { return static_cast<unsigned>(std::min<std::uint64_t>(value, UINT_MAX)); }
unsigned milliseconds(double seconds) {
    return std::isfinite(seconds) && seconds > 0 ? static_cast<unsigned>(std::min(seconds * 1000, static_cast<double>(UINT_MAX))) : 0;
}
void logBytes(DiagnosticEvent event, std::uint64_t bytes) {
    diagnosticLog().write(event, S_OK, static_cast<unsigned>(bytes >> 32), static_cast<unsigned>(bytes));
}
struct ExportCounts {
    std::uint64_t decoded = 0, submitted = 0, encoded = 0, muxed = 0;
    ~ExportCounts() {
        diagnosticLog().write(DiagnosticEvent::CompressionFrames, S_OK, number(decoded), number(submitted));
        diagnosticLog().write(DiagnosticEvent::CompressionPackets, S_OK, number(encoded), number(muxed));
    }
};

// Inspect only the decoder's selected adapter, without names or device paths.
// FFmpeg's h264_mf encoder does not expose its chosen MFT/adapter here.
void logDecodeAdapter(AVBufferRef* device) {
    if (!diagnosticLog().enabled()) return;
    const auto* context = reinterpret_cast<const AVHWDeviceContext*>(device->data);
    const auto* d3d = static_cast<const AVD3D11VADeviceContext*>(context->hwctx);
    IDXGIDevice* graphics = nullptr;
    if (!d3d || !d3d->device || FAILED(d3d->device->QueryInterface(IID_PPV_ARGS(&graphics)))) return;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(graphics->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(adapter->GetDesc(&description)))
            diagnosticLog().write(DiagnosticEvent::CompressionAdapter, S_OK, description.VendorId, description.DeviceId);
        LARGE_INTEGER driver{};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver)))
            diagnosticLog().write(DiagnosticEvent::CompressionDriver, S_OK, static_cast<unsigned>(driver.HighPart), driver.LowPart);
        adapter->Release();
    }
    graphics->Release();
}

void logLuma(DiagnosticEvent event, const AVFrame& frame) {
    if (const auto sample = sampleExportLuma(frame))
        diagnosticLog().write(event, S_OK, (sample->minimum << 8) | sample->maximum, sample->mean);
    else diagnosticLog().write(event, E_NOTIMPL);
}

std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
void require(bool ok, const wchar_t* message) {
    if (!ok) {
        if (std::wstring_view(message) != L"compression cancelled") failure();
        throw std::wstring(message);
    }
}
void windowsCheck(HRESULT result) {
    if (FAILED(result)) diagnosticLog().write(DiagnosticEvent::CompressionSystemError, result, static_cast<unsigned>(exportStage));
}
void windowsRequire(bool ok, const wchar_t* message) {
    if (!ok) windowsCheck(HRESULT_FROM_WIN32(GetLastError()));
    require(ok, message);
}
void check(int code, const wchar_t* message) {
    if (code >= 0) return;
    failure(code);
    char detail[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, detail, sizeof(detail));
    std::wstring text(message);
    text += L" (";
    for (const char c : std::string(detail)) text += static_cast<wchar_t>(c);
    throw text + L")";
}
struct Resources {
    AVFormatContext* input = nullptr;
    AVFormatContext* output = nullptr;
    AVCodecContext* decoder = nullptr;
    AVCodecContext* encoder = nullptr;
    AVBufferRef* device = nullptr;
    AVFrame* decoded = av_frame_alloc();
    AVFrame* downloaded = av_frame_alloc();
    AVFrame* scaled = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    AVPacket* encoded = av_packet_alloc();
    AVPacket* held = av_packet_alloc(); // One encoded picture, to derive durations from adjacent PTS.
    SwsContext* scaler = nullptr;
    ~Resources() {
        if (output && output->pb) avio_closep(&output->pb);
        avformat_free_context(output);
        avformat_close_input(&input);
        avcodec_free_context(&decoder); avcodec_free_context(&encoder);
        av_buffer_unref(&device);
        av_frame_free(&decoded); av_frame_free(&downloaded); av_frame_free(&scaled);
        av_packet_free(&packet); av_packet_free(&encoded);
        av_packet_free(&held);
        sws_freeContext(scaler);
    }
};
struct SourceLock {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~SourceLock() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
struct Temporary {
    std::filesystem::path path;
    ~Temporary() { if (!path.empty()) DeleteFileW(path.c_str()); }
};
int interrupt(void* context) { return static_cast<const std::atomic<bool>*>(context)->load() ? 1 : 0; }
AVPixelFormat hardwareFormat(AVCodecContext*, const AVPixelFormat* formats) {
    for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p) if (*p == AV_PIX_FMT_D3D11) return *p;
    return AV_PIX_FMT_NONE;
}
// The encoder spreads its CBR budget over the pictures it expects each second, so it gets the
// clip's average rate. Capture timestamps jitter (8 ms/25 ms pairs at 60 fps), which makes FFmpeg's
// guess, the smallest regular interval, say 120; copies then came out at about half their budget.
AVRational averageRate(AVFormatContext* input, AVStream* stream) {
    AVRational rate = stream->avg_frame_rate;
    if (rate.num <= 0 || rate.den <= 0) rate = av_guess_frame_rate(input, stream, nullptr);
    if (rate.num <= 0 || rate.den <= 0) return {60, 1};
    return {std::max(1, static_cast<int>(std::lround(av_q2d(rate)))), 1}; // A hint only; timestamps keep the capture clock.
}
bool openDecoder(Resources& r, AVStream* stream, bool hardware) {
    avcodec_free_context(&r.decoder);
    av_buffer_unref(&r.device);
    const auto id = stream->codecpar->codec_id;
    const auto failed = [&](int code) {
        diagnosticLog().write(DiagnosticEvent::CompressionDecoder, static_cast<HRESULT>(code), hardware, id);
        return false;
    };
    const auto* codec = id == AV_CODEC_ID_AV1 ? avcodec_find_decoder_by_name(hardware ? "av1" : "libdav1d") : avcodec_find_decoder(id);
    if (!codec) return failed(AVERROR_DECODER_NOT_FOUND);
    r.decoder = avcodec_alloc_context3(codec);
    if (!r.decoder) return failed(AVERROR(ENOMEM));
    const int parameters = avcodec_parameters_to_context(r.decoder, stream->codecpar);
    if (parameters < 0) return failed(parameters);
    r.decoder->pkt_timebase = stream->time_base;
    r.decoder->thread_count = 2; // Bounded CPU decoding when D3D11 is unavailable.
    if (hardware) {
        bool supported = false;
        for (int i = 0; const auto* config = avcodec_get_hw_config(codec, i); ++i)
            if (config->device_type == AV_HWDEVICE_TYPE_D3D11VA && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) supported = true;
        if (!supported) return failed(AVERROR(ENOSYS));
        const int device = av_hwdevice_ctx_create(&r.device, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
        diagnosticLog().write(DiagnosticEvent::CompressionDecodeDevice, static_cast<HRESULT>(device));
        if (device < 0) return failed(device);
        logDecodeAdapter(r.device);
        r.decoder->hw_device_ctx = av_buffer_ref(r.device);
        r.decoder->get_format = hardwareFormat;
        r.decoder->thread_count = 1;
    }
    const int opened = avcodec_open2(r.decoder, codec, nullptr);
    diagnosticLog().write(DiagnosticEvent::CompressionDecoder, static_cast<HRESULT>(opened), hardware, id);
    return opened >= 0;
}

void encodeFile(const std::filesystem::path& source, const std::filesystem::path& partial, double budget,
                std::atomic<float>& progress, const std::atomic<bool>& cancel) {
    mark(ExportStage::OpenInput);
    ExportCounts counts;
    Resources r;
    require(r.decoded && r.downloaded && r.scaled && r.packet && r.encoded && r.held, L"not enough memory to compress this clip");
    r.input = avformat_alloc_context();
    require(r.input != nullptr, L"not enough memory to open this clip");
    r.input->interrupt_callback = {interrupt, const_cast<std::atomic<bool>*>(&cancel)};
    check(avformat_open_input(&r.input, utf8(source).c_str(), nullptr, nullptr), L"could not open this clip (MP4 recordings are supported)");
    mark(ExportStage::ReadStreams);
    check(avformat_find_stream_info(r.input, nullptr), L"could not read this clip");
    const int video = av_find_best_stream(r.input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    require(video >= 0, L"this clip has no video");
    auto* stream = r.input->streams[video];
    diagnosticLog().write(DiagnosticEvent::CompressionSource, S_OK, stream->codecpar->width, stream->codecpar->height);
    diagnosticLog().write(DiagnosticEvent::CompressionSourceFormat, S_OK, stream->codecpar->codec_id, stream->codecpar->format);
    diagnosticLog().write(DiagnosticEvent::CompressionColour, S_OK, stream->codecpar->color_space, stream->codecpar->color_range);
    diagnosticLog().write(DiagnosticEvent::CompressionTimeBase, S_OK, stream->time_base.num, stream->time_base.den);
    const auto sourceRate = averageRate(r.input, stream);
    diagnosticLog().write(DiagnosticEvent::CompressionFrameRate, S_OK, sourceRate.num, sourceRate.den);
    require(stream->codecpar->color_trc != AVCOL_TRC_SMPTE2084 && stream->codecpar->color_trc != AVCOL_TRC_ARIB_STD_B67,
            L"HDR compression is not supported yet");
    int audio = -1; // The first track is Refarter's stored mix, not all sources added together again.
    for (unsigned i = 0; i < r.input->nb_streams; ++i)
        if (r.input->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { audio = static_cast<int>(i); break; }
    const double videoDuration = stream->duration > 0 ? stream->duration * av_q2d(stream->time_base) : 0;
    const double duration = std::max(videoDuration, r.input->duration > 0 ? r.input->duration / static_cast<double>(AV_TIME_BASE) : 0);
    diagnosticLog().write(DiagnosticEvent::CompressionDuration, S_OK, milliseconds(duration));
    mark(ExportStage::Plan);
    const auto plan = planExport(stream->codecpar->width, stream->codecpar->height, duration, audio >= 0, budget);
    require(plan.has_value(), L"this clip is too long for a useful 20mb copy; trim it first");
    diagnosticLog().write(DiagnosticEvent::CompressionPlan, S_OK, plan->width, plan->height);
    diagnosticLog().write(DiagnosticEvent::CompressionRate, S_OK, plan->bitrate, plan->thirtyFps);
    mark(ExportStage::Decoder);
    require(openDecoder(r, stream, true) || openDecoder(r, stream, false), L"this video format cannot be decoded");
    mark(ExportStage::Encoder);
    check(avformat_alloc_output_context2(&r.output, nullptr, "mp4", utf8(partial).c_str()), L"could not create the compressed copy");
    r.output->interrupt_callback = r.input->interrupt_callback;
    const auto* codec = avcodec_find_encoder_by_name("h264_mf");
    require(codec != nullptr, L"hardware H.264 compression is not available in this build");
    const auto openEncoder = [&](int index, int profile) {
        avcodec_free_context(&r.encoder);
        r.encoder = avcodec_alloc_context3(codec);
        require(r.encoder != nullptr, L"not enough memory to compress this clip");
        r.encoder->width = plan->width; r.encoder->height = plan->height;
        r.encoder->sample_aspect_ratio = stream->sample_aspect_ratio;
        r.encoder->pix_fmt = AV_PIX_FMT_NV12;
        r.encoder->time_base = stream->time_base; // Preserve the capture clock; never invent a rounded frame grid.
        r.encoder->framerate = plan->thirtyFps ? AVRational{30, 1} : sourceRate;
        r.encoder->bit_rate = plan->bitrate;
        r.encoder->gop_size = 120;
        r.encoder->profile = profile; // MF defaults to Baseline, which GPU decoders reject; players then fall back to software.
        r.encoder->max_b_frames = 0;
        r.encoder->color_range = stream->codecpar->color_range;
        r.encoder->colorspace = stream->codecpar->color_space;
        r.encoder->color_trc = stream->codecpar->color_trc;
        r.encoder->color_primaries = stream->codecpar->color_primaries;
        r.encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check(av_opt_set_int(r.encoder->priv_data, "hw_encoding", 1, 0), L"could not request hardware compression");
        check(av_opt_set_int(r.encoder->priv_data, "mft_index", index, 0), L"could not request hardware compression");
        check(av_opt_set(r.encoder->priv_data, "rate_control", "cbr", 0), L"could not set the compression bitrate");
        const int result = avcodec_open2(r.encoder, codec, nullptr);
        diagnosticLog().write(DiagnosticEvent::CompressionEncoder, static_cast<HRESULT>(result), r.encoder->pix_fmt, index); // One line per attempt, so retries are visible.
        return result;
    };
    // Windows lists hardware encoders in its own order and h264_mf takes the first. That one can
    // refuse system-memory pictures (an AMD iGPU's beside an NVIDIA card's), so try each in turn.
    int opened = AVERROR_ENCODER_NOT_FOUND;
    for (int index = 0; index < 16; ++index) {
        const int high = openEncoder(index, AV_PROFILE_H264_HIGH); // An encoder that refuses High gets its default.
        if (high == AVERROR_ENCODER_NOT_FOUND) break; // No encoders left to try.
        opened = high >= 0 ? high : openEncoder(index, AV_PROFILE_UNKNOWN);
        if (opened >= 0) break;
    }
    require(opened != AVERROR_ENCODER_NOT_FOUND, L"no hardware H.264 encoder was found on this computer");
    check(opened, L"no hardware H.264 encoder on this computer could compress this clip");
    auto* outVideo = avformat_new_stream(r.output, nullptr);
    require(outVideo != nullptr, L"could not create the video track");
    check(avcodec_parameters_from_context(outVideo->codecpar, r.encoder), L"could not set up the video track");
    outVideo->time_base = r.encoder->time_base;
    const double origin = r.input->start_time != AV_NOPTS_VALUE ? r.input->start_time / static_cast<double>(AV_TIME_BASE) :
        stream->start_time != AV_NOPTS_VALUE ? stream->start_time * av_q2d(stream->time_base) : 0;
    const double videoEnd = videoDuration > 0 ? videoDuration +
        (stream->start_time != AV_NOPTS_VALUE ? stream->start_time * av_q2d(stream->time_base) - origin : 0) : duration;
    detail::MixEncoder mix;
    mark(ExportStage::Audio);
    if (audio >= 0) {
        auto* outAudio = avformat_new_stream(r.output, nullptr);
        require(outAudio && mix.open(r.input, {audio}, r.output, outAudio, origin, 0, duration, 96'000), L"could not set up the audio mix");
        av_dict_set(&outAudio->metadata, "title", "mix", 0);
    }
    mark(ExportStage::Header);
    check(avio_open(&r.output->pb, utf8(partial).c_str(), AVIO_FLAG_WRITE), L"could not write the compressed copy");
    AVDictionary* options = nullptr;
    av_dict_set(&options, "movflags", "+faststart", 0);
    const int header = avformat_write_header(r.output, &options);
    av_dict_free(&options);
    check(header, L"could not write the MP4 header");
    r.scaled->format = AV_PIX_FMT_NV12;
    r.scaled->width = plan->width; r.scaled->height = plan->height;
    check(av_frame_get_buffer(r.scaled, 32), L"not enough memory for compression");
    const auto write = [&](AVPacket* packet) {
        if (cancel.load()) return false;
        const auto previousStage = exportStage;
        exportStage = ExportStage::Mux;
        const bool videoPacket = packet->stream_index == outVideo->index;
        check(av_interleaved_write_frame(r.output, packet), L"could not write the compressed copy (check free disk space)");
        if (videoPacket) ++counts.muxed;
        exportStage = previousStage;
        return true;
    };
    const auto drain = [&] {
        int code = 0;
        while ((code = avcodec_receive_packet(r.encoder, r.encoded)) >= 0) {
            ++counts.encoded;
            r.encoded->stream_index = outVideo->index;
            av_packet_rescale_ts(r.encoded, r.encoder->time_base, outVideo->time_base);
            if (r.held->buf) {
                r.held->duration = r.encoded->dts - r.held->dts;
                require(r.held->duration > 0, L"the hardware encoder returned invalid video timestamps");
                require(write(r.held), L"compression cancelled");
            }
            av_packet_move_ref(r.held, r.encoded);
        }
        if (code != AVERROR_EOF && code != AVERROR(EAGAIN)) check(code, L"hardware compression failed");
    };
    std::int64_t lastPts = AV_NOPTS_VALUE, lastBucket = -1;
    std::uint64_t frames = 0;
    unsigned lumaSamples = 0;
    const auto pictures = [&] {
        int code = 0;
        while ((code = avcodec_receive_frame(r.decoder, r.decoded)) >= 0) {
            ++counts.decoded;
            require(!cancel.load(), L"compression cancelled");
            const auto pts = r.decoded->best_effort_timestamp;
            require(pts != AV_NOPTS_VALUE, L"this clip has missing video timestamps");
            const double seconds = pts * av_q2d(stream->time_base) - origin;
            const auto bucket = static_cast<std::int64_t>(std::floor(std::max(0.0, seconds) * 30 + 1e-6));
            if (plan->thirtyFps && bucket == lastBucket) { av_frame_unref(r.decoded); continue; }
            lastBucket = bucket;
            AVFrame* picture = r.decoded;
            if (picture->format == AV_PIX_FMT_D3D11) {
                exportStage = ExportStage::Download;
                av_frame_unref(r.downloaded);
                check(av_hwframe_transfer_data(r.downloaded, picture, 0), L"could not read a decoded GPU picture");
                picture = r.downloaded;
            }
            if (frames == 0) diagnosticLog().write(DiagnosticEvent::CompressionFrameFormat, S_OK,
                static_cast<unsigned>(r.decoded->format), static_cast<unsigned>(picture->format));
            exportStage = ExportStage::Scale;
            require(picture->width == stream->codecpar->width && picture->height == stream->codecpar->height,
                    L"the clip changes resolution; trim it before compression");
            r.scaler = sws_getCachedContext(r.scaler, picture->width, picture->height, static_cast<AVPixelFormat>(picture->format),
                plan->width, plan->height, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
            require(r.scaler != nullptr, L"could not resize this clip");
            const int colour = stream->codecpar->color_space == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_DEFAULT;
            const int full = stream->codecpar->color_range == AVCOL_RANGE_JPEG;
            check(sws_setColorspaceDetails(r.scaler, sws_getCoefficients(colour), full,
                sws_getCoefficients(colour), full, 0, 1 << 16, 1 << 16), L"could not preserve video colours");
            check(av_frame_make_writable(r.scaled), L"not enough memory for compression");
            require(sws_scale(r.scaler, picture->data, picture->linesize, 0, picture->height, r.scaled->data, r.scaled->linesize) == plan->height,
                    L"could not resize a video picture");
            // Only two tiny CPU-plane samples per attempt, on the export worker.
            // There is no extra GPU readback, output decode or per-frame logging.
            if (diagnosticLog().enabled() && (lumaSamples == 0 || (lumaSamples == 1 && seconds >= duration / 2))) {
                diagnosticLog().write(DiagnosticEvent::CompressionLumaSample, S_OK, ++lumaSamples, milliseconds(seconds));
                logLuma(DiagnosticEvent::CompressionDecodedLuma, *picture);
                logLuma(DiagnosticEvent::CompressionScaledLuma, *r.scaled);
            }
            r.scaled->pts = pts - static_cast<std::int64_t>(std::llround(origin / av_q2d(stream->time_base)));
            require(lastPts == AV_NOPTS_VALUE || r.scaled->pts > lastPts, L"this clip has non-increasing video timestamps");
            lastPts = r.scaled->pts;
            r.scaled->duration = plan->thirtyFps ? av_rescale_q(1, AVRational{1, 30}, stream->time_base) : r.decoded->duration;
            if (r.scaled->duration <= 0) r.scaled->duration = av_rescale_q(1, av_inv_q(r.encoder->framerate), stream->time_base);
            r.scaled->color_range = r.encoder->color_range; r.scaled->colorspace = r.encoder->colorspace;
            r.scaled->color_primaries = r.encoder->color_primaries; r.scaled->color_trc = r.encoder->color_trc;
            exportStage = ExportStage::Encode;
            check(avcodec_send_frame(r.encoder, r.scaled), L"hardware compression failed");
            ++counts.submitted;
            drain(); ++frames;
            progress.store(static_cast<float>(std::clamp(seconds / duration, 0.0, 0.99)));
            av_frame_unref(r.decoded);
            exportStage = ExportStage::Decode;
        }
        if (code != AVERROR_EOF && code != AVERROR(EAGAIN)) check(code, L"could not decode this clip");
    };
    int read = 0;
    mark(ExportStage::ReadPacket);
    while (!cancel.load() && (read = av_read_frame(r.input, r.packet)) >= 0) {
        if (r.packet->stream_index == video) {
            exportStage = ExportStage::Decode;
            check(avcodec_send_packet(r.decoder, r.packet), L"could not decode a video packet");
            pictures();
        } else if (r.packet->stream_index == audio) {
            exportStage = ExportStage::Audio;
            require(mix.feed(audio, r.packet, write), L"could not encode the audio mix");
        }
        av_packet_unref(r.packet);
        exportStage = ExportStage::ReadPacket;
    }
    require(!cancel.load(), L"compression cancelled");
    if (read != AVERROR_EOF) check(read, L"could not finish reading this clip");
    mark(ExportStage::FlushDecoder);
    check(avcodec_send_packet(r.decoder, nullptr), L"could not finish decoding this clip");
    pictures();
    require(frames > 0, L"this clip has no decodable video pictures");
    mark(ExportStage::FlushEncoder);
    check(avcodec_send_frame(r.encoder, nullptr), L"could not finish hardware compression"); drain();
    if (r.held->buf) {
        r.held->duration = std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(videoEnd / av_q2d(outVideo->time_base))) - r.held->pts);
        require(write(r.held), L"compression cancelled");
    }
    exportStage = ExportStage::Audio;
    if (audio >= 0) require(mix.finish(write), L"could not finish the audio mix");
    mark(ExportStage::Trailer);
    check(av_write_trailer(r.output), L"could not finish the compressed MP4");
    check(avio_closep(&r.output->pb), L"could not finish writing the compressed copy");
}
} // namespace

ExportResult compressClip(const std::filesystem::path& source, std::atomic<float>& progress, const std::atomic<bool>& cancel) {
    const auto began = GetTickCount64();
    diagnosticLog().write(DiagnosticEvent::CompressionStarted);
    diagnosticLog().write(DiagnosticEvent::CompressionLibraries, S_OK, avcodec_version(), avformat_version());
    mark(ExportStage::Prepare);
    ExportResult result;
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct Com { HRESULT status; ~Com() { if (SUCCEEDED(status)) CoUninitialize(); } } apartment{com};
    Temporary temporary;
    SourceLock sourceLock;
    try {
        windowsCheck(com);
        require(SUCCEEDED(com), L"could not initialise the compression worker");
        require(!cancel.load(), L"compression cancelled");
        // Deny writes/deletion for the entire job, including retries and publication.
        mark(ExportStage::LockSource);
        sourceLock.handle = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        windowsRequire(sourceLock.handle != INVALID_HANDLE_VALUE, L"this clip is busy or unavailable; close any editor and try again");
        LARGE_INTEGER bytes{};
        windowsRequire(GetFileSizeEx(sourceLock.handle, &bytes) != FALSE, L"could not read this clip's size");
        logBytes(DiagnosticEvent::CompressionInputBytes, static_cast<std::uint64_t>(bytes.QuadPart));
        if (bytes.QuadPart > 0 && static_cast<std::uint64_t>(bytes.QuadPart) < exportLimit) {
            result.alreadyFits = true;
            diagnosticLog().write(DiagnosticEvent::CompressionSkipped);
            return result;
        }
        GUID id{};
        const auto guidResult = CoCreateGuid(&id);
        windowsCheck(guidResult);
        require(SUCCEEDED(guidResult), L"could not create a temporary file name");
        wchar_t guid[40]{}; StringFromGUID2(id, guid, 40);
        const auto candidate = source.parent_path() / (L"refarter-export-" + std::wstring(guid) + L".part");
        SourceLock reservation;
        reservation.handle = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        windowsRequire(reservation.handle != INVALID_HANDLE_VALUE, L"could not create an export in this folder");
        CloseHandle(reservation.handle); reservation.handle = INVALID_HANDLE_VALUE;
        temporary.path = candidate;
        double budget = 18'800'000;
        for (int attempt = 0; attempt < 3; ++attempt) {
            diagnosticLog().write(DiagnosticEvent::CompressionAttempt, S_OK, attempt + 1, static_cast<unsigned>(budget));
            progress.store(0);
            encodeFile(source, temporary.path, budget, progress, cancel);
            mark(ExportStage::CheckSize);
            std::error_code error;
            const auto size = std::filesystem::file_size(temporary.path, error);
            if (error) windowsCheck(HRESULT_FROM_WIN32(error.value()));
            require(!error && size > 0, L"could not check the compressed file size");
            logBytes(DiagnosticEvent::CompressionOutputBytes, size);
            if (size < exportLimit) {
                require(!cancel.load(), L"compression cancelled");
                mark(ExportStage::Publish);
                for (int copy = 1; copy <= 10'000; ++copy) {
                    const auto path = exportPath(source, copy);
                    // MoveFile never replaces an existing file; naming races are safe.
                    if (MoveFileW(temporary.path.c_str(), path.c_str())) {
                        temporary.path.clear(); result.path = path; progress.store(1);
                        diagnosticLog().write(DiagnosticEvent::CompressionFinished, S_OK, number(GetTickCount64() - began));
                        return result;
                    }
                    const auto failure = GetLastError();
                    if (failure != ERROR_ALREADY_EXISTS && failure != ERROR_FILE_EXISTS) windowsCheck(HRESULT_FROM_WIN32(failure));
                    require(failure == ERROR_ALREADY_EXISTS || failure == ERROR_FILE_EXISTS, L"could not publish the compressed copy");
                }
                throw std::wstring(L"too many compressed copies already exist");
            }
            budget *= static_cast<double>(exportLimit) / static_cast<double>(size) * 0.90;
        }
        throw std::wstring(L"the hardware encoder could not meet 20mb; try trimming the clip first");
    } catch (const std::wstring& message) { result.error = message; }
      catch (const std::exception&) { failure(); result.error = L"could not compress this clip"; }
    result.cancelled = cancel.load();
    if (result.cancelled) result.error.clear();
    diagnosticLog().write(result.cancelled ? DiagnosticEvent::CompressionCancelled : DiagnosticEvent::CompressionError,
        S_OK, static_cast<unsigned>(exportStage));
    return result;
}
} // namespace refarter::player
