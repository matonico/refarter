#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace refarter::player {
inline constexpr std::uint64_t exportLimit = 20'000'000; // Decimal MB, including the MP4 container.
// The hardware encoder overshoots CBR by up to about 7% on busy clips (9% below 2 Mb/s); this 8.7% margin
// lets most clips pass first time, and the retries cover the rest.
inline constexpr double firstBudget = 18'400'000;
inline constexpr int maxExportFps = 60; // Output rate when pictures are dropped.
// Pictures are dropped only above 63: capture jitter puts a 60 fps clip's rounded average at 61 or 62.
inline constexpr int fpsTolerance = 3;
inline bool reducesFps(int averageFps) { return averageFps > maxExportFps + fpsTolerance; }
// Below this the encoder cannot hold the rate even at 480p; ask for a shorter clip instead.
inline constexpr double minExportBitrate = 160'000;
struct ExportPlan { int width, height, bitrate, fps; bool reduceFps; };

// Pixel cap for a bitrate at 60 fps. Frame rate is only reduced above 63: smoothness matters more than
// the extra sharpness 30 fps would buy. Below 3.5 Mb/s 1080p loses to 720p and the encoder cannot hold
// the rate; 1440p only beats 1080p from about 12 Mb/s (VMAF sweep at 60 fps, docs/performance.md).
// Below 1 Mb/s (clips over about 2 min 15 s) 480p keeps 720p's measured bits per pixel; not measured itself.
inline double maxPixels(int bitrate) {
    if (bitrate < 1'000'000) return 854.0 * 480;
    if (bitrate < 3'500'000) return 1280.0 * 720;
    if (bitrate < 12'000'000) return 1920.0 * 1080;
    return 2560.0 * 1440;
}

// `fps`: the source's average frame rate. The tiers compare bits per picture with the 60 fps sweep, so a 30 fps
// clip gets the size a 60 fps clip would at twice the rate. Rates below 30 count as 30, so a misreported
// low rate cannot push a clip to 1440p.
inline std::optional<ExportPlan> planExport(int width, int height, double seconds, bool audio,
                                           double budget = firstBudget, int fps = maxExportFps) {
    if (width < 2 || height < 2 || !std::isfinite(seconds) || seconds <= 0 || seconds > 86'400 ||
        !std::isfinite(budget) || budget <= 0 || budget >= exportLimit) return {};
    const double rate = budget * 8 / seconds - (audio ? 96'000 : 0);
    if (rate < minExportBitrate) return {}; // Ask for a shorter clip; never silently violate the size limit.
    const int bitrate = static_cast<int>(std::min(rate, 20'000'000.0));
    const bool reduce = reducesFps(fps);
    const int outputFps = reduce ? maxExportFps : std::max(1, fps);
    const int tierFps = std::clamp(outputFps, 30, maxExportFps);
    const int perSixty = static_cast<int>(std::min(bitrate * (static_cast<double>(maxExportFps) / tierFps), 2e9));
    // Same aspect ratio, so ultrawide and 1080p sources get the bits per pixel the tiers were measured at.
    const double scale = std::min(1.0, std::sqrt(maxPixels(perSixty) / (static_cast<double>(width) * height)));
    // Even sizes; the epsilon keeps 1920 * (2/3) at 1280 rather than 1278.
    const auto even = [](double v) { return std::max(2, static_cast<int>(std::floor(v / 2 + 1e-6)) * 2); };
    return ExportPlan{even(width * scale), even(height * scale), bitrate, outputFps, reduce};
}

inline std::filesystem::path exportPath(const std::filesystem::path& source, int copy = 1) {
    return source.parent_path() / (L"20mb_" + source.stem().wstring() +
        (copy > 1 ? L" (" + std::to_wstring(copy) + L")" : L"") + L".mp4");
}
} // namespace refarter::player
