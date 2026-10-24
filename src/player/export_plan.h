#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace refarter::player {
inline constexpr std::uint64_t exportLimit = 20'000'000; // Decimal MB, including the MP4 container.
struct ExportPlan { int width, height, bitrate; bool thirtyFps; };

inline std::optional<ExportPlan> planExport(int width, int height, double seconds, bool audio,
                                           double budget = 18'800'000) {
    if (width < 2 || height < 2 || !std::isfinite(seconds) || seconds <= 0 || seconds > 86'400 ||
        !std::isfinite(budget) || budget <= 0 || budget >= exportLimit) return {};
    const double rate = budget * 8 / seconds - (audio ? 96'000 : 0);
    if (rate < 120'000) return {}; // Ask for a shorter clip; never silently violate the size limit.
    const int bitrate = static_cast<int>(std::min(rate, 20'000'000.0));
    const int maxWidth = bitrate < 2'000'000 ? 854 : bitrate < 3'500'000 ? 1280 : width;
    const double scale = std::min(1.0, static_cast<double>(maxWidth) / width);
    return ExportPlan{std::max(2, static_cast<int>(width * scale) & ~1),
        std::max(2, static_cast<int>(height * scale) & ~1), bitrate, bitrate < 2'000'000};
}

inline std::filesystem::path exportPath(const std::filesystem::path& source, int copy = 1) {
    return source.parent_path() / (L"20mb_" + source.stem().wstring() +
        (copy > 1 ? L" (" + std::to_wstring(copy) + L")" : L"") + L".mp4");
}
} // namespace refarter::player
