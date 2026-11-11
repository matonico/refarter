#include "player/clip_combine.h"
#include "player/combine_storage.h"
#include "clip_list.h"
#include "text.h"
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <limits>
#include <stdexcept>
#include <vector>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

namespace refarter::player {
namespace {
struct Problem { std::wstring text; };
void require(bool ok, const wchar_t* text) { if (!ok) throw Problem{text}; }
struct Input {
    AVFormatContext* format = nullptr;
    ~Input() { avformat_close_input(&format); }
    void open(const std::filesystem::path& path, const std::atomic<bool>& cancel) {
        format = avformat_alloc_context();
        require(format != nullptr, L"not enough memory to read a clip");
        format->interrupt_callback = {[](void* p) { return static_cast<const std::atomic<bool>*>(p)->load() ? 1 : 0; }, const_cast<std::atomic<bool>*>(&cancel)};
        require(avformat_open_input(&format, utf8(path.wstring()).c_str(), nullptr, nullptr) >= 0 &&
                avformat_find_stream_info(format, nullptr) >= 0, L"a selected clip could not be read");
        require(std::strstr(format->iformat->name, "mov") != nullptr, L"only MP4 and MOV clips can be combined");
    }
};
struct Packet {
    AVPacket* value = av_packet_alloc();
    Packet() { require(value != nullptr, L"not enough memory to copy a clip"); }
    ~Packet() { av_packet_free(&value); }
};
struct Parameters {
    AVCodecParameters* value = avcodec_parameters_alloc();
    Parameters() { require(value != nullptr, L"not enough memory to check the clips"); }
    ~Parameters() { avcodec_parameters_free(&value); }
};
struct Output {
    AVFormatContext* format = nullptr;
    std::filesystem::path partial;
    ~Output() {
        if (format) { if (format->pb) avio_closep(&format->pb); avformat_free_context(format); }
        if (!partial.empty()) { std::error_code ignored; std::filesystem::remove(partial, ignored); }
    }
};
struct Locks {
    std::vector<HANDLE> files;
    ~Locks() { for (auto file : files) CloseHandle(file); }
};
bool sameExtra(const AVCodecParameters* a, const AVCodecParameters* b) {
    return a->extradata_size == b->extradata_size &&
        (!a->extradata_size || std::memcmp(a->extradata, b->extradata, a->extradata_size) == 0);
}
bool sameSideData(const AVCodecParameters* a, const AVCodecParameters* b) {
    if (a->nb_coded_side_data != b->nb_coded_side_data) return false;
    for (int i = 0; i < a->nb_coded_side_data; ++i) {
        const auto& x = a->coded_side_data[i];
        const auto* y = av_packet_side_data_get(b->coded_side_data, b->nb_coded_side_data, x.type);
        if (!y || x.size != y->size || (x.size && std::memcmp(x.data,y->data,x.size))) return false;
    }
    return true;
}
void compatible(const AVCodecParameters* a, const AVCodecParameters* b, bool video) {
    require(a->codec_id == b->codec_id, video ? L"these clips use different video codecs" : L"these clips use different audio codecs");
    if (video) {
        require(a->width == b->width && a->height == b->height, L"these clips have different resolutions");
        require(av_cmp_q(a->sample_aspect_ratio, b->sample_aspect_ratio) == 0 && a->format == b->format &&
            a->color_range == b->color_range && a->color_space == b->color_space && a->color_primaries == b->color_primaries &&
            a->color_trc == b->color_trc && a->chroma_location == b->chroma_location && a->field_order == b->field_order,
            L"these clips have different picture or colour settings");
    } else {
        require(a->sample_rate == b->sample_rate && av_channel_layout_compare(&a->ch_layout, &b->ch_layout) == 0,
            L"these clips have different audio sample rates or channel layouts");
        require(a->frame_size == b->frame_size && a->format == b->format, L"these clips have different audio settings");
    }
    require(a->profile == b->profile && a->level == b->level && sameExtra(a, b),
        video ? L"these clips have different video encoder settings" : L"these clips have different audio encoder settings");
    require(sameSideData(a,b), L"these clips have different stream metadata, such as rotation or HDR settings");
}
std::vector<int> streams(AVFormatContext* f) {
    std::vector<int> result;
    int audio = -1, audioCount = 0, mixes = 0;
    for (unsigned i = 0; i < f->nb_streams; ++i) {
        auto* s = f->streams[i];
        if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            require(!(s->disposition & AV_DISPOSITION_ATTACHED_PIC) && result.empty(), L"each clip must contain exactly one video stream");
            result.push_back(static_cast<int>(i));
        } else if (s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (audio < 0) audio = static_cast<int>(i);
            ++audioCount;
            for (const char* key : {"handler_name", "title", "name"}) {
                const auto* name = av_dict_get(s->metadata, key, nullptr, 0);
                if (name && _stricmp(name->value, "mix") == 0) { audio = static_cast<int>(i); ++mixes; break; }
            }
        }
    }
    require(result.size() == 1, L"a selected clip has no video");
    require(audioCount <= 1 || mixes == 1, L"a clip has several audio tracks but no unique combined mix");
    if (audio >= 0) result.push_back(audio);
    for (int index : result) require(f->streams[index]->time_base.num > 0 && f->streams[index]->time_base.den > 0,
        L"a clip has an invalid stream clock");
    auto* video = f->streams[result[0]]->codecpar;
    require(video->codec_id == AV_CODEC_ID_H264 || video->codec_id == AV_CODEC_ID_HEVC || video->codec_id == AV_CODEC_ID_AV1,
        L"only H.264, HEVC and AV1 video can be combined");
    require(video->video_delay == 0, L"clips with reordered video frames cannot be combined yet");
    if (audio >= 0) require(f->streams[audio]->codecpar->codec_id == AV_CODEC_ID_AAC, L"only AAC audio can be combined");
    return result;
}
// origin: where this clip's segment starts (its first picture, or the start of the one audio frame that crosses it).
struct Span { std::int64_t origin = 0, length = 0, picture = 0; AVRational pictureBase{0, 1}; };
// Kept audio ends after the first picture: priming and anything else wholly before it is dropped; the frame a
// keyframe trim cut through is kept, so each join's audio stays continuous. Compared exactly, in stream clocks.
bool keptAudio(const AVPacket* p, AVRational base, const Span& span) {
    return av_compare_ts(p->pts + p->duration, base, span.picture, span.pictureBase) > 0;
}
void addBytes(std::uint64_t& total, std::uint64_t bytes) {
    require(bytes <= (std::numeric_limits<std::uint64_t>::max)() - total, L"the combined clip would be too large");
    total += bytes;
}
// Microseconds are used only for planning; packet times are rescaled directly from each stream's clock.
std::int64_t micros(std::int64_t value, AVRational base) { return av_rescale_q(value, base, AV_TIME_BASE_Q); }
Span inspect(Input& input, const std::filesystem::path& path, const std::vector<int>& kept,
             const std::atomic<bool>& cancel, std::uint64_t& estimate) {
    Packet packet;
    bool video = false;
    Span span;
    // Audio can be demuxed before the first picture. Establish its origin first rather than
    // retaining a potentially unbounded list of leading audio packets or trusting stream metadata.
    {
        Input first; first.open(path, cancel);
        bool found = false;
        while (av_read_frame(first.format, packet.value) >= 0) {
            if (cancel.load()) return {};
            if (packet.value->stream_index == kept.front()) {
                require(packet.value->pts != AV_NOPTS_VALUE, L"a clip has unsupported or damaged packet timing");
                span.picture = packet.value->pts; span.pictureBase = first.format->streams[kept.front()]->time_base;
                span.origin = micros(span.picture, span.pictureBase);
                found = true; av_packet_unref(packet.value); break;
            }
            av_packet_unref(packet.value);
        }
        require(found, L"a selected clip has no usable video");
    }
    std::vector<std::int64_t> last(kept.size(), AV_NOPTS_VALUE);
    int status;
    while ((status = av_read_frame(input.format, packet.value)) >= 0) {
        if (cancel.load()) return {};
        const auto at = std::find(kept.begin(), kept.end(), packet.value->stream_index);
        if (at != kept.end()) {
            const auto k = static_cast<std::size_t>(at - kept.begin());
            auto* s = input.format->streams[*at];
            auto* p = packet.value;
            require(p->pts != AV_NOPTS_VALUE && p->dts != AV_NOPTS_VALUE && p->pts == p->dts && p->duration > 0 &&
                    p->pts <= (std::numeric_limits<std::int64_t>::max)() - p->duration &&
                    !(p->flags & AV_PKT_FLAG_CORRUPT), L"a clip has unsupported or damaged packet timing");
            require(last[k] == AV_NOPTS_VALUE || p->dts > last[k], L"a clip has non-increasing packet timestamps");
            last[k] = p->dts;
            // Limit both each input and the finished compilation to a safe, bounded timestamp range (seven days).
            const auto time = micros(p->pts, s->time_base), duration = micros(p->duration, s->time_base);
            require(time > -604800LL * AV_TIME_BASE && time < 604800LL * AV_TIME_BASE && duration > 0 && duration < 604800LL * AV_TIME_BASE,
                L"a clip's timestamps are outside the supported range");
            if (!k && !video) {
                require((p->flags & AV_PKT_FLAG_KEY) != 0, L"a clip does not start with a keyframe");
                video = true; // The pre-pass found this same first picture.
            }
            if (!k || keptAudio(p, s->time_base, span)) {
                if (k && time < span.origin) {
                    // Only a frame crossing the picture starts earlier.
                    require(crossingFrameFits(duration, s->codecpar->frame_size, s->codecpar->sample_rate),
                        L"a clip's audio starts long before its video; trim it before combining");
                    span.origin = time;
                }
                addBytes(estimate, static_cast<std::uint64_t>(p->size) + 128); // Payload and conservative MP4 sample-table overhead.
            }
            span.length = std::max(span.length, time + duration);
            std::size_t extraSize = 0;
            if (const auto* extra = av_packet_get_side_data(p, AV_PKT_DATA_NEW_EXTRADATA, &extraSize))
                require(extraSize == static_cast<std::size_t>(s->codecpar->extradata_size) &&
                    std::memcmp(extra, s->codecpar->extradata, extraSize) == 0, L"a clip changes encoder settings part way through");
        }
        av_packet_unref(packet.value);
    }
    require(status == AVERROR_EOF && video, L"a selected clip is incomplete or unreadable");
    span.length -= span.origin;
    require(span.length > 0, L"a selected clip has no usable duration");
    return span;
}
}

