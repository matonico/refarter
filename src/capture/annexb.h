#pragma once

#include "video_settings.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Minimal Annex B helpers for H.264 and HEVC: enough to find parameter sets
// and keyframes in encoder output without depending on a bitstream library.
// AV1 is not Annex B; its keyframes come from the encoder's clean-point flag.
namespace refarter::capture::annexb {

struct Nal {
    std::span<const std::uint8_t> withStartCode;
    std::uint8_t header; // First byte after the start code.
};

template <typename Visit>
void forEachNal(std::span<const std::uint8_t> data, Visit&& visit) {
    const auto startCode = [&](std::size_t i) -> std::size_t {
        if (i + 3 <= data.size() && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return 3;
        if (i + 4 <= data.size() && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) return 4;
        return 0;
    };
    std::size_t begin = 0;
    std::size_t prefix = 0;
    while (begin < data.size() && (prefix = startCode(begin)) == 0) ++begin;
    while (begin < data.size()) {
        std::size_t next = begin + prefix;
        std::size_t nextPrefix = 0;
        while (next < data.size() && (nextPrefix = startCode(next)) == 0) ++next;
        if (begin + prefix < next) visit(Nal{data.subspan(begin, next - begin), data[begin + prefix]});
        begin = next;
        prefix = nextPrefix;
    }
}

inline std::uint8_t nalType(Codec codec, std::uint8_t header) {
    return codec == Codec::HEVC ? static_cast<std::uint8_t>((header >> 1) & 0x3f) : static_cast<std::uint8_t>(header & 0x1f);
}

// H.264: IDR (5). HEVC: IRAP pictures, BLA/IDR/CRA (16-21); the encoder's
// keyframes are IDR, and every IRAP starts a decodable sequence.
inline bool containsKeyframe(Codec codec, std::span<const std::uint8_t> data) {
    bool found = false;
    forEachNal(data, [&](const Nal& nal) {
        const auto type = nalType(codec, nal.header);
        found = found || (codec == Codec::HEVC ? type >= 16 && type <= 21 : type == 5);
    });
    return found;
}

// Parameter sets with start codes, as used for MF_MT_MPEG_SEQUENCE_HEADER:
// H.264 SPS + PPS, HEVC VPS + SPS + PPS. Empty unless all are present.
inline std::vector<std::uint8_t> parameterSets(Codec codec, std::span<const std::uint8_t> data) {
    if (codec == Codec::AV1) return {};
    const std::vector<std::uint8_t> wanted = codec == Codec::HEVC ? std::vector<std::uint8_t>{32, 33, 34} : std::vector<std::uint8_t>{7, 8};
    std::vector<std::vector<std::uint8_t>> sets(wanted.size());
    forEachNal(data, [&](const Nal& nal) {
        for (std::size_t i = 0; i < wanted.size(); ++i) {
            if (nalType(codec, nal.header) == wanted[i]) sets[i].insert(sets[i].end(), nal.withStartCode.begin(), nal.withStartCode.end());
        }
    });
    std::vector<std::uint8_t> header;
    for (const auto& set : sets) {
        if (set.empty()) return {};
        header.insert(header.end(), set.begin(), set.end());
    }
    return header;
}

} // namespace refarter::capture::annexb
