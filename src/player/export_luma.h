#pragma once
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#include <algorithm>
#include <cstdint>
#include <optional>

namespace refarter::player {
struct ExportLuma { unsigned minimum, maximum, mean; };

// At most 16 x 16 luma values from a CPU picture, normalised to 8-bit.
// Diagnostic summaries only: no image/plane is retained or added to the log.
inline std::optional<ExportLuma> sampleExportLuma(const AVFrame& frame) {
    unsigned bytes = 1, shift = 0;
    switch (frame.format) {
    case AV_PIX_FMT_NV12: case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P: break;
    case AV_PIX_FMT_YUV420P10LE: bytes = 2; shift = 2; break;
    case AV_PIX_FMT_YUV420P12LE: bytes = 2; shift = 4; break;
    case AV_PIX_FMT_P010LE: bytes = 2; shift = 8; break;
    default: return {};
    }
    const auto stride = static_cast<std::int64_t>(frame.linesize[0]);
    if (!frame.data[0] || frame.width < 1 || frame.height < 1 ||
        (stride < 0 ? -stride : stride) < static_cast<std::int64_t>(frame.width) * bytes) return {};
    const int columns = std::min(16, frame.width), rows = std::min(16, frame.height);
    unsigned low = 255, high = 0, sum = 0;
    for (int y = 0; y < rows; ++y) {
        const auto row = rows == 1 ? 0 : static_cast<std::int64_t>(y) * (frame.height - 1) / (rows - 1);
        for (int x = 0; x < columns; ++x) {
            const auto column = columns == 1 ? 0 : static_cast<std::int64_t>(x) * (frame.width - 1) / (columns - 1);
            const auto* sample = frame.data[0] + row * stride + column * bytes;
            const unsigned raw = bytes == 1 ? sample[0] : sample[0] | (static_cast<unsigned>(sample[1]) << 8);
            const auto value = std::min(255u, raw >> shift);
            low = std::min(low, value); high = std::max(high, value); sum += value;
        }
    }
    return ExportLuma{low, high, sum / static_cast<unsigned>(columns * rows)};
}
} // namespace refarter::player
