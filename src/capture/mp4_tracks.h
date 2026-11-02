#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace refarter::capture {

inline std::uint32_t be32(const std::uint8_t* p) { return (std::uint32_t{p[0]} << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

inline void putBe32(std::uint8_t* p, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
}

// Children of a box payload as (offset, size, type), offsets relative to `data`.
template <typename Visit>
void forEachBox(const std::vector<std::uint8_t>& data, std::size_t begin, std::size_t end, Visit&& visit) {
    while (begin + 8 <= end) {
        const std::size_t size = be32(&data[begin]);
        if (size < 8 || begin + size > end) return;
        visit(begin, size, std::string_view(reinterpret_cast<const char*>(&data[begin + 4]), 4));
        begin += size;
    }
}

// Offset of the maxBitrate field (avgBitrate follows) in an AAC track's esds,
// given its mdia box, or 0 when it cannot be found.
inline std::size_t declaredBitrates(const std::vector<std::uint8_t>& moov, std::size_t mdia, std::size_t mdiaSize) {
    std::size_t found = 0;
    const auto inside = [&](std::size_t at, std::size_t size, std::string_view type, auto&& then) {
        forEachBox(moov, at, at + size, [&](std::size_t child, std::size_t childSize, std::string_view childType) {
            if (childType == type) then(child, childSize);
        });
    };
    inside(mdia + 8, mdiaSize - 8, "minf", [&](std::size_t minf, std::size_t minfSize) {
        inside(minf + 8, minfSize - 8, "stbl", [&](std::size_t stbl, std::size_t stblSize) {
            inside(stbl + 8, stblSize - 8, "stsd", [&](std::size_t stsd, std::size_t stsdSize) {
                if (stsdSize < 16) return;
                inside(stsd + 16, stsdSize - 16, "mp4a", [&](std::size_t entry, std::size_t entrySize) {
                    if (entrySize < 36) return;
                    inside(entry + 36, entrySize - 36, "esds", [&](std::size_t esds, std::size_t esdsSize) {
                        const std::size_t end = esds + esdsSize;
                        std::size_t p = esds + 12; // After the version and flags.
                        // A descriptor: its tag, then a length of one to four bytes.
                        const auto descriptor = [&](std::uint8_t tag) {
                            if (p >= end || moov[p++] != tag) return false;
                            for (int i = 0; i < 4 && p < end; ++i) if (!(moov[p++] & 0x80)) return true;
                            return false;
                        };
                        if (!descriptor(0x03) || p + 3 > end) return; // ES_Descriptor
                        const std::uint8_t flags = moov[p + 2];
                        p += 3;
                        if (flags & 0x80) p += 2;                        // dependsOn_ES_ID
                        if (flags & 0x40) p += p < end ? 1 + moov[p] : 0; // URL
                        if (flags & 0x20) p += 2;                        // OCR_ES_Id
                        if (!descriptor(0x04) || p + 13 > end) return;   // DecoderConfigDescriptor
                        found = p + 5; // After objectTypeIndication, streamType and bufferSizeDB.
                    });
                });
            });
        });
    });
    return found;
}

// How much higher the mix's declared bitrates are than any other audio track's.
// Media Foundation (Windows Media Player) ignores the enabled flags and plays the
// audio track whose esds average and maximum bitrates are highest, the last
// track on a tie; Refarter encodes every stereo track, the mix included, at the
// same 192 kb/s (the mono microphone at 96). 1 or 8 b/s more did not win, 1000
// did (measured on Windows 11).
constexpr std::uint32_t mixBitrateLead = 1000;

// Patches the audio tracks of a `moov` box (the whole box, header included) and returns it.
//
// Media Foundation marks every audio track enabled with no alternate group, so
// some players play all of them at once. Put audio tracks in one alternate
// group and enable only the first (the mix), as ffmpeg does, and declare the
// mix's bitrate highest for Media Foundation (see mixBitrateLead; the audio is
// unchanged). Each track's
// name replaces Media Foundation's "SoundHandler" handler name, which has room
// for 12 characters; FFmpeg-based players (and Refarter's own) read it there.
// Media Foundation, and so Windows Media Player, reads a track's title only
// from a `udta/name` box, so with `addTitles` each named track also gets one,
// as ffmpeg writes a title. That grows the box: only allowed when nothing
// follows it in the file, or chunk offsets would point at the wrong bytes.
inline std::vector<std::uint8_t> markedMoov(std::vector<std::uint8_t> moov, const std::vector<std::string>& names,
                                             bool alternates, bool addTitles) {
    if (moov.size() < 8 || be32(moov.data()) != moov.size()) return moov; // A 64-bit size: left alone.
    struct Title { std::size_t trak; std::string_view name; };
    std::vector<Title> titles;
    std::vector<std::size_t> bitrates; // declaredBitrates of each audio track.
    std::size_t index = 0; // Of the audio track, in writing order.
    forEachBox(moov, 8, moov.size(), [&](std::size_t trak, std::size_t trakSize, std::string_view type) {
        if (type != "trak") return;
        std::size_t tkhd = 0, tkhdSize = 0, hdlr = 0, hdlrSize = 0, mdia = 0, mdiaSize = 0;
        bool udta = false;
        forEachBox(moov, trak + 8, trak + trakSize, [&](std::size_t at, std::size_t boxSize, std::string_view child) {
            if (child == "tkhd") { tkhd = at; tkhdSize = boxSize; }
            if (child == "udta") udta = true;
            if (child != "mdia") return;
            mdia = at;
            mdiaSize = boxSize;
            forEachBox(moov, at + 8, at + boxSize, [&](std::size_t h, std::size_t s, std::string_view grandchild) {
                if (grandchild == "hdlr" && s >= 32 && std::string_view(reinterpret_cast<const char*>(&moov[h + 16]), 4) == "soun") {
                    hdlr = h;
                    hdlrSize = s;
                }
            });
        });
        if (!hdlr || !tkhd) return;
        bitrates.push_back(declaredBitrates(moov, mdia, mdiaSize));
        const std::string_view name = index < names.size() ? std::string_view(names[index]) : std::string_view{};
        if (!name.empty() && name.size() < hdlrSize - 32) { // Keeps the terminating zero.
            std::fill(moov.begin() + static_cast<std::ptrdiff_t>(hdlr + 32), moov.begin() + static_cast<std::ptrdiff_t>(hdlr + hdlrSize), std::uint8_t{0});
            std::copy(name.begin(), name.end(), moov.begin() + static_cast<std::ptrdiff_t>(hdlr + 32));
        }
        if (!name.empty() && !udta) titles.push_back({trak, name}); // Media Foundation writes no track udta.
        // A version-0 tkhd has its alternate_group at byte 42 and flags at 11, a version-1 one at 54: a shorter box is
        // left as it is rather than written past.
        const std::size_t group = tkhd + 8 + (tkhdSize > 8 && moov[tkhd + 8] == 1 ? 46 : 34); // alternate_group
        if (alternates && tkhdSize >= group - tkhd + 2) {
            moov[group] = 0;
            moov[group + 1] = 1;
            if (index == 0) moov[tkhd + 11] |= 0x01; else moov[tkhd + 11] &= ~0x01; // track_enabled
        }
        ++index;
    });
    if (alternates && bitrates.size() > 1 && bitrates[0] &&
        std::all_of(bitrates.begin() + 1, bitrates.end(), [](std::size_t at) { return at != 0; })) {
        for (std::size_t field = 0; field < 8; field += 4) { // maxBitrate, then avgBitrate.
            std::uint32_t others = 0;
            for (std::size_t t = 1; t < bitrates.size(); ++t) others = std::max(others, be32(&moov[bitrates[t] + field]));
            auto* mix = &moov[bitrates[0] + field];
            if (others <= UINT32_MAX - mixBitrateLead && be32(mix) < others + mixBitrateLead) putBe32(mix, others + mixBitrateLead);
        }
    }
    if (!addTitles || titles.empty()) return moov;

    // udta { name { title } }, appended at the end of its trak.
    std::vector<std::uint8_t> grown(moov.begin(), moov.begin() + 8);
    auto title = titles.begin();
    std::size_t walked = 8;
    forEachBox(moov, 8, moov.size(), [&](std::size_t at, std::size_t size, std::string_view) {
        walked = at + size;
        const std::size_t box = grown.size();
        grown.insert(grown.end(), moov.begin() + static_cast<std::ptrdiff_t>(at), moov.begin() + static_cast<std::ptrdiff_t>(at + size));
        if (title == titles.end() || title->trak != at) return;
        const auto nameSize = static_cast<std::uint32_t>(8 + title->name.size());
        std::uint8_t header[16];
        putBe32(header, 8 + nameSize);
        std::copy_n("udta", 4, header + 4);
        putBe32(header + 8, nameSize);
        std::copy_n("name", 4, header + 12);
        grown.insert(grown.end(), header, header + 16);
        grown.insert(grown.end(), title->name.begin(), title->name.end());
        putBe32(&grown[box], static_cast<std::uint32_t>(grown.size() - box));
        ++title;
    });
    if (walked != moov.size()) return moov; // Bytes the walk could not read would be lost.
    putBe32(grown.data(), static_cast<std::uint32_t>(grown.size()));
    return grown;
}

} // namespace refarter::capture
