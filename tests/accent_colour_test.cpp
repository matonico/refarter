#include "accent_colour.h"
#include "theme.h"
#include <iostream>
#include <vector>

using namespace refarter;

namespace {
// A square icon of premultiplied BGRA pixels; `paint` picks each pixel's straight colour and alpha.
template <class Paint>
std::vector<std::uint8_t> icon(int size, Paint paint) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const auto [colour, alpha] = paint(x, y);
            auto* p = &pixels[(static_cast<std::size_t>(y) * size + x) * 4];
            p[0] = static_cast<std::uint8_t>(GetBValue(colour) * alpha / 255);
            p[1] = static_cast<std::uint8_t>(GetGValue(colour) * alpha / 255);
            p[2] = static_cast<std::uint8_t>(GetRValue(colour) * alpha / 255);
            p[3] = static_cast<std::uint8_t>(alpha);
        }
    return pixels;
}

struct Pixel { COLORREF colour; int alpha; };

double hueOf(COLORREF c) {
    const double r = GetRValue(c), g = GetGValue(c), b = GetBValue(c), high = std::max({r, g, b}), low = std::min({r, g, b});
    double h = high == r ? std::fmod((g - b) / (high - low), 6.0) : high == g ? (b - r) / (high - low) + 2 : (r - g) / (high - low) + 4;
    h *= 60;
    return h < 0 ? h + 360 : h;
}
} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    const auto accentOf = [](const std::vector<std::uint8_t>& pixels, int size) {
        return iconAccent(pixels.data(), size, size, size * 4, theme::raised);
    };
    constexpr int size = 22;

    const auto red = accentOf(icon(size, [](int, int) { return Pixel{RGB(200, 30, 30), 255}; }), size);
    check(red && (hueOf(*red) < 15 || hueOf(*red) > 345), "a red icon gives a red accent");
    check(red && contrast(*red, theme::raised) >= 3.0 && contrast(*red, theme::background) >= 3.0, "readable on every surface it is drawn on");

    const auto navy = accentOf(icon(size, [](int, int) { return Pixel{RGB(20, 30, 120), 255}; }), size);
    check(navy && hueOf(*navy) > 220 && hueOf(*navy) < 250 && contrast(*navy, theme::raised) >= 3.0,
          "a dark blue icon is lifted until it reads, keeping its hue");

    check(!accentOf(icon(size, [](int, int) { return Pixel{RGB(128, 128, 130), 255}; }), size), "a grey icon keeps the normal accent");
    check(!accentOf(icon(size, [](int, int) { return Pixel{RGB(250, 30, 30), 0}; }), size), "a transparent icon keeps the normal accent");
    check(!accentOf(icon(size, [](int x, int y) { return Pixel{x < 2 && y < 2 ? RGB(30, 200, 30) : RGB(60, 60, 60), 255}; }), size),
          "a speck of colour on a grey icon is not enough");

    // Mostly blue with some red on a transparent edge: blue wins, and the edge does not count.
    const auto mixed = accentOf(icon(size, [](int x, int y) {
        if (x < 3 || y < 3) return Pixel{RGB(255, 0, 0), 40};
        return Pixel{x < 15 ? RGB(40, 90, 220) : RGB(220, 40, 40), 255};
    }), size);
    check(mixed && hueOf(*mixed) > 200 && hueOf(*mixed) < 240, "the largest colour wins");

    // 60 % red on both sides of 0° (355° and 5°) against 40 % yellow: red, not the colour that fits one bucket.
    const auto straddling = accentOf(icon(size, [](int x, int y) {
        const int at = y * size + x;
        if (at % 10 < 3) return Pixel{RGB(230, 0, 20), 255};  // About 355°.
        if (at % 10 < 6) return Pixel{RGB(230, 20, 0), 255};  // About 5°.
        return Pixel{RGB(230, 230, 0), 255};                  // 60°.
    }), size);
    check(straddling && (hueOf(*straddling) < 20 || hueOf(*straddling) > 340), "a red split across 0° still wins");
    for (const COLORREF blue : {RGB(20, 40, 200), RGB(60, 20, 160), RGB(10, 10, 90)}) {
        const auto lifted = accentOf(icon(size, [&](int, int) { return Pixel{blue, 255}; }), size);
        check(lifted && contrast(*lifted, theme::raised) >= 3.0, "blues and violets reach 3:1 on the raised surface");
    }
    return failures ? 1 : 0;
}
