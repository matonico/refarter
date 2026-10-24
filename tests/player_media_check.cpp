// Manual check for the built-in player's media path; not part of CTest.
// Usage: player_media_check <clip> <scratch-folder>
// Copies the clip into the scratch folder, then: decodes and seeks on the GPU
// and in software, renames a copy while it is open, trims a middle part and
// proves every copied video and audio packet is byte-identical to the original
// (no re-encoding, same quality).
#include "capture/gpu.h"
#include "player/clip_trim.h"
#include "player/media.h"
#include "player/renderer.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <windows.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

using namespace refarter;
using Clock = std::chrono::steady_clock;

namespace {
int failures = 0;
void check(bool passes, const char* what) {
    std::printf("%s %s\n", passes ? "ok  " : "FAIL", what);
    if (!passes) ++failures;
}
double ms(Clock::time_point since) { return std::chrono::duration<double, std::milli>(Clock::now() - since).count(); }

std::string utf8(const std::filesystem::path& path) {
    const auto text = path.wstring();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

// Draws the picture at `time` through the player's own converter and returns its mean
// BGRA brightness (0-255), or -1 when it could not be drawn. Software pictures (also
// when a GPU decoder falls back) must look like GPU ones, not black.
double drawnBrightness(const std::filesystem::path& clip, ID3D11Device* device, bool software, double time) {
    player::MediaFile file;
    if (!file.open(clip, {device, false, software})) { std::printf("     open failed\n"); return -1; }
    auto picture = file.seek(time);
    if (!picture) { std::printf("     no picture\n"); return -1; }
    constexpr UINT width = 640, height = 360;
    auto target = player::createPictureTexture(device, width, height);
    player::VideoConverter converter(device);
    if (!target || !converter.convert(picture.frame.get(), target.get())) { std::printf("     convert failed fmt %d\n", picture.frame->format); return -1; }
    D3D11_TEXTURE2D_DESC desc{};
    target->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.put()))) return -1;
    winrt::com_ptr<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.put());
    context->CopyResource(staging.get(), target.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return -1;
    double sum = 0;
    for (UINT y = 0; y < height; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
        for (UINT x = 0; x < width; ++x) sum += row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2];
    }
    context->Unmap(staging.get(), 0);
    return sum / (3.0 * width * height);
}

void drawing(const std::filesystem::path& clip, ID3D11Device* device) {
    std::printf("\n[drawing]\n");
    const double gpu = drawnBrightness(clip, device, false, 1.0);
    const double soft = drawnBrightness(clip, device, true, 1.0);
    std::printf("     mean brightness: default decoder %.1f, software %.1f\n", gpu, soft);
    check(gpu > 8, "the picture drawn from the default decoder is not black");
    check(soft > 8, "the picture drawn from a software decoder is not black");
    check(gpu > 0 && soft > 0 && std::abs(gpu - soft) < 6, "software and default pictures look alike");
}

// The player renames the clip it plays without closing it: the file must move while open and keep decoding.
void renameWhileOpen(const std::filesystem::path& clip) {
    std::printf("\n[rename while open]\n");
    const auto opened = clip.parent_path() / (L"open " + clip.filename().wstring());
    const auto renamed = clip.parent_path() / (L"renamed " + clip.filename().wstring());
    std::filesystem::remove(renamed);
    std::filesystem::copy_file(clip, opened, std::filesystem::copy_options::overwrite_existing);
    {
        player::MediaFile file;
        const bool open = file.open(opened, {nullptr, true, true});
        check(open, "opens");
        if (!open) return;
        check(MoveFileExW(opened.c_str(), renamed.c_str(), 0) != FALSE, "renamed while open");
        int frames = 0;
        while (frames < 30 && file.next()) ++frames;
        float samples[2 * 1024];
        const bool sounds = file.audioTracks() == 0 || file.audio(samples, 1024) > 0;
        check(frames == 30 && file.seek(file.duration() / 2) && sounds, "decodes and seeks after the rename");
    }
    std::error_code ignored;
    std::filesystem::remove(renamed, ignored);
    std::filesystem::remove(opened, ignored);
}

