#pragma once

#include <algorithm>

namespace refarter::player {

// The player's magnifier: 1×, 2× or 4× of the picture, and which part of it shows. Only the view: the video
// processor draws that part of the decoded frame into the video rectangle, so nothing is decoded again, and trimming,
// saving a frame and compression always use the whole picture.
struct Zoom {
    int level = 1;               // 1, 2 or 4.
    float x = 0.5f, y = 0.5f;    // The centre of the part shown, as a fraction of the picture's width and height.
    bool operator==(const Zoom&) const = default;
};

inline constexpr int maxZoom = 4;

// The part of the picture shown, as fractions of its width and height.
struct Crop { float left = 0, top = 0, right = 1, bottom = 1; };

inline Crop crop(const Zoom& zoom) {
    const float half = 0.5f / zoom.level;
    return {zoom.x - half, zoom.y - half, zoom.x + half, zoom.y + half};
}

// Keeps the shown part inside the picture.
inline Zoom clamped(Zoom zoom) {
    zoom.level = std::clamp(zoom.level, 1, maxZoom);
    const float half = 0.5f / zoom.level;
    zoom.x = std::clamp(zoom.x, half, 1 - half);
    zoom.y = std::clamp(zoom.y, half, 1 - half);
    return zoom;
}

// One wheel notch or key press in (+1) or out (-1), around a point of the video rectangle (fractions of it,
// 0.5 for its centre): that part of the picture stays under the pointer.
inline Zoom zoomed(Zoom zoom, int steps, float px = 0.5f, float py = 0.5f) {
    const int level = std::clamp(steps > 0 ? zoom.level * 2 : steps < 0 ? zoom.level / 2 : zoom.level, 1, maxZoom);
    if (level == zoom.level) return zoom;
    const auto before = crop(zoom);
    // The picture point under the pointer, then the centre that puts it there at the new level.
    const float pointX = before.left + (before.right - before.left) * std::clamp(px, 0.f, 1.f);
    const float pointY = before.top + (before.bottom - before.top) * std::clamp(py, 0.f, 1.f);
    const float width = 1.f / level;
    return clamped({level, pointX + (0.5f - px) * width, pointY + (0.5f - py) * width});
}

// Moves the shown part by a distance in fractions of the video rectangle (a drag of the whole width is 1): the
// picture follows the pointer.
inline Zoom panned(Zoom zoom, float dx, float dy) {
    zoom.x -= dx / zoom.level;
    zoom.y -= dy / zoom.level;
    return clamped(zoom);
}

} // namespace refarter::player
