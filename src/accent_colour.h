#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace refarter {

// Relative luminance (WCAG) of a colour.
inline double luminance(COLORREF colour) {
    const auto channel = [](int value) {
        const double c = value / 255.0;
        return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(GetRValue(colour)) + 0.7152 * channel(GetGValue(colour)) + 0.0722 * channel(GetBValue(colour));
}

inline double contrast(COLORREF a, COLORREF b) {
    const double x = luminance(a), y = luminance(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

inline COLORREF fromHsl(double hue, double saturation, double lightness) {
    const double c = (1 - std::fabs(2 * lightness - 1)) * saturation, h = std::fmod(hue, 360.0) / 60.0;
    const double x = c * (1 - std::fabs(std::fmod(h, 2.0) - 1)), m = lightness - c / 2;
    double r = 0, g = 0, b = 0;
    if (h < 1) { r = c; g = x; } else if (h < 2) { r = x; g = c; } else if (h < 3) { g = c; b = x; }
    else if (h < 4) { g = x; b = c; } else if (h < 5) { r = x; b = c; } else { r = c; b = x; }
    const auto byte = [&](double v) { return static_cast<BYTE>(std::lround(std::clamp(v + m, 0.0, 1.0) * 255)); };
    return RGB(byte(r), byte(g), byte(b));
}

// The colour a game's icon mostly is, made fit for the app's dark palette: its own hue, at least moderately
// saturated, light enough to read against `against` (3:1, as for interface parts; pass the lightest surface it is drawn on). Nothing when the icon is grey,
// mostly transparent, or its colour covers too little of it: the normal accent then stays. `pixels` are premultiplied
// BGRA rows of `stride` bytes.
inline std::optional<COLORREF> iconAccent(const std::uint8_t* pixels, int width, int height, int stride, COLORREF against) {
    constexpr int buckets = 12; // 30° of hue each.
    double weight[buckets]{}, red[buckets]{}, green[buckets]{}, blue[buckets]{};
    double opaque = 0;
    for (int y = 0; y < height; ++y) {
        const auto* row = pixels + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            const int a = row[x * 4 + 3];
            if (a < 128) continue;
            opaque += 1;
            const double b = row[x * 4] * 255.0 / a, g = row[x * 4 + 1] * 255.0 / a, r = row[x * 4 + 2] * 255.0 / a;
            const double high = std::max({r, g, b}), low = std::min({r, g, b});
            const double saturation = high > 0 ? (high - low) / high : 0, value = high / 255;
            if (saturation < 0.35 || value < 0.25) continue; // Grey, black or near white: no colour to take.
            double hue = 0;
            if (high == r) hue = std::fmod((g - b) / (high - low), 6.0);
            else if (high == g) hue = (b - r) / (high - low) + 2;
            else hue = (r - g) / (high - low) + 4;
            hue *= 60;
            if (hue < 0) hue += 360;
            const int bucket = std::min(buckets - 1, static_cast<int>(hue / (360.0 / buckets)));
            const double share = saturation * value;
            weight[bucket] += share;
            red[bucket] += r * share;
            green[bucket] += g * share;
            blue[bucket] += b * share;
        }
    }
    // Two neighbouring buckets at a time, round the circle: a colour straddling a boundary (red at 355° and 5°) counts
    // as one, rather than losing to a smaller colour that fits a single bucket. Pairs, not wider, so the colours next
    // to it on the wheel (yellow beside red) do not blend in.
    const auto around = [&](const double (&values)[buckets], int bucket) { return values[bucket] + values[(bucket + 1) % buckets]; };
    int best = 0;
    for (int bucket = 1; bucket < buckets; ++bucket) if (around(weight, bucket) > around(weight, best)) best = bucket;
    const double share = around(weight, best);
    if (opaque == 0 || share < 0.12 * opaque) return std::nullopt;
    const double r = around(red, best) / share / 255, g = around(green, best) / share / 255, b = around(blue, best) / share / 255;
    const double high = std::max({r, g, b}), low = std::min({r, g, b});
    if (high - low < 0.02) return std::nullopt; // Colours that cancel out into grey.
    double hue = 0;
    if (high == r) hue = std::fmod((g - b) / (high - low), 6.0);
    else if (high == g) hue = (b - r) / (high - low) + 2;
    else hue = (r - g) / (high - low) + 4;
    hue = hue * 60 < 0 ? hue * 60 + 360 : hue * 60;
    const double lightness = (high + low) / 2;
    const double saturation = high == low ? 0 : (high - low) / (1 - std::fabs(2 * lightness - 1));
    // Calm, but clearly a colour: as saturated and about as light as the app's own accent at most.
    const double s = std::clamp(saturation, 0.55, 0.9);
    double l = std::clamp(lightness, 0.58, 0.72);
    auto colour = fromHsl(hue, s, l);
    while (contrast(colour, against) < 3.0 && l < 0.85) colour = fromHsl(hue, s, l += 0.02);
    return colour;
}

} // namespace refarter
