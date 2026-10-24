#pragma once

#include "capture/video_settings.h"

#include <optional>
#include <string>
#include <string_view>

namespace refarter {
// The replay interval is the recorder's (capture::minReplaySeconds to capture::maxReplaySeconds).

inline std::optional<int> parseReplaySeconds(std::wstring_view text) {
    if (text.empty() || text.size() > 4) return std::nullopt;
    int value = 0;
    for (const auto c : text) {
        if (c < L'0' || c > L'9') return std::nullopt;
        value = value * 10 + (c - L'0');
    }
    if (value < capture::minReplaySeconds || value > capture::maxReplaySeconds) return std::nullopt;
    return value;
}

// "3 min", "45 s" or "1 min 30 s".
inline std::wstring formatReplayDuration(int seconds) {
    const auto minutes = std::to_wstring(seconds / 60) + L" min", rest = std::to_wstring(seconds % 60) + L" s";
    return seconds < 60 ? rest : seconds % 60 == 0 ? minutes : minutes + L" " + rest;
}
} // namespace refarter
