// Layout of the player surface: the audio-source switches and the trim bar's buttons never overlap or invert.
#include "player/player_view.h"
#include <iostream>

using namespace refarter::player;

namespace {
bool empty(const D2D1_RECT_F& r) { return r.right <= r.left; }
bool sane(const D2D1_RECT_F& r) { return r.right >= r.left && r.bottom >= r.top; }
bool overlap(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return !empty(a) && !empty(b) && a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}
} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };

    ClipInfo clip;
    clip.ready = true;
    clip.duration = 60;
    clip.tracks = {L"desktop", L"microphone", L"discord"};
    clip.checked = {true, true, false};
    clip.toggles = true;

    const auto sourceHits = [](const Layout& l, const View& v, const ClipInfo& c) {
        int hits = 0;
        for (int i = 0; i < l.sourceCount; ++i) {
            const auto& r = l.sources[i];
            const auto part = hitTest(l, v, c, (r.left + r.right) / 2, (r.top + r.bottom) / 2);
            if (sourceIndex(part) == i) ++hits;
        }
        return hits;
    };

    for (const bool editing : {false, true}) {
        for (const float scale : {1.f, 1.5f}) {
            for (const float width : {520.f, 700.f, 960.f, 1280.f, 1920.f}) {
                View view;
                view.editing = editing;
                view.scale = scale;
                view.in = 0;
                view.out = 60;
                const float w = width * scale;
                const auto l = computeLayout(w, 600 * scale, view, clip);
                const D2D1_RECT_F* all[] = {&l.play, &l.time, &l.track, &l.mute, &l.volume, &l.trim, &l.folder, &l.fullscreen,
                                            &l.save, &l.replace, &l.cancel, &l.compress, &l.group};
                bool sound = true;
                for (const auto* r : all) sound = sound && sane(*r);
                for (int i = 0; i < l.sourceCount; ++i) sound = sound && sane(l.sources[i]);
                check(sound, "no rectangle is inverted");
                if (editing) {
                    check(!empty(l.save) && !empty(l.replace) && !empty(l.cancel), "trim buttons are shown");
                    check(!overlap(l.save, l.replace) && !overlap(l.replace, l.cancel), "trim buttons do not overlap");
                    check(!overlap(l.compress, l.cancel) && !overlap(l.compress, l.group), "compress slot clear of cancel and switches");
                    check(!overlap(l.compress, l.time), "compress slot clear of the time");
                    check(width < 960 || (l.compress.top >= l.controls.top && l.compress.bottom <= l.controls.bottom), "compress stays on the controls row at a typical width");
                }
                if (l.sourceCount > 0) {
                    check(l.sourceCount == 3, "one switch per source");
                    check(!overlap(l.group, l.time) && !overlap(l.group, l.cancel) && !overlap(l.group, l.mute), "switches clear of neighbours");
                    check(l.group.left >= l.time.right && l.group.right <= l.controls.right, "switches inside the controls row");
                    for (int i = 0; i < l.sourceCount; ++i) {
                        check(l.sources[i].left >= l.group.left && l.sources[i].right <= l.group.right, "switch inside its box");
                        if (i) check(!overlap(l.sources[i - 1], l.sources[i]) && l.sources[i].left - l.sources[i - 1].right >= 1.9f * scale, "switches 2 px apart");
                    }
                    check(sourceHits(l, view, clip) == l.sourceCount, "each switch hits its own index");
                    const float group = l.group.right - l.group.left;
                    check(group > 90 * scale && group < 105 * scale, "about 100 px for three");
                } else {
                    check(width < 700, "switches are only hidden when very narrow");
                }
            }
        }
    }

    {   // Normal width: switches shown while viewing and trimming; compress on the main row.
        View view;
        view.editing = true;
        const auto l = computeLayout(960, 600, view, clip);
        check(l.sourceCount == 3 && !empty(l.compress), "960 px trim bar shows switches and the compress slot");
        check(!l.compressShort, "full compress label at 960 px");
        check(l.compress.top >= l.controls.top && l.compress.bottom <= l.controls.bottom, "compress is in the controls row");
    }
    {   // Small width: the compress box moves to its own row, nothing overlaps.
        View view;
        view.editing = true;
        const auto l = computeLayout(600, 600, view, clip);
        check(l.compress.top >= l.strip.bottom && l.compress.bottom <= l.bar.bottom, "narrow: compress on its own row under the strip");
        check(l.sourceCount == 3 && !overlap(l.group, l.cancel), "narrow: switches remain");
    }
    {   // One source and a plain multi-track file.
        ClipInfo plain = clip;
        plain.toggles = false;
        plain.checked = {true, false, false};
        View view;
        const auto l = computeLayout(960, 600, view, plain);
        check(l.sourceCount == 0 && !empty(l.track), "plain multi-track files keep the pill");
        check(sane(l.track) && !overlap(l.track, l.mute), "pill never inverted");
        View tiny;
        const auto narrow = computeLayout(300, 400, tiny, clip);
        check(narrow.sourceCount == 0 && empty(narrow.group), "no room: switches hidden, not inverted");
    }
    {   // Pressing a switch reports its part; the part maps back to an index.
        check(sourceIndex(Part::Source0) == 0 && sourceIndex(Part::Source5) == 5 && sourceIndex(Part::Save) == -1, "source parts map to indices");
    }
    {
        View view;
        view.editing = true; view.in = 5; view.out = 55;
        const auto l = computeLayout(960, 600, view, clip);
        const float x = l.strip.left + (l.strip.right - l.strip.left) * static_cast<float>(view.in / clip.duration);
        const float y = (l.strip.top + l.strip.bottom) / 2;
        check(hitTest(l, view, clip, x, y) == Part::InHandle, "trim handle is draggable before export");
        view.compressing = view.saving = true;
        check(hitTest(l, view, clip, x, y) == Part::Timeline, "compression freezes handles but keeps preview seeking");
        check(hitTest(l, view, clip, (l.cancel.left + l.cancel.right) / 2,
            (l.cancel.top + l.cancel.bottom) / 2) == Part::Cancel, "compression keeps cancellation reachable");
    }
    return failures ? 1 : 0;
}
