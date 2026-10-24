#include "player/clip_trim.h"
#include "player/mix_encoder.h"
#include "text.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <vector>

namespace refarter::player {

namespace {

struct Packets {
    std::map<std::size_t, AVPacket*> waiting;
    ~Packets() { for (auto& [id, packet] : waiting) av_packet_free(&packet); }
};

using detail::MixEncoder;
} // namespace

std::filesystem::path trimmedPath(const std::filesystem::path& source) {
    const auto folder = source.parent_path();
    const auto stem = source.stem().wstring() + L" trim";
    auto path = folder / (stem + L".mp4");
    std::error_code error;
    for (int n = 2; std::filesystem::exists(path, error) && n < 1000; ++n)
        path = folder / (stem + L" (" + std::to_wstring(n) + L").mp4");
    return path;
}

TrimResult trimClip(const std::filesystem::path& source, TrimRange range, std::atomic<float>& progress,
                    const std::atomic<bool>& cancel, const TrimAudio& audio) {
    TrimResult result;
    result.path = trimmedPath(source);
    // Written under a temporary name the library ignores, then renamed.
    const auto partial = std::filesystem::path(result.path.wstring() + L".part");
    AVFormatContext* input = nullptr;
    AVFormatContext* output = nullptr;
    bool opened = false;
    const auto fail = [&](const wchar_t* message) {
        if (output) {
            if (opened) avio_closep(&output->pb);
            avformat_free_context(output);
        }
        avformat_close_input(&input);
        std::error_code ignored;
        std::filesystem::remove(partial, ignored);
        result.ok = false;
        result.error = message;
        return result;
    };
    if (avformat_open_input(&input, utf8(source.wstring()).c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(input, nullptr) < 0) return fail(L"the clip could not be read.");
    const int video = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video < 0) return fail(L"the clip has no video.");
    // The name is kept for faststart, which reopens the file to move the index to the front.
    if (avformat_alloc_output_context2(&output, nullptr, "mp4", utf8(partial.wstring()).c_str()) < 0) return fail(L"the trimmed clip could not be created.");

    const auto* videoStream = input->streams[video];
    const double origin = videoStream->start_time != AV_NOPTS_VALUE ? videoStream->start_time * av_q2d(videoStream->time_base) : 0;

    // Copy the video and the kept audio tracks exactly; streams keep their order and flags.
    // Leaving tracks out replaces the mix with a new one of the kept tracks, first and default.
    std::vector<int> kept; // Input streams of the kept audio tracks.
    std::vector<std::string> keptNames; // Their names to write, where known.
    std::vector<int> mixed; // Of those, the streams the new mix sums.
    for (unsigned i = 0, track = 0; i < input->nb_streams; ++i) {
        const auto* parameters = input->streams[i]->codecpar;
        if (parameters->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        // Track numbers are the player's (MediaFile::open): only audio streams that have a decoder are counted.
        // One without is never a track the player named; it is copied with "every track" and dropped otherwise.
        const bool decodable = avcodec_find_decoder(parameters->codec_id) != nullptr;
        const int trackIndex = decodable ? static_cast<int>(track++) : -1;
        const auto at = std::find(audio.keep.begin(), audio.keep.end(), trackIndex) - audio.keep.begin();
        if (audio.none || (!audio.keep.empty() && at == std::ssize(audio.keep))) continue;
        kept.push_back(static_cast<int>(i));
        if (decodable && std::find(audio.apart.begin(), audio.apart.end(), trackIndex) == audio.apart.end()) mixed.push_back(static_cast<int>(i));
        keptNames.push_back(at < std::ssize(audio.names) ? audio.names[at] : std::string{});
    }
    const bool chosen = audio.changesAudio();
    if (chosen && !audio.none && kept.empty()) return fail(L"the chosen audio tracks are not in this clip.");
    std::vector<int> map(input->nb_streams, -1);
    const auto copy = [&](int i) {
        const auto* in = input->streams[i];
        auto* out = avformat_new_stream(output, nullptr);
        if (!out || avcodec_parameters_copy(out->codecpar, in->codecpar) < 0) return false;
        out->codecpar->codec_tag = 0;
        out->time_base = in->time_base;
        out->disposition = in->disposition;
        av_dict_copy(&out->metadata, in->metadata, 0);
        map[i] = out->index;
        return true;
    };
    if (!copy(video)) return fail(L"the trimmed clip could not be created.");
    MixEncoder mixer;
    AVStream* mixStream = nullptr;
    const bool mixing = chosen && audio.mix && mixed.size() > 1;
    if (mixing) {
        auto* out = mixStream = avformat_new_stream(output, nullptr);
        if (!out || !mixer.open(input, mixed, output, out, origin, range.start, range.length()))
            return fail(L"the new audio mix could not be prepared.");
        out->disposition = AV_DISPOSITION_DEFAULT;
        av_dict_set(&out->metadata, "handler_name", "mix", 0);
    }
    for (std::size_t k = 0; k < kept.size(); ++k) {
        const int i = kept[k];
        if (!copy(i)) return fail(L"the trimmed clip could not be created.");
        auto* out = output->streams[map[i]];
        // The default track is the new mix, or the first kept track on its own.
        if (chosen) out->disposition = (out->disposition & ~AV_DISPOSITION_DEFAULT) |
            (!mixing && i == kept.front() ? AV_DISPOSITION_DEFAULT : 0);
        if (!keptNames[k].empty()) av_dict_set(&out->metadata, "handler_name", keptNames[k].c_str(), 0);
    }
    if (avio_open(&output->pb, utf8(partial.wstring()).c_str(), AVIO_FLAG_WRITE) < 0)
        return fail(L"the clip folder cannot be written to.");
    opened = true;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "movflags", "+faststart", 0); // Index first: plays while uploading or streaming.
    const int header = avformat_write_header(output, &options);
    av_dict_free(&options);
    if (header < 0) return fail(L"the trimmed clip could not be created.");

    const auto at = [&](int stream, double time) {
        return static_cast<long long>(std::llround((time + origin) / av_q2d(input->streams[stream]->time_base)));
    };
    if (av_seek_frame(input, video, at(video, range.start), AVSEEK_FLAG_BACKWARD) < 0) return fail(L"the clip could not be read.");

    VideoCut cut(range.start, range.end);
    Packets packets;
    std::vector<bool> finished(input->nb_streams, true);
    for (unsigned i = 0; i < input->nb_streams; ++i) finished[i] = map[i] < 0;
    std::size_t sequence = 0;
    bool written = false, failed = false;
    const auto write = [&](AVPacket* packet) {
        const int stream = packet->stream_index;
        const auto shift = at(stream, range.start) - at(stream, 0); // The copied streams keep the source's origin (the mix is moved to it below).
        if (packet->pts != AV_NOPTS_VALUE) packet->pts -= shift;
        if (packet->dts != AV_NOPTS_VALUE) packet->dts -= shift;
        av_packet_rescale_ts(packet, input->streams[stream]->time_base, output->streams[map[stream]]->time_base);
        packet->stream_index = map[stream];
        packet->pos = -1;
        if (av_interleaved_write_frame(output, packet) < 0) failed = true;
        written = true;
    };
    // The mix counts samples from the cut, while the copied streams start at the source's origin: so does the mix.
    const auto writeMix = [&](AVPacket* encoded) {
        const auto offset = av_rescale_q(std::llround(origin * MixEncoder::rate), AVRational{1, MixEncoder::rate}, mixStream->time_base);
        if (encoded->pts != AV_NOPTS_VALUE) encoded->pts += offset;
        if (encoded->dts != AV_NOPTS_VALUE) encoded->dts += offset;
        if (av_interleaved_write_frame(output, encoded) < 0) failed = true;
        return !failed;
    };
    AVPacket* packet = av_packet_alloc();
    while (!failed && !cancel && av_read_frame(input, packet) >= 0) {
        const int stream = packet->stream_index;
        if (finished[stream] || packet->pts == AV_NOPTS_VALUE) { av_packet_unref(packet); continue; }
        const double time = packet->pts * av_q2d(input->streams[stream]->time_base) - origin;
        if (stream == video) {
            const auto id = sequence++;
            packets.waiting[id] = av_packet_clone(packet);
            for (const auto keep : cut.push(id, time, (packet->flags & AV_PKT_FLAG_KEY) != 0)) {
                write(packets.waiting[keep]);
                av_packet_free(&packets.waiting[keep]);
                packets.waiting.erase(keep);
            }
            const auto& held = cut.held();
            if (packets.waiting.count(id) && std::find(held.begin(), held.end(), id) == held.end()) {
                av_packet_free(&packets.waiting[id]);
                packets.waiting.erase(id);
            }
            if (cut.done()) finished[stream] = true;
            progress = static_cast<float>(std::clamp((time - range.start) / std::max(0.001, range.length()), 0.0, 1.0));
        } else if (time >= range.end) {
            finished[stream] = true;
        } else {
            // The mix decodes from shortly before the cut, so its first sound is complete.
            if (mixing && time >= range.start - 0.5 && !mixer.feed(stream, packet, writeMix)) failed = true;
            if (time >= range.start - 1e-4) write(packet);
        }
        av_packet_unref(packet);
        if (std::all_of(finished.begin(), finished.end(), [](bool done) { return done; })) break;
    }
    av_packet_free(&packet);
    if (cancel) return fail(L"trimming was cancelled.");
    if (mixing && !failed && !mixer.finish(writeMix)) failed = true;
    if (failed || !written) return fail(L"the trimmed clip could not be written.");
    if (av_write_trailer(output) < 0) return fail(L"the trimmed clip could not be finished.");
    avio_closep(&output->pb);
    opened = false;
    avformat_free_context(output);
    output = nullptr;
    avformat_close_input(&input);
    result.identity = temporaryClipIdentity(partial);
    if (!MoveFileExW(partial.c_str(), result.path.c_str(), MOVEFILE_WRITE_THROUGH)) return fail(L"the trimmed clip could not be saved.");
    progress = 1.f;
    result.ok = true;
    return result;
}

} // namespace refarter::player
