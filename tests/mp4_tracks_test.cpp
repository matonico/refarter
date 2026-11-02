#include "capture/mp4_tracks.h"
#include <iostream>

using namespace refarter::capture;
using Bytes = std::vector<std::uint8_t>;

namespace {
Bytes box(std::string_view type, const Bytes& payload) {
    Bytes out(8);
    putBe32(out.data(), static_cast<std::uint32_t>(8 + payload.size()));
    std::copy(type.begin(), type.end(), out.begin() + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
Bytes join(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}
// stsd with one AAC entry declaring `bitrate` as its maximum and average. Media Foundation writes
// four-byte descriptor lengths; `shortLengths` writes one byte, as ffmpeg does.
Bytes aacSampleTable(std::uint32_t bitrate, bool shortLengths = false) {
    const auto length = [&](std::uint8_t n) { return shortLengths ? Bytes{n} : Bytes{0x80, 0x80, 0x80, n}; };
    Bytes config{0x40, 0x15, 0, 6, 0};
    config.resize(13);
    putBe32(&config[5], bitrate);
    putBe32(&config[9], bitrate);
    config.insert(config.end(), {0x05, 2, 0x11, 0x90}); // AudioSpecificConfig: AAC-LC, 48 kHz, stereo.
    const Bytes decoder = join({Bytes{0x04}, length(static_cast<std::uint8_t>(config.size())), config});
    const Bytes es = join({Bytes{0, 1, 0}, decoder, Bytes{0x06}, length(1), Bytes{0x02}});
    const Bytes esds = box("esds", join({Bytes(4), Bytes{0x03}, length(static_cast<std::uint8_t>(es.size())), es}));
    Bytes entry(28);
    entry[7] = 1;   // data_reference_index
    entry[17] = 2;  // channelcount
    entry[19] = 16; // samplesize
    putBe32(&entry[24], 48000u << 16);
    Bytes stsd(8);
    stsd[7] = 1;    // entry_count
    return box("minf", box("stbl", box("stsd", join({stsd, box("mp4a", join({entry, esds}))}))));
}
// As Media Foundation writes them: tkhd version 0 with only track_enabled, hdlr with a 13-byte name.
Bytes trak(std::string_view handler, const Bytes& extra = {}, std::uint32_t bitrate = 192000, bool shortLengths = false) {
    Bytes tkhd(84);
    tkhd[3] = 0x01;
    Bytes hdlr(37);
    std::copy(handler.begin(), handler.end(), hdlr.begin() + 8);
    const std::string_view name = handler == "soun" ? "SoundHandler" : "VideoHandler";
    std::copy(name.begin(), name.end(), hdlr.begin() + 24);
    const Bytes media = handler == "soun" ? aacSampleTable(bitrate, shortLengths) : Bytes{};
    return box("trak", join({box("tkhd", tkhd), box("mdia", join({box("hdlr", hdlr), media})), extra}));
}
Bytes moov(std::initializer_list<Bytes> traks) {
    Bytes payload = box("mvhd", Bytes(100));
    for (const auto& t : traks) payload.insert(payload.end(), t.begin(), t.end());
    return box("moov", payload);
}

struct Track { std::size_t at, size; };
std::vector<Track> traks(const Bytes& moov) {
    std::vector<Track> out;
    forEachBox(moov, 8, moov.size(), [&](std::size_t at, std::size_t size, std::string_view type) {
        if (type == "trak") out.push_back({at, size});
    });
    return out;
}
std::string child(const Bytes& moov, Track t, std::string_view path) { // "mdia": the handler name; "udta": the title.
    std::string found;
    forEachBox(moov, t.at + 8, t.at + t.size, [&](std::size_t at, std::size_t size, std::string_view type) {
        if (type != path.substr(0, 4)) return;
        if (path == "udta") {
            forEachBox(moov, at + 8, at + size, [&](std::size_t n, std::size_t s, std::string_view inner) {
                if (inner == "name") found.assign(reinterpret_cast<const char*>(&moov[n + 8]), s - 8);
            });
        }
        if (path == "mdia") {
            forEachBox(moov, at + 8, at + size, [&](std::size_t h, std::size_t, std::string_view inner) {
                if (inner == "hdlr") found = reinterpret_cast<const char*>(&moov[h + 32]);
            });
        }
    });
    return found;
}
int udtaCount(const Bytes& moov, Track t) {
    int count = 0;
    forEachBox(moov, t.at + 8, t.at + t.size, [&](std::size_t, std::size_t, std::string_view type) { count += type == "udta"; });
    return count;
}
// Declared {maxBitrate, avgBitrate} of an audio track.
std::pair<std::uint32_t, std::uint32_t> bitrates(const Bytes& moov, Track t) {
    std::pair<std::uint32_t, std::uint32_t> found{};
    forEachBox(moov, t.at + 8, t.at + t.size, [&](std::size_t at, std::size_t size, std::string_view type) {
        if (type != "mdia") return;
        if (const auto field = declaredBitrates(moov, at, size)) found = {be32(&moov[field]), be32(&moov[field + 4])};
    });
    return found;
}
std::uint8_t enabled(const Bytes& moov, Track t) { return moov[t.at + 8 + 11] & 0x01; }
int group(const Bytes& moov, Track t) { return moov[t.at + 8 + 42] << 8 | moov[t.at + 8 + 43]; }
} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    const std::vector<std::string> names{"mix", "desktop", "game output"};
    const auto original = moov({trak("vide"), trak("soun"), trak("soun"), trak("soun")});

    const auto inPlace = markedMoov(original, names, true, false);
    check(inPlace.size() == original.size(), "without titles the index keeps its size");
    auto t = traks(inPlace);
    check(t.size() == 4, "four tracks");
    check(child(inPlace, t[0], "mdia") == "VideoHandler", "the video handler name stays");
    check(child(inPlace, t[1], "mdia") == "mix" && child(inPlace, t[2], "mdia") == "desktop" && child(inPlace, t[3], "mdia") == "game output",
          "audio handler names are the track names");
    check(enabled(inPlace, t[0]) && group(inPlace, t[0]) == 0, "video stays enabled, in no group");
    check(enabled(inPlace, t[1]) && !enabled(inPlace, t[2]) && !enabled(inPlace, t[3]), "only the mix is enabled");
    check(group(inPlace, t[1]) == 1 && group(inPlace, t[3]) == 1, "audio tracks share alternate group 1");
    check(udtaCount(inPlace, t[1]) == 0, "no title without permission to grow");
    using Rates = std::pair<std::uint32_t, std::uint32_t>;
    check(bitrates(original, traks(original)[1]) == Rates{192000, 192000}, "the test's esds is read");
    check(bitrates(inPlace, t[1]) == Rates{193000, 193000}, "the mix declares the highest bitrate");
    check(bitrates(inPlace, t[2]) == Rates{192000, 192000} && bitrates(inPlace, t[3]) == Rates{192000, 192000},
          "the other tracks keep their declared bitrate");

    const auto ffmpegStyle = moov({trak("vide"), trak("soun", {}, 192000, true), trak("soun", {}, 192600, true)});
    const auto styled = markedMoov(ffmpegStyle, {"mix", "desktop"}, true, false);
    check(bitrates(styled, traks(styled)[1]) == Rates{193600, 193600}, "one-byte descriptor lengths: above the highest other track");
    const auto ahead = moov({trak("vide"), trak("soun", {}, 200000), trak("soun", {}, 192000)});
    const auto unchanged = markedMoov(ahead, {"mix", "desktop"}, true, false);
    check(bitrates(unchanged, traks(unchanged)[1]) == Rates{200000, 200000}, "a mix already far enough ahead is not lowered");
    const auto single = markedMoov(moov({trak("vide"), trak("soun")}), {"audio"}, false, false);
    check(bitrates(single, traks(single)[1]) == Rates{192000, 192000}, "a single track is not touched");

    const auto titled = markedMoov(original, names, true, true);
    check(titled.size() == original.size() + 3 * 16 + 3 + 7 + 11, "each title adds udta and name headers and its text");
    check(be32(titled.data()) == titled.size(), "the moov size is updated");
    t = traks(titled);
    check(t.size() == 4 && t.back().at + t.back().size == titled.size(), "track sizes add up to the moov");
    check(child(titled, t[0], "udta").empty() && udtaCount(titled, t[0]) == 0, "video gets no title");
    check(child(titled, t[1], "udta") == "mix" && child(titled, t[2], "udta") == "desktop" && child(titled, t[3], "udta") == "game output",
          "audio tracks are titled");
    check(child(titled, t[2], "mdia") == "desktop" && enabled(titled, t[1]) && !enabled(titled, t[2]), "titles keep the in-place marks");

    const auto twoNames = markedMoov(original, {"mix", ""}, false, true);
    t = traks(twoNames);
    check(udtaCount(twoNames, t[1]) == 1 && udtaCount(twoNames, t[2]) == 0 && udtaCount(twoNames, t[3]) == 0, "unnamed tracks get no title");
    check(enabled(twoNames, t[2]) && group(twoNames, t[2]) == 0, "a single track keeps Media Foundation's marks");

    const auto withUdta = moov({trak("vide"), trak("soun", box("udta", box("name", Bytes{'o', 'l', 'd'})))});
    const auto kept = markedMoov(withUdta, {"mix"}, false, true);
    t = traks(kept);
    check(kept.size() == withUdta.size() && udtaCount(kept, t[1]) == 1 && child(kept, t[1], "udta") == "old",
          "an existing track udta is left as it is");

    auto large = original;
    putBe32(large.data(), 1); // A 64-bit size follows.
    check(markedMoov(large, names, true, true) == large, "a 64-bit moov is left alone");

    auto trailing = original;
    trailing.insert(trailing.end(), {0, 0, 0});
    putBe32(trailing.data(), static_cast<std::uint32_t>(trailing.size()));
    const auto unread = markedMoov(trailing, names, true, true);
    check(unread.size() == trailing.size() && child(unread, traks(unread)[1], "mdia") == "mix",
          "bytes the walk cannot read stop the titles, not the names");

    if (failures == 0) std::cout << "mp4 tracks: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