ExportResult combineClips(const std::vector<std::filesystem::path>& clips, std::atomic<float>& progress, const std::atomic<bool>& cancel,
                          const CombineStorage& storage) {
    ExportResult result;
    try {
        require(clips.size() >= 2, L"select at least two clips to combine");
        Locks locks;
        std::vector<Span> spans;
        Parameters video, audio;
        bool hasAudio = false;
        AVRational videoClock{}, audioClock{};
        std::int64_t total = 0;
        std::uint64_t estimate = 1'000'000; // Fixed MP4/container allowance, in addition to retained packet data.
        for (std::size_t i = 0; i < clips.size(); ++i) {
            if (cancel.load()) { result.cancelled = true; return result; }
            auto file = CreateFileW(clips[i].c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            require(file != INVALID_HANDLE_VALUE, L"a selected clip is missing, being written or unavailable");
            locks.files.push_back(file); // Inputs cannot change, be renamed or be deleted during the export.
            Input input; input.open(clips[i], cancel);
            const auto kept = streams(input.format);
            if (!i) {
                hasAudio = kept.size() == 2;
                require(avcodec_parameters_copy(video.value, input.format->streams[kept[0]]->codecpar) >= 0, L"could not check video settings");
                videoClock = input.format->streams[kept[0]]->time_base;
                if (hasAudio) {
                    require(avcodec_parameters_copy(audio.value, input.format->streams[kept[1]]->codecpar) >= 0, L"could not check audio settings");
                    audioClock = input.format->streams[kept[1]]->time_base;
                }
                for (int index : kept) {
                    const auto* parameters = input.format->streams[index]->codecpar;
                    addBytes(estimate, static_cast<std::uint64_t>(parameters->extradata_size));
                    for (int n = 0; n < parameters->nb_coded_side_data; ++n) addBytes(estimate, parameters->coded_side_data[n].size);
                }
            } else {
                require(hasAudio == (kept.size() == 2), L"some clips have audio and others do not");
                compatible(video.value, input.format->streams[kept[0]]->codecpar, true);
                if (hasAudio) compatible(audio.value, input.format->streams[kept[1]]->codecpar, false);
            }
            spans.push_back(inspect(input, clips[i], kept, cancel, estimate));
            if (cancel.load()) { result.cancelled = true; return result; }
            total += spans.back().length;
            require(total < 604800LL * AV_TIME_BASE, L"the combined clip would be longer than seven days");
            progress.store(0.25f * static_cast<float>(i + 1) / static_cast<float>(clips.size()));
        }
        const auto folder = std::filesystem::absolute(clips.front()).parent_path();
        ULARGE_INTEGER free{};
        require(GetDiskFreeSpaceExW(folder.c_str(), &free, nullptr, nullptr) != FALSE,
                L"free disk space could not be checked; no combined clip was written");
        std::uint64_t used = 0;
        if (storage.folderLimit) {
            // Skip only vanished files. Other failures must not turn an unknown usage into permission to exceed the limit.
            std::error_code error;
            for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
                if (cancel.load()) { result.cancelled = true; return result; }
                std::error_code entryError;
                if (!isClipFile(it->path())) continue;
                const bool regular = it->is_regular_file(entryError);
                if (entryError == std::errc::no_such_file_or_directory) continue;
                require(!entryError, L"the clip folder's storage usage could not be checked; no combined clip was written");
                if (!regular) continue;
                const auto size = it->file_size(entryError);
                if (entryError == std::errc::no_such_file_or_directory) continue;
                require(!entryError, L"the clip folder's storage usage could not be checked; no combined clip was written");
                addBytes(used, size);
            }
            require(!error, L"the clip folder's storage usage could not be checked; no combined clip was written");
        }
        if (const auto* problem = combineStorageProblem(estimate, free.QuadPart, used, storage.folderLimit, storage.reserve, storage.refuseOverLimit))
            throw Problem{problem};
        result.overLimit = combineOverLimit(estimate, used, storage.folderLimit);
        Output output;
        GUID id{}; wchar_t guid[40]{};
        require(SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id, guid, 40), L"could not create an export name");
        output.partial = clips.front().parent_path() / (L"refarter-export-" + std::wstring(guid) + L".part");
        // Reserve our unique temporary name before FFmpeg opens it. No existing file is truncated.
        auto reservation = CreateFileW(output.partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (reservation == INVALID_HANDLE_VALUE) { output.partial.clear(); throw Problem{L"the clip folder cannot be written to"}; }
        CloseHandle(reservation);
        require(avformat_alloc_output_context2(&output.format, nullptr, "mp4", utf8(output.partial.wstring()).c_str()) >= 0, L"could not create the combined clip");
        for (int i = 0; i < (hasAudio ? 2 : 1); ++i) {
            auto* s = avformat_new_stream(output.format, nullptr);
            require(s && avcodec_parameters_copy(s->codecpar, i ? audio.value : video.value) >= 0, L"could not create output streams");
            s->codecpar->codec_tag = 0;
            s->time_base = i ? audioClock : videoClock;
            s->disposition = AV_DISPOSITION_DEFAULT;
            if (i) { av_dict_set(&s->metadata, "handler_name", "mix", 0); av_dict_set(&s->metadata, "title", "mix", 0); }
        }
        require(avio_open(&output.format->pb, utf8(output.partial.wstring()).c_str(), AVIO_FLAG_WRITE) >= 0, L"the clip folder cannot be written to");
        // Keep the index at the end: avoids copying a potentially multi-gigabyte output for faststart.
        require(avformat_write_header(output.format, nullptr) >= 0, L"could not write the combined clip's header");
        Packet packet;
        std::vector<std::int64_t> last(hasAudio ? 2 : 1, AV_NOPTS_VALUE);
        std::int64_t offset = 0;
        for (std::size_t i = 0; i < clips.size(); ++i) {
            Input input; input.open(clips[i], cancel);
            const auto kept = streams(input.format);
            int status;
            while ((status = av_read_frame(input.format, packet.value)) >= 0) {
                if (cancel.load()) { result.cancelled = true; return result; }
                const auto at = std::find(kept.begin(), kept.end(), packet.value->stream_index);
                if (at != kept.end()) {
                    const auto k = static_cast<int>(at - kept.begin());
                    auto* in = input.format->streams[*at]; auto* out = output.format->streams[k]; auto* p = packet.value;
                    const auto end = micros(p->pts + p->duration, in->time_base);
                    // The segment starts at its origin, so a kept crossing frame begins exactly where the previous
                    // clip ended and the picture follows it by under one audio frame.
                    if (!k || keptAudio(p, in->time_base, spans[i])) {
                        av_packet_rescale_ts(p, in->time_base, out->time_base);
                        const auto shift = av_rescale_q(offset - spans[i].origin, AV_TIME_BASE_Q, out->time_base);
                        p->pts += shift; p->dts += shift;
                        require(last[k] == AV_NOPTS_VALUE || p->dts > last[k], L"these clips cannot be joined without changing their timing");
                        last[k] = p->dts; p->stream_index = k; p->pos = -1;
                        require(av_interleaved_write_frame(output.format, p) >= 0, L"the combined clip could not be written; check free disk space");
                    }
                    progress.store(0.25f + 0.75f * static_cast<float>(offset + std::clamp(end - spans[i].origin, 0LL, spans[i].length)) / static_cast<float>(total));
                }
                av_packet_unref(packet.value);
            }
            require(status == AVERROR_EOF, L"a selected clip could not be read completely");
            offset += spans[i].length;
        }
        require(av_write_trailer(output.format) >= 0, L"the combined clip could not be finished; check free disk space");
        require(avio_closep(&output.format->pb) >= 0, L"the combined clip could not be finished");
        if (cancel.load()) { result.cancelled = true; return result; }
        SYSTEMTIME now{}; GetLocalTime(&now); wchar_t stem[80]{};
        swprintf_s(stem, L"combined clips %04u-%02u-%02u %02u-%02u-%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        for (int n = 1; n <= 1000; ++n) {
            const auto path = clips.front().parent_path() / (std::wstring(stem) + (n == 1 ? L"" : L" (" + std::to_wstring(n) + L")") + L".mp4");
            if (MoveFileW(output.partial.c_str(), path.c_str())) { output.partial.clear(); result.path = path; progress.store(1); return result; }
            const auto error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
                throw Problem{L"the combined clip could not be published (windows error " + std::to_wstring(error) + L")"};
        }
        throw Problem{L"no free name for the combined clip"};
    } catch (const Problem& problem) { result.error = problem.text; }
      catch (const std::exception&) { result.error = L"not enough memory or a file could not be accessed"; }
    if (cancel.load()) { result.error.clear(); result.cancelled = true; }
    return result;
}
}
