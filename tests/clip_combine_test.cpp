#include "player/clip_combine.h"
#include "player/combine_storage.h"
#include "compression_job.h"
#include <windows.h>
#include <objbase.h>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
extern "C" {
#include <libavformat/avformat.h>
}

namespace {
int failures = 0;
void check(bool yes, const char* text) { if (!yes) { std::cerr << text << '\n'; ++failures; } }
struct Media {
    AVFormatContext* f = nullptr;
    explicit Media(const std::filesystem::path& path) {
        const auto text = path.u8string();
        if (avformat_open_input(&f, reinterpret_cast<const char*>(text.c_str()), nullptr, nullptr) >= 0)
            avformat_find_stream_info(f, nullptr);
    }
    ~Media() { avformat_close_input(&f); }
};
using Bytes = std::vector<std::vector<unsigned char>>;
Bytes payloads(const std::filesystem::path& path, AVMediaType type) {
    Media media(path); Bytes bytes;
    if (!media.f) return bytes;
    AVPacket* p = av_packet_alloc();
    while (av_read_frame(media.f,p) >= 0) {
        if (media.f->streams[p->stream_index]->codecpar->codec_type == type &&
            (type == AVMEDIA_TYPE_VIDEO || p->pts + p->duration > 0)) bytes.emplace_back(p->data,p->data+p->size);
        av_packet_unref(p);
    }
    av_packet_free(&p); return bytes;
}
bool monotonic(const std::filesystem::path& path) {
    Media media(path); if (!media.f) return false;
    std::vector<int64_t> last(media.f->nb_streams,AV_NOPTS_VALUE);
    AVPacket* p = av_packet_alloc(); bool ok = true;
    while (av_read_frame(media.f,p) >= 0) {
        auto& before = last[p->stream_index];
        ok &= p->dts != AV_NOPTS_VALUE && (before == AV_NOPTS_VALUE || p->dts > before) && p->duration > 0;
        before = p->dts; av_packet_unref(p);
    }
    av_packet_free(&p); return ok;
}
bool noPartial(const std::filesystem::path& folder) {
    for (const auto& entry : std::filesystem::directory_iterator(folder)) if (entry.path().extension() == L".part") return false;
    return true;
}
// Audio packets that start at or after a clip's first picture (to find the one frame that crosses it).
Bytes audioFromPicture(const std::filesystem::path& path) {
    Media media(path); Bytes bytes;
    if (!media.f) return bytes;
    AVPacket* p = av_packet_alloc();
    std::int64_t origin = AV_NOPTS_VALUE;
    while (origin == AV_NOPTS_VALUE && av_read_frame(media.f,p) >= 0) {
        const auto* s = media.f->streams[p->stream_index];
        if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) origin = av_rescale_q(p->pts,s->time_base,AV_TIME_BASE_Q);
        av_packet_unref(p);
    }
    av_seek_frame(media.f,-1,INT64_MIN,AVSEEK_FLAG_BACKWARD);
    while (origin != AV_NOPTS_VALUE && av_read_frame(media.f,p) >= 0) {
        const auto* s = media.f->streams[p->stream_index];
        if (s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && av_rescale_q(p->pts,s->time_base,AV_TIME_BASE_Q) >= origin)
            bytes.emplace_back(p->data,p->data+p->size);
        av_packet_unref(p);
    }
    av_packet_free(&p); return bytes;
}
// A real MP4 whose audio is wholly before its delayed first picture. Packet bytes stay unchanged.
bool leadingAudio(const std::filesystem::path& source, const std::filesystem::path& path, std::int64_t videoDelay = 2'000'000) {
    Media in(source); if (!in.f) return false;
    AVFormatContext* out = nullptr;
    const auto name = path.u8string();
    if (avformat_alloc_output_context2(&out,nullptr,"mp4",reinterpret_cast<const char*>(name.c_str())) < 0) return false;
    bool ok = true;
    for (unsigned i=0;i<in.f->nb_streams && ok;++i) {
        auto* stream = avformat_new_stream(out,nullptr);
        ok = stream && avcodec_parameters_copy(stream->codecpar,in.f->streams[i]->codecpar) >= 0;
        if (ok) { stream->time_base=in.f->streams[i]->time_base; stream->codecpar->codec_tag=0; }
    }
    if (ok) ok = avio_open(&out->pb,reinterpret_cast<const char*>(name.c_str()),AVIO_FLAG_WRITE) >= 0;
    if (ok) ok = avformat_write_header(out,nullptr) >= 0;
    AVPacket* p = av_packet_alloc();
    if (!p) ok=false;
    while (ok && av_read_frame(in.f,p) >= 0) {
        const auto* stream=in.f->streams[p->stream_index];
        if (stream->codecpar->codec_type==AVMEDIA_TYPE_VIDEO) {
            const auto shift=av_rescale_q(videoDelay,AVRational{1,1'000'000},stream->time_base);
            p->pts+=shift; p->dts+=shift;
        }
        av_packet_rescale_ts(p,stream->time_base,out->streams[p->stream_index]->time_base);
        p->pos=-1;
        ok=av_interleaved_write_frame(out,p) >= 0;
        av_packet_unref(p);
    }
    av_packet_free(&p);
    if (ok) ok=av_write_trailer(out) >= 0;
    if (out->pb) avio_closep(&out->pb);
    avformat_free_context(out); return ok;
}
}
int main() {
    av_log_set_level(AV_LOG_QUIET);
    GUID id{}; CoCreateGuid(&id); wchar_t guid[40]{}; StringFromGUID2(id,guid,40);
    const auto root = std::filesystem::temp_directory_path() / (L"refarter-combine-test-" + std::wstring(guid));
    std::filesystem::create_directories(root);
    const std::filesystem::path fixtures(REFARTER_COMBINE_FIXTURES);
    for (const auto& entry : std::filesystem::directory_iterator(fixtures))
        if (entry.path().extension() == L".mp4") std::filesystem::copy_file(entry.path(),root/entry.path().filename());
    const auto red = root/L"red.mp4", blue = root/L"blue.mp4", wider = root/L"wider.mp4";
    const auto redSize = std::filesystem::file_size(red);
    std::atomic<float> progress{0}; std::atomic<bool> cancel{false};
    auto joined = refarter::player::combineClips({red,blue},progress,cancel);
    if (!joined.error.empty()) std::wcerr << L"combine: " << joined.error << L'\n';
    check(joined.error.empty() && !joined.path.empty(), "compatible clips combine");
    if (!joined.path.empty()) {
        auto expected = payloads(red,AVMEDIA_TYPE_VIDEO), tail = payloads(blue,AVMEDIA_TYPE_VIDEO);
        expected.insert(expected.end(),tail.begin(),tail.end());
        check(payloads(joined.path,AVMEDIA_TYPE_VIDEO) == expected, "video bytes and requested order preserved");
        expected = payloads(red,AVMEDIA_TYPE_AUDIO); tail = payloads(blue,AVMEDIA_TYPE_AUDIO);
        expected.insert(expected.end(),tail.begin(),tail.end());
        check(payloads(joined.path,AVMEDIA_TYPE_AUDIO) == expected, "combined audio bytes preserved without duplicate sources");
        check(monotonic(joined.path), "joined timestamps strictly increase");
        Media m(joined.path);
        check(m.f && m.f->nb_streams == 2 && m.f->duration > 1900000 && m.f->duration < 2100000, "two-second output has video and one audio track");
        check(progress.load() == 1.f, "completion reports full progress");
    }
    auto reverse = refarter::player::combineClips({blue,red},progress,cancel);
    check(reverse.error.empty() && !reverse.path.empty() && reverse.path != joined.path, "second export does not overwrite first");
    if (!reverse.path.empty()) {
        const auto bytes = payloads(reverse.path,AVMEDIA_TYPE_VIDEO), first = payloads(blue,AVMEDIA_TYPE_VIDEO);
        check(bytes.size() == 4 && !first.empty() && bytes.front() == first.front(), "reordering changes the first clip");
    }
    using refarter::player::combineStorageProblem;
    check(combineStorageProblem(100,199,0,0,100)!=nullptr, "disk preflight preserves the replay reserve");
    check(!combineStorageProblem(100,200,0,0,100), "exact disk budget fits");
    check(combineStorageProblem(UINT64_MAX,UINT64_MAX,0,0,1)!=nullptr, "disk budget cannot overflow");
    check(combineStorageProblem(100,1000,950,1000,100)!=nullptr, "export respects the folder limit");
    check(!combineStorageProblem(100,1000,900,1000,100), "exact folder budget fits");
    check(!combineStorageProblem(100,1000,950,1000,100,false), "warn and save lets an export pass the folder limit");
    check(combineStorageProblem(100,199,0,1,100,false)!=nullptr, "a full drive refuses whatever the storage setting");
    using refarter::player::crossingFrameFits;
    check(crossingFrameFits(21'334,1024,48000) && crossingFrameFits(128'000,1024,8000), "one AAC frame crosses at 48 and 8 kHz");
    check(crossingFrameFits(21'334,0,48000), "an unknown frame size assumes AAC-LC");
    check(!crossingFrameFits(40'000,1024,48000) && !crossingFrameFits(5'000'000,1024,8000), "a longer crossing packet is refused");
    check(!crossingFrameFits(1,1024,0), "no sample rate refuses");
    check(!crossingFrameFits(0,1024,48000) && !crossingFrameFits(-1,1024,48000), "non-positive crossing durations refuse");
    check(!crossingFrameFits(INT64_MAX,1024,48000) && !crossingFrameFits(INT64_MAX,INT_MAX,INT_MAX),
          "extreme crossing durations and stream values refuse without arithmetic overflow");
    check(crossingFrameFits(32'000,1024,48000) && !crossingFrameFits(32'001,1024,48000),
          "crossing-frame allowance has an exact inclusive boundary");
    auto limited=refarter::player::combineClips({red,blue},progress,cancel,{1,0,true});
    check(limited.path.empty() && limited.error.find(L"storage limit")!=std::wstring::npos && noPartial(root), "folder limit refuses before creating output");
    // A directory sharing lock prevents enumeration without changing permissions or locking its video files.
    const HANDLE folderLock = CreateFileW(root.c_str(),FILE_LIST_DIRECTORY,0,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    check(folderLock != INVALID_HANDLE_VALUE, "folder enumeration can be held for the storage check");
    if (folderLock != INVALID_HANDLE_VALUE) {
        const auto unreadable=refarter::player::combineClips({red,blue},progress,cancel,{UINT64_MAX,0,true});
        CloseHandle(folderLock);
        check(unreadable.path.empty() && unreadable.error.find(L"storage usage could not be checked")!=std::wstring::npos && noPartial(root),
              "failed folder enumeration refuses instead of treating storage usage as zero");
    }
    auto warned=refarter::player::combineClips({red,blue},progress,cancel,{1,0,false});
    check(warned.error.empty() && !warned.path.empty() && warned.overLimit, "warn and save combines past the limit and says so");
    if (!warned.path.empty()) std::filesystem::remove(warned.path);
    auto lowDisk=refarter::player::combineClips({red,blue},progress,cancel,{0,UINT64_MAX,true});
    check(lowDisk.path.empty() && lowDisk.error.find(L"free disk space")!=std::wstring::npos && noPartial(root), "insufficient space refuses before creating output");
    const auto leading=root/L"leading-audio.mp4";
    check(leadingAudio(red,leading), "delayed-video fixture created");
    const auto leadingJoin=refarter::player::combineClips({leading,leading},progress,cancel);
    check(leadingJoin.error.empty() && !leadingJoin.path.empty(), "audio wholly before non-zero video origin is omitted, not refused");
    if (!leadingJoin.path.empty()) check(payloads(leadingJoin.path,AVMEDIA_TYPE_AUDIO).empty(), "omitted leading audio is not shifted into the output");
    const auto crossing=root/L"crossing-audio.mp4";
    check(leadingAudio(red,crossing,10'000), "crossing-audio fixture created");
    const auto crossingJoin=refarter::player::combineClips({crossing,crossing},progress,cancel);
    check(crossingJoin.error.empty() && !crossingJoin.path.empty(), "audio crossing the first picture (a trimmed clip) combines");
    if (!crossingJoin.path.empty()) {
        const auto inside = audioFromPicture(crossing);
        auto expected = payloads(crossing,AVMEDIA_TYPE_AUDIO); const auto tail = expected;
        check(inside.size() + 1 == tail.size(), "the fixture really has one frame across its first picture");
        expected.insert(expected.end(),tail.begin(),tail.end());
        check(payloads(crossingJoin.path,AVMEDIA_TYPE_AUDIO)==expected && monotonic(crossingJoin.path),
              "the crossing audio frame is kept, so the join's audio stays continuous");
        Media m(crossingJoin.path);
        check(m.f && m.f->duration > 1900000 && m.f->duration < 2200000, "the crossing frame does not add or lose a clip's worth of time");
    }
    auto mismatch = refarter::player::combineClips({red,wider},progress,cancel);
    check(mismatch.path.empty() && mismatch.error.find(L"resolutions") != std::wstring::npos, "different resolutions refused before publishing");
    mismatch = refarter::player::combineClips({red,root/L"audio-44100.mp4"},progress,cancel);
    check(mismatch.path.empty() && mismatch.error.find(L"sample rates") != std::wstring::npos, "different audio clocks refused");
    mismatch = refarter::player::combineClips({red,root/L"av1.mp4"},progress,cancel);
    check(mismatch.path.empty() && mismatch.error.find(L"video codecs") != std::wstring::npos, "different video codecs refused");
    mismatch = refarter::player::combineClips({red,root/L"rotated.mp4"},progress,cancel);
    check(mismatch.path.empty() && mismatch.error.find(L"metadata") != std::wstring::npos, "different display rotation refused");
    mismatch = refarter::player::combineClips({red,root/L"no-mix.mp4"},progress,cancel);
    check(mismatch.path.empty() && mismatch.error.find(L"combined mix") != std::wstring::npos, "ambiguous audio never silently loses sources");
    mismatch = refarter::player::combineClips({red,root/L"video-only.mp4"},progress,cancel);
    check(mismatch.path.empty() && mismatch.error.find(L"others do not") != std::wstring::npos, "missing audio never silently drops sound");
    auto videoOnly = refarter::player::combineClips({root/L"video-only.mp4",root/L"video-only.mp4"},progress,cancel);
    check(videoOnly.error.empty() && !videoOnly.path.empty(), "two video-only clips combine");
    auto av1 = refarter::player::combineClips({root/L"av1.mp4",root/L"av1.mp4"},progress,cancel);
    check(av1.error.empty() && !av1.path.empty() && monotonic(av1.path), "compatible AV1 clips combine");
    auto separate = refarter::player::combineClips({red,root/L"separate-tracks.mp4"},progress,cancel);
    check(separate.error.empty() && !separate.path.empty(), "matching mixes combine despite different source-track counts");
    if (!separate.path.empty()) {
        auto expected = payloads(red,AVMEDIA_TYPE_AUDIO); const auto tail=expected;
        expected.insert(expected.end(),tail.begin(),tail.end());
        Media m(separate.path);
        check(m.f && m.f->nb_streams==2 && payloads(separate.path,AVMEDIA_TYPE_AUDIO)==expected, "sources are not stacked on the stored mix");
    }
    { std::ofstream bad(root/L"bad.mp4"); bad << "not a video"; }
    check(!refarter::player::combineClips({red,root/L"bad.mp4"},progress,cancel).error.empty(), "corrupt input refused");
    check(!refarter::player::combineClips({red,root/L"missing.mp4"},progress,cancel).error.empty(), "missing input refused");
    check(!refarter::player::combineClips({red},progress,cancel).error.empty(), "single input refused");
    const HANDLE writer = CreateFileW(red.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    check(writer != INVALID_HANDLE_VALUE, "fixture can be held by a writer");
    if (writer != INVALID_HANDLE_VALUE) {
        auto held = refarter::player::combineClips({red,blue},progress,cancel);
        check(held.path.empty() && !held.error.empty(), "input being written is refused");
        CloseHandle(writer);
    }
    cancel.store(true);
    auto stopped = refarter::player::combineClips({red,blue},progress,cancel);
    check(stopped.cancelled && stopped.path.empty(), "cancel before work publishes nothing");
    cancel.store(false); progress.store(0);
    std::vector<std::filesystem::path> many(120,red);
    auto running = std::async(std::launch::async,[&] { return refarter::player::combineClips(many,progress,cancel); });
    while (progress.load() <= 0.26f && running.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {}
    const HANDLE edit = CreateFileW(red.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    check(edit == INVALID_HANDLE_VALUE, "all active inputs refuse external writes");
    if (edit != INVALID_HANDLE_VALUE) CloseHandle(edit);
    cancel.store(true); stopped = running.get();
    check(stopped.cancelled && stopped.path.empty(), "cancel during packet copy publishes nothing");
    check(noPartial(root), "errors and cancellation remove owned partials");
    check(std::filesystem::file_size(red) == redSize && payloads(red,AVMEDIA_TYPE_VIDEO).size() == 2, "originals untouched");
    // The shared worker has the same completion and cancellation lifecycle as compression.
    refarter::CompressionJob worker;
    check(!worker.startCombine({{},red},{}) && !worker.startCombine({red,{}},{}) && !worker.active(), "empty inputs never leave an unobservable export worker");
    check(worker.startCombine({red,blue},{}), "export worker starts");
    std::optional<refarter::player::ExportResult> result;
    for (int i=0;i<1000 && !result;++i) { result=worker.poll(); if (!result) std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    check(result && result->error.empty() && !worker.active(), "export worker publishes completion and releases its slot");
    worker.stop();
    // Only this test's unique temporary directory is removed.
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
