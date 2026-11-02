// Layout of the player surface: the audio-source switches and the trim bar's buttons never overlap or invert.
#include "player/player_view.h"
#include <cmath>
#include <iostream>

using namespace refarter::player;

namespace {
bool empty(const D2D1_RECT_F& r) { return r.right <= r.left; }
float height(const D2D1_RECT_F& r) { return r.bottom - r.top; }
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
                    check(!overlap(l.mute, l.time) && !overlap(l.volume, l.time) && !overlap(l.mute, l.compress) && !overlap(l.volume, l.compress) &&
                              !overlap(l.volume, l.cancel) && !overlap(l.mute, l.volume), "volume controls clear of time, compress, cancel and each other");
                    if (l.volume.right > l.volume.left) check(hitTest(l, view, clip, (l.volume.left + l.volume.right) / 2, (l.volume.top + l.volume.bottom) / 2) == Part::Volume &&
                        hitTest(l, view, clip, (l.mute.left + l.mute.right) / 2, (l.mute.top + l.mute.bottom) / 2) == Part::Mute, "trim bar volume and mute are hit");
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
    {   // Narrower still: the switches have priority; the volume rail goes first, then the mute button, never the switches.
        View view;
        view.editing = true;
        bool railDropped = false;
        for (int width = 500; width <= 700; ++width) {
            const auto l = computeLayout(static_cast<float>(width), 600, view, clip);
            const bool group = !empty(l.group), mute = !empty(l.mute), rail = !empty(l.volume);
            check(!rail || mute, "the rail is dropped before the mute button");
            check(!mute || group, "the mute button never stays while the switches are hidden");
            check(!overlap(l.mute, l.group) && !overlap(l.volume, l.group) && !overlap(l.mute, l.time) && !overlap(l.volume, l.cancel) &&
                      !overlap(l.mute, l.compress) && !overlap(l.volume, l.compress), "narrow: nothing overlaps");
            railDropped = railDropped || (group && mute && !rail);
        }
        check(railDropped, "narrow: some width keeps the switches and the mute button without the rail");
    }
    {   // The player's real widths: the window's minimum (1080) and default (1120) client width less the expanded (196) or
        // collapsed (64) sidebar and two 24 px content gaps, and fullscreen. Trimming is one row: play, time, the switches (or
        // the pill), the mute button and rail, the compress slot (the short label allowed, never its own row) and the buttons.
        constexpr float minimumWidth = 1080, maxSwitches = 4; // desktop, microphone, discord and a game track (or game output)
        ClipInfo four = clip;
        four.tracks = {L"desktop", L"microphone", L"discord", L"game"};
        four.checked = {true, true, false, true};
        ClipInfo plainClip = clip;
        plainClip.toggles = false;
        plainClip.checked = {true, false, false};
        ClipInfo single = clip;
        single.tracks = {L"desktop"};
        single.checked = {true};
        single.toggles = false;
        const auto middle = [](const D2D1_RECT_F& r) { return (r.top + r.bottom) / 2; };
        struct Case { float width; bool fullscreen; };
        const Case cases[] = {{minimumWidth - 196 - 48, false}, {minimumWidth - 64 - 48, false}, {1120 - 196 - 48, false}, {1120 - 64 - 48, false},
                              {minimumWidth, true}, {1920, true}};
        for (const float scale : {1.f, 1.25f, 1.5f}) {
            for (const auto& [width, fullscreen] : cases) {
                for (const int kind : {0, 1, 2, 3}) { // Three switches, four, the pill, a single track.
                    const ClipInfo& info = kind == 0 ? clip : kind == 1 ? four : kind == 2 ? plainClip : single;
                    View view;
                    view.editing = true;
                    view.fullscreen = fullscreen;
                    view.scale = scale;
                    view.in = 0;
                    view.out = 60;
                    const auto l = computeLayout(width * scale, 700 * scale, view, info);
                    const D2D1_RECT_F& sources = kind < 2 ? l.group : l.track;
                    const float mid = middle(l.controls);
                    const D2D1_RECT_F* items[] = {&l.play, &l.time, &sources, &l.mute, &l.volume, &l.compress, &l.cancel, &l.replace, &l.save};
                    for (const auto* a : items) for (const auto* b : items) check(a == b || !overlap(*a, *b), "real widths: nothing overlaps");
                    for (const auto* r : items) check(sane(*r), "real widths: nothing is inverted");
                    check(std::abs(height(l.controls) - 52 * scale) < 1, "real widths: one row of controls");
                    for (const auto* r : items) check(empty(*r) || std::abs(middle(*r) - mid) < 1, "real widths: everything centred on the controls");
                    check(!empty(l.mute) && !empty(l.volume), "real widths: mute button and volume rail are shown");
                    check(!empty(l.compress) && l.compress.top >= l.controls.top && l.compress.bottom <= l.controls.bottom, "real widths: compress in the row");
                    check(l.mute.left >= sources.right && l.volume.left >= l.mute.right && l.compress.left >= l.volume.right, "real widths: sources, volume, compress in order");
                    if (kind == 0) check(l.sourceCount == 3 && !empty(l.group), "real widths: three switches shown");
                    if (kind == 1) check(l.sourceCount == maxSwitches && !empty(l.group), "real widths: four switches shown");
                    if (kind == 2) check(!empty(l.track) && l.sourceCount == 0, "real widths: the pill is shown");
                    if (kind == 3) check(empty(l.group) && empty(l.track), "real widths: a single track has no switches");
                    check(hitTest(l, view, info, (l.volume.left + l.volume.right) / 2, middle(l.volume)) == Part::Volume &&
                              hitTest(l, view, info, (l.mute.left + l.mute.right) / 2, middle(l.mute)) == Part::Mute, "real widths: volume and mute are hit");
                    if (kind == 1) check(hitTest(l, view, info, (l.sources[3].left + l.sources[3].right) / 2, middle(l.sources[3])) == Part::Source3, "real widths: the fourth switch is hit");
                    if (kind != 3 && width == minimumWidth - 196 - 48 && !fullscreen) check(l.compressShort, "the minimum window uses the short compress label");
                }
            }
        }
        {   // The minimum is tight: one pixel less and four switches no longer have the whole row.
            View view;
            view.editing = true;
            const auto l = computeLayout(minimumWidth - 196 - 48 - 1, 700, view, four);
            check(l.compress.top >= l.strip.bottom || empty(l.volume), "below the minimum something gives way");
            check(l.sourceCount == 4, "below the minimum the switches still stay");
        }
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
