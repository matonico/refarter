#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace refarter::capture {

enum class Resolution { Native, P2160, P1440, P1080, P720 };
// H.264 plays everywhere. HEVC and AV1 need a GPU that encodes them, which is
// checked before they can be chosen (see codec_probe.h).
enum class Codec { H264, HEVC, AV1 };
// Presets size a variable bitrate to the encoded picture; Custom is a constant
// bitrate the user picks.
enum class Quality { Small, Balanced, High, Custom };

struct Size {
    int width = 0;
    int height = 0;
    friend bool operator==(const Size&, const Size&) = default;
};

// User-facing video choices. Defaults are a starting point, not measured
// product defaults.
struct VideoSettings {
    Resolution resolution = Resolution::Native;
    int fps = 60;
    int bitrateMbps = 50; // Custom quality only.
    int replaySeconds = 180;
    Quality quality = Quality::Balanced;
    Codec codec = Codec::H264;
};

struct Bitrate {
    int averageKbps = 0;
    int peakKbps = 0;    // Upper bound the encoder may reach in fast motion.
    bool constant = false;
    friend bool operator==(const Bitrate&, const Bitrate&) = default;
};

inline constexpr int minFps = 10;
// Above 60 fps, Windows delivers captured frames unevenly (measured 2026-10-12);
// the product records at up to 60.
inline constexpr int maxFps = 60;
inline constexpr int minBitrateMbps = 2;
// H.264 High level 5.2 allows up to 300 Mb/s; stay below it.
inline constexpr int maxBitrateMbps = 250;
inline constexpr int minReplaySeconds = 5;
inline constexpr int maxReplaySeconds = 3600;
// Hardware H.264 encoders commonly stop at 4096 pixels per side.
inline constexpr int maxEncodeDimension = 4096;

inline std::optional<std::wstring> validate(const VideoSettings& s) {
    if (s.fps < minFps || s.fps > maxFps) return L"frame rate must be 10-60 fps";
    if (s.bitrateMbps < minBitrateMbps || s.bitrateMbps > maxBitrateMbps) return L"bitrate must be 2-250 Mb/s";
    if (s.replaySeconds < minReplaySeconds || s.replaySeconds > maxReplaySeconds) return L"replay must be 5-3600 seconds";
    if (static_cast<unsigned>(s.quality) > static_cast<unsigned>(Quality::Custom) ||
        static_cast<unsigned>(s.codec) > static_cast<unsigned>(Codec::AV1)) return L"unknown quality or codec";
    return std::nullopt;
}

inline int targetHeight(Resolution r) {
    switch (r) {
    case Resolution::P2160: return 2160;
    case Resolution::P1440: return 1440;
    case Resolution::P1080: return 1080;
    case Resolution::P720: return 720;
    default: return 0;
    }
}

// Encoded size for a captured window: keeps the window's aspect ratio, never
// upscales (that only spends bits), and rounds to even dimensions for NV12.
inline Size outputSize(Resolution r, Size source) {
    if (source.width < 2 || source.height < 2) return {};
    double scale = 1.0;
    if (const int h = targetHeight(r); h > 0 && h < source.height) scale = static_cast<double>(h) / source.height;
    const int longest = std::max(source.width, source.height);
    if (longest * scale > maxEncodeDimension) scale = static_cast<double>(maxEncodeDimension) / longest;
    const auto even = [](double v) { return std::max(2, static_cast<int>(v + 0.5) & ~1); };
    return {even(source.width * scale), even(source.height * scale)};
}

// Bits per encoded pixel at 60 fps for H.264 presets. Balanced matches OBS's
// replay clips at CQP 23 (1080p60: median 22.6 Mb/s, 90th percentile 39.7 Mb/s
// across 136 clips): 19.9 Mb/s average, 39.8 Mb/s peak.
inline constexpr double presetBitsPerPixel[] = {0.09, 0.16, 0.28};
// Bitrate at equal picture quality relative to H.264; common estimates for
// hardware encoders, not measured here.
inline constexpr double codecEfficiency[] = {1.0, 0.65, 0.55};

inline Bitrate bitrateFor(const VideoSettings& s, Size output) {
    if (s.quality == Quality::Custom) {
        const int kbps = s.bitrateMbps * 1000;
        return {kbps, kbps, true};
    }
    // Fewer frames change more between frames, so bits fall slower than fps.
    const double frames = 60.0 * std::pow(s.fps / 60.0, 0.75);
    const double bits = presetBitsPerPixel[static_cast<int>(s.quality)] * codecEfficiency[static_cast<int>(s.codec)] *
        output.width * output.height * frames;
    const int average = std::clamp(static_cast<int>(bits / 1000), minBitrateMbps * 1000, maxBitrateMbps * 500);
    return {average, average * 2, false};
}

inline const wchar_t* codecName(Codec codec) {
    return codec == Codec::AV1 ? L"av1" : codec == Codec::HEVC ? L"hevc" : L"h.264";
}

// "vbr 19.9 mbps, peak 39.8" or "cbr 50.0 mbps".
inline std::wstring describe(const Bitrate& b) {
    const auto mbps = [](int kbps) { return std::to_wstring(kbps / 1000) + L"." + std::to_wstring(kbps % 1000 / 100); };
    return b.constant ? L"cbr " + mbps(b.averageKbps) + L" mbps" : L"vbr " + mbps(b.averageKbps) + L" mbps, peak " + mbps(b.peakKbps);
}

// Compressed video for `seconds` at `kbps`.
inline std::uint64_t replayBytes(int kbps, int seconds) {
    return static_cast<std::uint64_t>(kbps) * 1000 / 8 * static_cast<std::uint64_t>(seconds);
}

} // namespace refarter::capture