void decode(const std::filesystem::path& clip, ID3D11Device* device, bool software) {
    player::MediaFile file;
    auto start = Clock::now();
    const bool opened = file.open(clip, {device, true, software});
    std::printf("\n[%s] open %.0f ms: %ls\n", software ? "software" : "gpu", ms(start), opened ? file.decoderName().c_str() : file.error().c_str());
    check(opened, "opens");
    if (!opened) return;
    check(software || file.hardware(), "gpu decoding when a device is given");
    std::printf("     duration %.2f s, %zu keyframes, %d audio tracks", file.duration(), file.keyframes().size(), file.audioTracks());
    for (int t = 0; t < file.audioTracks(); ++t) std::printf(" [%ls]", file.audioTrackName(t).c_str());
    std::printf("\n");

    std::vector<double> pictureTimes;
    if (auto first = file.seek(0)) pictureTimes.push_back(first.time);
    start = Clock::now();
    int frames = 0;
    double last = -1;
    bool ordered = true;
    while (frames < 240) {
        auto picture = file.next();
        if (!picture) break;
        ordered = ordered && picture.time > last;
        last = picture.time;
        pictureTimes.push_back(picture.time);
        ++frames;
    }
    const double elapsed = ms(start);
    std::printf("     decoded %d pictures in %.0f ms (%.0f per second)\n", frames, elapsed, frames * 1000.0 / elapsed);
    check(frames > 0 && ordered, "pictures arrive in display order");

    // Compare backwards stepping with sequential decoding, including keyframe
    // boundaries and irregular PTS. Also prove that dot can return to the frame
    // just left, repeated comma keeps moving, and the first frame is stable.
    bool backwards = pictureTimes.size() > 1;
    double backwardMs = 0, backwardWorst = 0;
    int backwardCount = 0;
    for (std::size_t i = 1; i < pictureTimes.size(); ++i) {
        const bool boundary = std::any_of(file.keyframes().begin(), file.keyframes().end(),
            [&](double key) { return std::abs(key - pictureTimes[i]) < 0.001; });
        if (i > 2 && i % 17 != 0 && !boundary) continue;
        start = Clock::now();
        auto prior = file.previous(pictureTimes[i]);
        const double took = ms(start);
        backwardMs += took;
        backwardWorst = std::max(backwardWorst, took);
        ++backwardCount;
        auto following = file.next();
        backwards = backwards && prior && following &&
            std::abs(prior.time - pictureTimes[i - 1]) < 1e-7 &&
            std::abs(following.time - pictureTimes[i]) < 1e-7;
    }
    if (!pictureTimes.empty()) {
        auto first = file.previous(pictureTimes.front());
        backwards = backwards && first && std::abs(first.time - pictureTimes.front()) < 1e-7;
        auto position = pictureTimes.size() - 1;
        auto stepped = file.seek(pictureTimes[position]);
        for (int i = 0; i < 4 && position > 0 && stepped; ++i) {
            stepped = file.previous(stepped.time);
            --position;
            backwards = backwards && stepped && std::abs(stepped.time - pictureTimes[position]) < 1e-7;
        }
    }
    check(backwards, "backwards steps match preceding pictures, cross keyframes and reverse forwards");
    std::printf("     %d backwards steps: average %.1f ms, worst %.1f ms\n", backwardCount,
                backwardMs / std::max(1, backwardCount), backwardWorst);

    double worst = 0, total = 0;
    bool exact = true;
    const int seeks = 12;
    for (int i = 0; i < seeks; ++i) {
        const double target = file.duration() * (i * 7 % seeks + 0.37) / seeks;
        start = Clock::now();
        auto picture = file.seek(target);
        auto following = file.next();
        const double took = ms(start);
        worst = std::max(worst, took);
        total += took;
        // The shown picture is the last one at or before the target.
        exact = exact && picture && picture.time <= target + 0.002 && (!following || following.time > target - 0.002);
        std::vector<float> audio(48000 * 2);
        const auto got = file.audio(audio.data(), 48000);
        if (file.audioTracks() && got < 24000) exact = false;
    }
    std::printf("     %d exact seeks: average %.0f ms, worst %.0f ms\n", seeks, total / seeks, worst);
    check(exact, "seeks land on the picture showing at the target, with audio");
    if (file.audioTracks() > 1) {
        check(file.selectAudio(file.audioTracks() - 1), "switches audio track");
        file.seek(file.duration() / 2);
        std::vector<float> audio(48000 * 2);
        check(file.audio(audio.data(), 48000) == 48000, "decodes the other track");
    }
    if (file.audioTracks() >= 3) {
        // Refarter clips: sources summed (one switched off in the player) against the stored mix in track 1.
        const auto span = static_cast<std::size_t>(std::min(8.0, file.duration() - 0.5) * 48000);
        const auto decode = [&](const std::vector<int>& tracks, double& took) {
            std::vector<float> samples(span * 2);
            file.selectAudio(tracks);
            file.seek(0);
            const auto began = Clock::now();
            std::size_t done = 0;
            while (done < span) {
                const auto got = file.audio(samples.data() + done * 2, span - done);
                if (!got) break;
                done += got;
            }
            took = ms(began);
            samples.resize(done * 2);
            return samples;
        };
        std::vector<int> sources;
        for (int t = 1; t < file.audioTracks(); ++t) if (file.audioTrackName(t) != L"game") sources.push_back(t); // Desktop holds the game.
        double mixMs = 0, sumMs = 0;
        const auto mix = decode({0}, mixMs);
        const auto sum = decode(sources, sumMs);
        double difference = 0, level = 0;
        for (std::size_t i = 0; i < std::min(mix.size(), sum.size()); ++i) {
            difference += (mix[i] - sum[i]) * (mix[i] - sum[i]);
            level += mix[i] * mix[i];
        }
        std::printf("     %.1f s of audio: stored mix %.1f ms, %zu sources summed %.1f ms\n",
                    static_cast<double>(span) / 48000, mixMs, sources.size(), sumMs);
        check(mix.size() == span * 2 && sum.size() == span * 2, "summed sources decode as far as the mix");
        // AAC codes each track separately, so the sum matches the mix closely, not bit for bit.
        check(difference <= 0.01 * level + 1e-9 * static_cast<double>(mix.size()), "summed sources match the stored mix");
        file.selectAudio(0);
    }
}

