#pragma once

#include <optional>
#include <string>

namespace refarter {

// A typed volume: optional spaces, digits, an optional "%" and spaces. A value outside [low, high] is
// clamped to it (a huge number is the top); empty or any other text gives nothing, so the caller reverts.
inline std::optional<int> parseVolumePercent(const std::wstring& text, int low, int high) {
    const auto blank = [](wchar_t c) { return c == L' ' || c == L'\t'; };
    size_t begin = 0, end = text.size();
    while (begin < end && blank(text[begin])) ++begin;
    while (end > begin && blank(text[end - 1])) --end;
    if (end > begin && text[end - 1] == L'%') {
        --end;
        while (end > begin && blank(text[end - 1])) --end;
    }
    if (begin == end) return std::nullopt;
    long long value = 0;
    for (size_t i = begin; i < end; ++i) {
        if (text[i] < L'0' || text[i] > L'9') return std::nullopt;
        if (value <= high) value = value * 10 + (text[i] - L'0'); // Past the top it only needs to stay past it.
    }
    return static_cast<int>(value < low ? low : value > high ? high : value);
}

} // namespace refarter
