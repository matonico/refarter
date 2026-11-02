#include "player/clip_preview.h"
#include <iostream>

using refarter::player::ClipPreview;

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };

    check(ClipPreview::decodeOrder(12) == std::vector<int>{0, 6, 3, 9, 1, 2, 4, 5, 7, 8, 10, 11}, "coarse pictures first, then the gaps");
    const auto order = ClipPreview::decodeOrder(17);
    std::vector<bool> seen(17);
    for (const int i : order) seen[i] = true;
    check(order.size() == 17 && std::all_of(seen.begin(), seen.end(), [](bool s) { return s; }), "every picture once, whatever the count");
    check(ClipPreview::decodeOrder(1) == std::vector<int>{0} && ClipPreview::decodeOrder(0).empty(), "one picture, or none");

    check(ClipPreview::timeOf(0, 10, 60) == 3 && ClipPreview::timeOf(9, 10, 60) == 57, "each picture from the middle of its slice");

    const std::vector<bool> decoded{true, false, false, false, false, false, true, false, false, false, false, false};
    check(ClipPreview::nearest(decoded, 6) == 6, "the picture under the pointer when it is there");
    check(ClipPreview::nearest(decoded, 2) == 0 && ClipPreview::nearest(decoded, 4) == 6, "else the nearest one");
    check(ClipPreview::nearest(decoded, 3) == 0, "the earlier one on a tie");
    check(ClipPreview::nearest(decoded, 11) == 6, "from the end too");
    check(ClipPreview::nearest(std::vector<bool>(12), 5) == -1, "nothing before the first picture arrives");
    return failures ? 1 : 0;
}