struct Packet { int stream; long long pts; std::vector<uint8_t> data; bool key; };

std::vector<Packet> packets(const std::filesystem::path& file, double from, double to, int streams) {
    std::vector<Packet> result;
    AVFormatContext* input = nullptr;
    if (avformat_open_input(&input, utf8(file).c_str(), nullptr, nullptr) < 0) return result;
    avformat_find_stream_info(input, nullptr);
    const auto* video = input->streams[0];
    const double origin = video->start_time != AV_NOPTS_VALUE ? video->start_time * av_q2d(video->time_base) : 0;
    if (from > 0) av_seek_frame(input, 0, static_cast<long long>((from + origin) / av_q2d(video->time_base)), AVSEEK_FLAG_BACKWARD);
    AVPacket* packet = av_packet_alloc();
    while (av_read_frame(input, packet) >= 0) {
        const double time = packet->pts * av_q2d(input->streams[packet->stream_index]->time_base) - origin;
        if (packet->stream_index < streams && time >= from - 1e-4 && time < to - 1e-4)
            result.push_back({packet->stream_index, packet->pts, {packet->data, packet->data + packet->size}, (packet->flags & AV_PKT_FLAG_KEY) != 0});
        av_packet_unref(packet);
        if (time > to + 2) break;
    }
    av_packet_free(&packet);
    avformat_close_input(&input);
    return result;
}

