#include "player/zoom.h"
#include <cmath>
#include <iostream>

using namespace refarter::player;

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };

    // 1×, 2×, 4× and back; never past either end.
    Zoom zoom;
    const auto whole = crop(zoom);
    check(near(whole.left, 0) && near(whole.top, 0) && near(whole.right, 1) && near(whole.bottom, 1), "1× shows the whole picture");
    zoom = zoomed(zoom, 1);
    check(zoom.level == 2, "one notch in is 2×");
    const auto middle = crop(zoom);
    check(near(middle.left, 0.25f) && near(middle.right, 0.75f) && near(middle.top, 0.25f) && near(middle.bottom, 0.75f),
          "2× around the centre shows the middle half");
    zoom = zoomed(zoomed(zoom, 1), 1);
    check(zoom.level == maxZoom, "4× is the most");
    zoom = zoomed(zoomed(zoomed(zoom, -1), -1), -1);
    check(zoom == Zoom{}, "zooming out ends at the whole picture, centred again");

    // The picture point under the pointer stays there.
    {
        const auto in = zoomed(Zoom{}, 1, 0.8f, 0.3f);
        const auto part = crop(in);
        check(near(part.left + (part.right - part.left) * 0.8f, 0.8f) && near(part.top + (part.bottom - part.top) * 0.3f, 0.3f),
              "zooming in keeps the point under the pointer");
        const auto deeper = zoomed(in, 1, 0.25f, 0.75f);
        const float pointX = part.left + (part.right - part.left) * 0.25f, pointY = part.top + (part.bottom - part.top) * 0.75f;
        const auto inner = crop(deeper);
        check(near(inner.left + (inner.right - inner.left) * 0.25f, pointX) && near(inner.top + (inner.bottom - inner.top) * 0.75f, pointY),
              "and again from 2× to 4×");
        const auto corner = crop(zoomed(Zoom{}, 1, 1.f, 0.f));
        check(near(corner.right, 1) && near(corner.top, 0), "at a corner the part stays inside the picture");
    }

    // Dragging moves the picture with the pointer, up to its edges.
    {
        const auto at2 = zoomed(Zoom{}, 1);
        const auto right = panned(at2, -0.2f, 0); // Dragging left shows more of the right side.
        check(near(right.x, 0.6f) && near(right.y, 0.5f), "a drag of a fifth of the width moves a tenth of the picture at 2×");
        const auto far = panned(at2, -5.f, 5.f);
        check(near(crop(far).right, 1) && near(crop(far).top, 0), "a long drag stops at the picture's edge");
        check(panned(Zoom{}, 0.3f, 0.3f) == Zoom{}, "nothing moves at 1×");
    }
    return failures ? 1 : 0;
}