void trim(const std::filesystem::path& clip) {
    player::MediaFile file;
    if (!file.open(clip, {nullptr, false, true})) return;
    const auto range = player::snapTrim(file.keyframes(), file.duration() * 0.3, file.duration() * 0.55, file.duration());
    std::printf("\n[trim] %.3f – %.3f s\n", range.start, range.end);
    std::atomic<float> progress = 0;
    std::atomic<bool> cancel = false;
    const auto start = Clock::now();
    const auto result = player::trimClip(clip, range, progress, cancel);
    std::printf("     %ls in %.0f ms: %ls\n", result.ok ? L"written" : L"failed", ms(start), result.ok ? result.path.filename().c_str() : result.error.c_str());
    check(result.ok && progress == 1.f, "trimmed copy written");
    if (!result.ok) return;
    check(std::filesystem::exists(clip), "original kept");
    check(!std::filesystem::exists(result.path.wstring() + L".part"), "no partial file left");

    player::MediaFile copy;
    check(copy.open(result.path, {nullptr, true, true}), "trimmed copy opens");
    std::printf("     copy duration %.3f s (range %.3f s), %d audio tracks\n", copy.duration(), range.length(), copy.audioTracks());
    check(std::abs(copy.duration() - range.length()) < 0.1, "duration matches the range");
    {
        player::MediaFile sound; // `file` opened without audio.
        check(sound.open(clip, {nullptr, true, true}) && copy.audioTracks() == sound.audioTracks(), "every audio track kept");
    }

    AVFormatContext* input = nullptr;
    avformat_open_input(&input, utf8(clip).c_str(), nullptr, nullptr);
    const int streams = static_cast<int>(input->nb_streams);
    std::vector<int> defaults;
    for (int i = 0; i < streams; ++i) defaults.push_back(input->streams[i]->disposition & AV_DISPOSITION_DEFAULT);
    avformat_close_input(&input);
    AVFormatContext* output = nullptr;
    avformat_open_input(&output, utf8(result.path).c_str(), nullptr, nullptr);
    bool sameDefaults = static_cast<int>(output->nb_streams) == streams;
    for (int i = 0; sameDefaults && i < streams; ++i) sameDefaults = (output->streams[i]->disposition & AV_DISPOSITION_DEFAULT) == defaults[i];
    avformat_close_input(&output);
    check(sameDefaults, "track order and default track unchanged");

    const auto original = packets(clip, range.start, range.end, streams);
    // The copy from its start; a reference picture past the end (B-frames) is not compared.
    const auto trimmed = packets(result.path, -1, range.length(), streams);
    std::size_t identical = 0, compared = 0;
    for (int stream = 0; stream < streams; ++stream) {
        std::vector<const Packet*> a, b;
        for (const auto& p : original) if (p.stream == stream) a.push_back(&p);
        for (const auto& p : trimmed) if (p.stream == stream) b.push_back(&p);
        const auto n = std::min(a.size(), b.size());
        for (std::size_t i = 0; i < n; ++i, ++compared) identical += a[i]->data == b[i]->data;
        std::printf("     stream %d: %zu packets in range, %zu in copy\n", stream, a.size(), b.size());
        if (stream == 0) check(!b.empty() && b.front()->key, "copy starts on a keyframe");
        if (stream == 0) check(a.size() == b.size(), "every video packet in the range copied");
    }
    std::printf("     %zu of %zu compared packets byte-identical\n", identical, compared);
    check(compared > 0 && identical == compared, "packets are byte-identical: no re-encoding");

    // Refarter clips (a mix, then its sources): keeping chosen sources only.
    player::MediaFile sound;
    if (!sound.open(clip, {nullptr, true, true})) return;
    if (sound.audioTracks() < 3 || sound.audioTrackName(0) != L"mix") return;
    std::vector<int> sources;
    for (int t = 1; t < sound.audioTracks(); ++t) if (sound.audioTrackName(t) != L"game") sources.push_back(t); // Desktop holds the game.
    const auto keepTracks = [&](const std::vector<int>& keep, bool mix, const char* label) {
        std::atomic<float> done = 0;
        const auto began = Clock::now();
        std::vector<std::string> names; // As the player passes them: older clips carry none.
        for (const int t : keep) {
            std::string name;
            for (const wchar_t c : sound.audioTrackName(t)) name += static_cast<char>(c);
            names.push_back(std::move(name));
        }
        const auto kept = player::trimClip(clip, range, done, cancel, {keep, mix, names});
        std::printf("     %s: %ls in %.0f ms\n", label, kept.ok ? L"written" : kept.error.c_str(), ms(began));
        return kept;
    };
    // All sources with a new mix: the new mix must sound like the stored one.
    const auto remixed = keepTracks(sources, true, "sources with a new mix");
    player::MediaFile mixed;
    check(remixed.ok && mixed.open(remixed.path, {nullptr, true, true}), "trim with a new mix opens");
    if (remixed.ok) {
        bool named = mixed.audioTracks() == sound.audioTracks() && mixed.audioTrackName(0) == L"mix";
        for (int t = 1; named && t < mixed.audioTracks(); ++t) named = mixed.audioTrackName(t) == sound.audioTrackName(t);
        check(named, "new mix first, then the kept sources by name");
        check(std::abs(mixed.duration() - range.length()) < 0.1, "new-mix trim duration matches the range");
        const std::size_t frames = static_cast<std::size_t>(std::min(5.0, range.length() - 0.2) * 48000);
        std::vector<float> stored(frames * 2), made(frames * 2);
        sound.selectAudio(0);
        sound.seek(range.start);
        mixed.selectAudio(0);
        mixed.seek(0);
        const auto a = sound.audio(stored.data(), frames), b = mixed.audio(made.data(), frames);
        double difference = 0, level = 0;
        for (std::size_t i = 0; i < std::min(a, b) * 2; ++i) {
            difference += (stored[i] - made[i]) * (stored[i] - made[i]);
            level += stored[i] * stored[i];
        }
        std::printf("     new mix against stored mix: %.1f dB difference over %.1f s\n",
                    10 * std::log10((difference + 1e-12) / (level + 1e-12)), static_cast<double>(std::min(a, b)) / 48000);
        check(a == frames && b == frames, "new mix decodes as long as the stored one");
        check(difference <= 0.05 * level + 1e-9 * static_cast<double>(frames), "new mix matches the stored mix");
    }
    // One source alone: copied as it is, no mix.
    const auto single = keepTracks({sources.back()}, true, "one source");
    player::MediaFile alone;
    check(single.ok && alone.open(single.path, {nullptr, true, true}) && alone.audioTracks() == 1 &&
          alone.audioTrackName(0) == sound.audioTrackName(sources.back()), "one kept source is the only track");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: player_media_check <clip> <scratch-folder>\n");
        return 2;
    }
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const std::filesystem::path folder = argv[2];
    std::filesystem::create_directories(folder);
    const auto clip = folder / std::filesystem::path(argv[1]).filename();
    std::filesystem::copy_file(argv[1], clip, std::filesystem::copy_options::overwrite_existing);
    const auto gpu = capture::createGpuForMonitor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
    std::printf("gpu: %ls\n", gpu.name.c_str());
    decode(clip, gpu.device.get(), false);
    decode(clip, nullptr, true);
    drawing(clip, gpu.device.get());
    renameWhileOpen(clip);
    trim(clip);
    std::printf("\n%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
