#include "player/player_view.h"
#include "player/timeline.h"
#include "theme.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace refarter::player {

namespace {
D2D1_COLOR_F colour(COLORREF value, float alpha = 1.f) {
    return {GetRValue(value) / 255.f, GetGValue(value) / 255.f, GetBValue(value) / 255.f, alpha};
}
constexpr COLORREF stageColour = RGB(10, 11, 13);
constexpr COLORREF accentHover = RGB(255, 182, 138);
// The source switches: 28 x 26 px cells, 2 px apart, inside a bordered box with 3 px of padding.
constexpr float sourceCellWidth = 28, sourceCellHeight = 26, sourceCellGap = 2, sourceGroupPad = 3;
float sourceGroupWidth(int count) { return count * sourceCellWidth + (count - 1) * sourceCellGap + 2 * sourceGroupPad; }

float width(const D2D1_RECT_F& r) { return r.right - r.left; }
float height(const D2D1_RECT_F& r) { return r.bottom - r.top; }
float middle(const D2D1_RECT_F& r) { return (r.top + r.bottom) / 2; }
bool inside(const D2D1_RECT_F& r, float x, float y, float grow = 0) {
    return width(r) > 0 && x >= r.left - grow && x < r.right + grow && y >= r.top - grow && y < r.bottom + grow;
}
D2D1_RECT_F centred(float cx, float cy, float w, float h) { return {cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2}; }

TimelineScale seekScale(const Layout& l, const ClipInfo& s) { return {l.seek.left, width(l.seek), s.duration}; }
TimelineScale stripScale(const Layout& l, const ClipInfo& s) { return {l.strip.left, width(l.strip), s.duration}; }
} // namespace

Layout computeLayout(float w, float h, const View& view, const ClipInfo& status) {
    Layout l;
    const float s = view.scale;
    l.scale = s;
    const auto d = [s](float value) { return value * s; };
    l.card = {0, 0, w, h};
    // The trim bar's right side: save, replace, cancel and the "compress & copy" slot. The audio sources need room
    // between the time and that slot (the icon switches a fixed width, the pill of a plain multi-track file about 110 px):
    // shorten the compress label, then move its box to its own row.
    const float margin = view.fullscreen ? d(28) : d(12);
    const float fixedRight = d(72.f + 8 + 72 + 8 + 84 + 10), timeRight = margin + d(38.f + 14 + 132 + 8);
    const int sourceCount = status.toggles ? std::min<int>(static_cast<int>(status.tracks.size()), maxSources) : 0;
    const float groupWidth = sourceCount > 0 ? d(sourceGroupWidth(sourceCount)) : 0;
    const bool pill = !status.toggles && status.tracks.size() > 1;
    const float roomFor = (w - margin) - fixedRight - timeRight - (sourceCount > 0 ? groupWidth + d(8) : pill ? d(110) : 0);
    const bool ownRow = view.editing && roomFor < d(104);
    l.compressShort = view.editing && roomFor < d(142);
    const float controlsHeight = d(52), barHeight = view.editing ? d(ownRow ? 102.f : 70.f) : d(26);
    const float backSize = d(34);
    if (!view.fullscreen) {
        // A title bar with the way back, then the stage; the card's other rows keep their place.
        const float pad = d(12), headHeight = d(44);
        l.head = {0, 0, w, headHeight};
        l.back = centred(w - pad - backSize / 2, headHeight / 2, backSize, backSize);
        l.rename = centred(l.back.left - d(8) - backSize / 2, headHeight / 2, backSize, backSize);
        l.stage = {pad, headHeight, w - pad, std::max(headHeight + d(40), h - pad - controlsHeight - barHeight)};
        l.bar = {pad, l.stage.bottom, w - pad, l.stage.bottom + barHeight};
        l.controls = {pad, l.bar.bottom, w - pad, l.bar.bottom + controlsHeight};
    } else {
        // Fullscreen: a solid band over the top of the picture while the controls show.
        const float side = d(28), headHeight = d(64);
        l.head = {0, 0, w, headHeight};
        l.back = centred(w - side - backSize / 2, headHeight / 2, backSize, backSize);
        l.rename = centred(l.back.left - d(8) - backSize / 2, headHeight / 2, backSize, backSize);
        l.stage = {0, 0, w, h};
        l.controls = {side, h - d(10) - controlsHeight, w - side, h - d(10)};
        l.bar = {side, l.controls.top - barHeight, w - side, l.controls.top};
    }
    // The picture keeps its aspect ratio inside the stage.
    const float aspect = status.height > 0 ? static_cast<float>(status.width) / status.height : 16.f / 9;
    float vw = width(l.stage), vh = vw / aspect;
    if (vh > height(l.stage)) { vh = height(l.stage); vw = vh * aspect; }
    l.video = centred((l.stage.left + l.stage.right) / 2, middle(l.stage), std::floor(vw), std::floor(vh));
    l.video = {std::round(l.video.left), std::round(l.video.top), std::round(l.video.left) + std::floor(vw),
               std::round(l.video.top) + std::floor(vh)};

    l.seek = {l.bar.left + d(6), middle(l.bar) - d(10), l.bar.right - d(6), middle(l.bar) + d(10)};
    l.strip = {l.bar.left + d(14), l.bar.top + d(8), l.bar.right - d(14), l.bar.top + d(60)};

    const float cy = middle(l.controls);
    l.play = centred(l.controls.left + d(19), cy, d(38), d(38));
    l.time = {l.play.right + d(14), cy - d(12), l.play.right + d(14) + d(view.editing ? 132.f : 112.f), cy + d(12)};
    float x = l.controls.right;
    const float button = d(34);
    if (!view.editing) {
        l.fullscreen = {x - button, cy - button / 2, x, cy + button / 2};
        x -= button + d(4);
        l.folder = {x - button, cy - button / 2, x, cy + button / 2};
        x -= button + d(4);
        l.trim = {x - button, cy - button / 2, x, cy + button / 2};
        x -= button + d(14);
        l.volume = {x - d(80), cy - d(10), x, cy + d(10)};
        x -= d(80) + d(4);
        l.mute = {x - button, cy - button / 2, x, cy + button / 2};
        x -= button + d(10);
    } else {
        l.save = {x - d(72), cy - d(17), x, cy + d(17)};
        x -= d(72) + d(8);
        l.replace = {x - d(72), cy - d(17), x, cy + d(17)};
        x -= d(72) + d(8);
        l.cancel = {x - d(84), cy - d(17), x, cy + d(17)};
        x -= d(84) + d(10);
        if (ownRow) {
            l.compress = {l.strip.right - d(104), l.strip.bottom + d(4), l.strip.right, l.strip.bottom + d(38)};
        } else {
            const float slot = d(l.compressShort ? 104.f : 142.f);
            l.compress = {x - slot, cy - d(17), x, cy + d(17)};
            x -= slot + d(10);
        }
    }
    // Sources to keep while trimming, to play otherwise: icon switches, or the pill of a plain multi-track file.
    if (sourceCount > 0) {
        const float left = x - groupWidth;
        if (left >= l.time.right + d(8)) { // Otherwise hidden: no width, never inverted.
            l.group = {left, cy - d(16), x, cy + d(16)};
            l.sourceCount = sourceCount;
            for (int i = 0; i < sourceCount; ++i) {
                const float cell = left + d(sourceGroupPad) + i * d(sourceCellWidth + sourceCellGap);
                l.sources[i] = {cell, cy - d(sourceCellHeight) / 2, cell + d(sourceCellWidth), cy + d(sourceCellHeight) / 2};
            }
        }
    } else if (pill) {
        const float left = std::max(l.time.right + d(8), x - d(180));
        l.track = {left, cy - d(17), std::max(x, left), cy + d(17)}; // Never inverted; no width means hidden.
    }
    const float scx = (l.stage.left + l.stage.right) / 2, scy = middle(l.stage);
    l.errorOpen = centred(scx, scy + d(52), d(210), d(36));
    // The title, which a click renames: as wide as the name, never past the rename button.
    const float titleLeft = d(view.fullscreen ? 28.f : 16.f);
    const float titleRight = std::max(titleLeft, std::min(titleLeft + d(view.titleWidth), l.rename.left - d(12)));
    l.title = {titleLeft, middle(l.head) - d(14), titleRight, middle(l.head) + d(14)};
    return l;
}

Part hitTest(const Layout& l, const View& view, const ClipInfo& status, float x, float y) {
    const bool hidden = view.fullscreen && !view.controls;
    if (!hidden && inside(l.back, x, y)) return Part::Back;
    if (!hidden && inside(l.rename, x, y)) return Part::Rename; // Also for a clip that cannot be played.
    if (!hidden && inside(l.title, x, y)) return Part::Title;
    if (!status.error.empty()) return inside(l.errorOpen, x, y) ? Part::ErrorOpen : Part::None;
    if (hidden) return Part::Stage;
    const float s = l.scale;
    if (view.editing && inside(l.compress, x, y)) return Part::CompressCopy; // Maybe on its own row below the strip.
    if (view.editing && status.duration > 0) {
        const auto scale = stripScale(l, status);
        const float xi = static_cast<float>(scale.x(view.in)), xo = static_cast<float>(scale.x(view.out));
        const bool rows = y >= l.strip.top - 6 * s && y < l.strip.bottom + 6 * s;
        if (!view.compressing && rows && x >= xi - 14 * s && x <= xi + 4 * s && std::abs(x - xi) <= std::abs(x - xo)) return Part::InHandle;
        if (!view.compressing && rows && x >= xo - 4 * s && x <= xo + 14 * s) return Part::OutHandle;
        if (y >= l.strip.top - 6 * s && y < l.bar.bottom && x >= l.strip.left - 6 * s && x < l.strip.right + 6 * s) return Part::Timeline;
    } else if (inside(l.bar, x, y) && x >= l.seek.left - 6 * s && x < l.seek.right + 6 * s) {
        return Part::Seek;
    }
    const std::pair<const D2D1_RECT_F*, Part> buttons[] = {
        {&l.play, Part::Play}, {&l.track, Part::Track}, {&l.mute, Part::Mute}, {&l.trim, Part::Trim},
        {&l.folder, Part::Folder}, {&l.fullscreen, Part::Fullscreen}, {&l.save, Part::Save}, {&l.replace, Part::Replace}, {&l.cancel, Part::Cancel},
        {&l.compress, Part::CompressCopy}};
    for (const auto& [rect, part] : buttons) if (inside(*rect, x, y)) return part;
    for (int i = 0; i < l.sourceCount; ++i) if (inside(l.sources[i], x, y)) return static_cast<Part>(static_cast<int>(Part::Source0) + i);
    if (inside(l.volume, x, y, 6 * s)) return Part::Volume;
    if (inside(l.stage, x, y) && !(view.fullscreen && (y >= l.bar.top || y < l.head.bottom))) return Part::Stage;
    return Part::None;
}

double timeAt(const Layout& l, const View& view, const ClipInfo& status, float x) {
    return (view.editing ? stripScale(l, status) : seekScale(l, status)).time(x);
}

int thumbnailCount(const Layout& l, const ClipInfo& status, float& w, float& h) {
    h = std::round(height(l.strip));
    const float aspect = status.height > 0 ? std::clamp(static_cast<float>(status.width) / status.height, 0.5f, 4.f) : 16.f / 9;
    w = std::round(h * aspect);
    return w > 0 ? static_cast<int>(std::ceil(width(l.strip) / w)) : 0;
}

void Painter::prepare(ID2D1DeviceContext* dc, ID2D1Factory1* factory, IDWriteFactory* write, const Layout& layout) {
    if (dc != dc_) {
        // A new surface: everything made for the old one goes.
        dc_ = dc;
        brush_ = nullptr;
        shades_[0] = shades_[1] = nullptr;
        for (auto& geometry : geometry_) geometry = nullptr;
        scale_ = 0;
        layout_ = {};
    }
    if (!brush_) dc->CreateSolidColorBrush(colour(theme::ink), brush_.put());
    if (!round_) factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, round_.put());
    // Shapes follow the layout, so they stay valid until it moves.
    if (std::memcmp(&layout, &layout_, sizeof(Layout)) != 0) {
        layout_ = layout;
        for (auto& geometry : geometry_) geometry = nullptr;
    }
    const float scale = layout.scale;
    if (scale == scale_) return;
    scale_ = scale;
    widths_.clear();
    const auto make = [&](winrt::com_ptr<IDWriteTextFormat>& format, float size, DWRITE_FONT_WEIGHT weight) {
        format = nullptr;
        write->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                size * scale, L"en-gb", format.put());
        if (!format) return;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        winrt::com_ptr<IDWriteInlineObject> ellipsis;
        write->CreateEllipsisTrimmingSign(format.get(), ellipsis.put());
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        format->SetTrimming(&trimming, ellipsis.get());
    };
    make(small_, 11.5f, DWRITE_FONT_WEIGHT_NORMAL);
    make(date_, 12.f, DWRITE_FONT_WEIGHT_NORMAL);
    make(body_, 13.5f, DWRITE_FONT_WEIGHT_NORMAL);
    make(strong_, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    make(title_, 17.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
}

// Widths of strings seen before come from the cache; a layout is made only for new ones.
float Painter::textWidth(IDWriteFactory* write, IDWriteTextFormat* format, const std::wstring& value) {
    if (!format) return 0;
    const auto key = std::make_pair(format, value);
    if (const auto found = widths_.find(key); found != widths_.end()) return found->second;
    winrt::com_ptr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(write->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()), format, 10000, 100, layout.put())))
        layout->GetMetrics(&metrics);
    if (widths_.size() > 256) widths_.clear(); // Times change as a clip plays.
    widths_.emplace(key, metrics.widthIncludingTrailingWhitespace);
    return metrics.widthIncludingTrailingWhitespace;
}

void Painter::draw(ID2D1DeviceContext* dc, IDWriteFactory* write, ID2D1Factory1* factory, const Layout& l,
                   const View& view, const ClipInfo& status, const Playback& playback, ID2D1Bitmap1* picture, const std::vector<Thumbnail>& thumbnails) {
    prepare(dc, factory, write, l);
    const float s = view.scale;
    const auto d = [s](float value) { return value * s; };
    auto* brush = brush_.get();
    const auto paint = [&](D2D1_COLOR_F c) { brush->SetColor(c); return brush; };
    const auto rounded = [&](D2D1_RECT_F r, float radius, D2D1_COLOR_F fill) {
        dc->FillRoundedRectangle({r, radius, radius}, paint(fill));
    };
    const auto outline = [&](D2D1_RECT_F r, float radius, D2D1_COLOR_F stroke, float thickness = 1.f) {
        const D2D1_RECT_F inset{r.left + thickness / 2, r.top + thickness / 2, r.right - thickness / 2, r.bottom - thickness / 2};
        dc->DrawRoundedRectangle({inset, radius, radius}, paint(stroke), thickness);
    };
    const auto text = [&](const std::wstring& value, IDWriteTextFormat* format, D2D1_RECT_F r, D2D1_COLOR_F c,
                          DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!format || value.empty()) return;
        format->SetTextAlignment(align);
        dc->DrawText(value.c_str(), static_cast<UINT32>(value.size()), format, r, paint(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    const auto measure = [&](const std::wstring& value, IDWriteTextFormat* format) { return textWidth(write, format, value); };
    // Consecutive text runs in different colours on one line.
    const auto runs = [&](std::initializer_list<std::pair<std::wstring, D2D1_COLOR_F>> parts, IDWriteTextFormat* format,
                          D2D1_RECT_F r, bool centre) {
        float total = 0;
        for (const auto& [value, c] : parts) total += measure(value, format);
        float x = centre ? (r.left + r.right - total) / 2 : r.left;
        for (const auto& [value, c] : parts) {
            const float w = measure(value, format);
            text(value, format, {x, r.top, std::min(r.right, x + w + 1), r.bottom}, c);
            x += w;
        }
    };
    // A path built once per layout: `build` fills the sink the first time `which` is asked for.
    const auto cached = [&](Geometry which, const auto& build) -> ID2D1Geometry* {
        auto& slot = geometry_[which];
        if (!slot) {
            winrt::com_ptr<ID2D1PathGeometry> path;
            winrt::com_ptr<ID2D1GeometrySink> sink;
            if (FAILED(factory->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) return nullptr;
            build(sink.get());
            if (FAILED(sink->Close())) return nullptr;
            slot = path;
        }
        return slot.get();
    };
    const auto polygon = [&](Geometry which, std::initializer_list<D2D1_POINT_2F> points, D2D1_COLOR_F c) {
        const auto geometry = cached(which, [&](ID2D1GeometrySink* sink) {
            auto it = points.begin();
            sink->BeginFigure(*it, D2D1_FIGURE_BEGIN_FILLED);
            for (++it; it != points.end(); ++it) sink->AddLine(*it);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        });
        if (geometry) dc->FillGeometry(geometry, paint(c));
    };
    const auto line = [&](float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float thickness) {
        dc->DrawLine({x1, y1}, {x2, y2}, paint(c), thickness);
    };
    const bool hovered = view.pressed == Part::None;
    const auto active = [&](Part part) { return view.hover == part && (hovered || view.pressed == part); };
    const D2D1_COLOR_F ink = colour(theme::ink), muted = colour(theme::muted), accent = colour(theme::accent);

    // Title bar: the clip's name and date, the way back at the right (and the fullscreen hint before it).
    const auto header = [&] {
        const float left = d(view.fullscreen ? 28.f : 16.f), gap = d(12);
        float right = l.rename.left - gap;
        if (view.fullscreen) {
            const std::wstring hint = L"esc exits fullscreen";
            const float hintRight = right - d(4), hintLeft = hintRight - measure(hint, small_.get()) - 1;
            text(hint, small_.get(), {hintLeft, l.head.top, hintRight, l.head.bottom}, muted);
            right = hintLeft - gap;
        }
        const float dateWidth = view.date.empty() ? 0 : measure(view.date, date_.get()) + 1;
        const float available = std::max(0.f, right - left - (dateWidth > 0 ? dateWidth + gap : 0));
        const float titleWidth = std::min(measure(view.title, title_.get()) + 1, available);
        if (active(Part::Title) || view.pressed == Part::Title) {
            // Like a text field under the mouse: a click edits the name.
            rounded({left - d(8), l.title.top, left + titleWidth + d(8), l.title.bottom}, d(6), colour(theme::raised));
        }
        text(view.title, title_.get(), {left, l.head.top, left + titleWidth, l.head.bottom}, ink);
        if (dateWidth > 0 && left + titleWidth + gap + dateWidth <= right)
            text(view.date, date_.get(), {left + titleWidth + gap, l.head.top, right, l.head.bottom}, muted);

        const bool lit = active(Part::Back) || view.pressed == Part::Back;
        rounded(l.back, d(7), colour(lit ? theme::raised : theme::surface));
        outline(l.back, d(7), colour(theme::border));
        // A return arrow, drawn on a 24 unit grid at 18 px.
        const float k = d(18) / 24, ox = (l.back.left + l.back.right) / 2 - d(9), oy = middle(l.back) - d(9);
        const auto at = [&](float x, float y) { return D2D1_POINT_2F{ox + x * k, oy + y * k}; };
        const auto arrow = cached(Arrow, [&](ID2D1GeometrySink* sink) {
            sink->BeginFigure(at(9, 14), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(at(4, 9));
            sink->AddLine(at(9, 4));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->BeginFigure(at(4, 9), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(at(14, 9));
            sink->AddArc({at(14, 21), {6 * k, 6 * k}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
            sink->AddLine(at(11, 21));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
        });
        if (arrow && round_) dc->DrawGeometry(arrow, paint(lit ? ink : muted), 1.6f * k, round_.get());

        // Rename: a pencil lying diagonally, tip at the bottom left (as on the library's tiles).
        const bool named = active(Part::Rename) || view.pressed == Part::Rename;
        rounded(l.rename, d(7), colour(named ? theme::raised : theme::surface));
        outline(l.rename, d(7), colour(theme::border));
        const float pencilX = (l.rename.left + l.rename.right) / 2, pencilY = middle(l.rename), u = d(1.15f);
        const auto on = [&](float x, float y) { return D2D1_POINT_2F{pencilX + x * u, pencilY + y * u}; };
        const auto pencil = cached(Pencil, [&](ID2D1GeometrySink* sink) {
            sink->BeginFigure(on(-5.5f, 5.5f), D2D1_FIGURE_BEGIN_HOLLOW);
            for (const auto point : {on(-1.8f, 4.3f), on(6.7f, -4.2f), on(4.2f, -6.7f), on(-4.3f, 1.8f)}) sink->AddLine(point);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->BeginFigure(on(-4.3f, 1.8f), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(on(-1.8f, 4.3f));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->BeginFigure(on(2.1f, -4.6f), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(on(4.6f, -2.1f));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
        });
        if (pencil && round_) dc->DrawGeometry(pencil, paint(named ? ink : muted), 1.4f * u, round_.get());
    };

    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    if (!view.fullscreen) {
        rounded(l.card, d(10), colour(theme::library));
        outline(l.card, d(10), colour(theme::border));
        rounded(l.stage, d(8), colour(stageColour));
        header();
    } else {
        dc->FillRectangle(l.stage, paint({0, 0, 0, 1}));
    }

    if (picture) {
        winrt::com_ptr<ID2D1BitmapBrush1> pictureBrush;
        if (SUCCEEDED(dc->CreateBitmapBrush(picture, pictureBrush.put()))) {
            pictureBrush->SetTransform(D2D1::Matrix3x2F::Translation(l.video.left, l.video.top));
            const float radius = view.fullscreen ? 0 : d(8);
            dc->FillRoundedRectangle({l.video, radius, radius}, pictureBrush.get());
        }
    }

    // Soft shades keep the title and controls legible over any picture.
    const auto shade = [&](float top, float bottom, float fromAlpha, float toAlpha) {
        // One brush per direction (title shade fades out, controls shade fades in), moved into place.
        auto& gradient = shades_[fromAlpha > toAlpha ? 0 : 1];
        if (!gradient) {
            const D2D1_GRADIENT_STOP stops[] = {{0.f, {0, 0, 0, fromAlpha}}, {1.f, {0, 0, 0, toAlpha}}};
            winrt::com_ptr<ID2D1GradientStopCollection> collection;
            if (FAILED(dc->CreateGradientStopCollection(stops, 2, collection.put())) ||
                FAILED(dc->CreateLinearGradientBrush({{0, top}, {0, bottom}}, collection.get(), gradient.put()))) return;
        }
        gradient->SetStartPoint({0, top});
        gradient->SetEndPoint({0, bottom});
        dc->FillRectangle({l.stage.left, top, l.stage.right, bottom}, gradient.get());
    };
    if (view.fullscreen && view.controls) {
        shade(0, d(84), 0.7f, 0.f);
        header();
    }

    const float scx = (l.stage.left + l.stage.right) / 2, scy = middle(l.stage);
    if (!status.error.empty()) {
        text(L"this clip can't be played here.", title_.get(), {l.stage.left + d(20), scy - d(40), l.stage.right - d(20), scy - d(10)}, ink, DWRITE_TEXT_ALIGNMENT_CENTER);
        text(status.error, body_.get(), {l.stage.left + d(20), scy - d(8), l.stage.right - d(20), scy + d(18)}, muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        rounded(l.errorOpen, d(7), colour(active(Part::ErrorOpen) ? theme::raised : theme::surface));
        outline(l.errorOpen, d(7), colour(active(Part::ErrorOpen) ? theme::raised : theme::border));
        text(L"open in default player", body_.get(), l.errorOpen, ink, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    if (!status.ready) {
        text(L"opening…", body_.get(), {l.stage.left, scy - d(12), l.stage.right, scy + d(12)}, muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    // A toast over the picture: trim progress, or the message raised with its kind.
    const bool progressKind = view.saving || view.toastKind == ToastKind::Progress;
    const std::wstring toast = view.saving ? std::wstring(view.compressing ? L"compressing · " : L"saving trimmed clip · ") + std::to_wstring(static_cast<int>(view.progress * 100)) + L"%" : view.toast;
    if (!toast.empty() && (!view.fullscreen || view.controls)) {
        // A card at the top centre of the picture: a round icon of the toast's kind, the text before " · " in
        // bold and the rest under it. Progress has no second line: its percentage ends the title line, with a
        // thin bar below.
        const size_t dot = toast.find(L" · ");
        const std::wstring head = toast.substr(0, dot), sub = dot == std::wstring::npos ? L"" : toast.substr(dot + 3);
        float fraction = -1;
        if (progressKind) fraction = view.saving ? view.progress : !sub.empty() && sub.back() == L'%' ? static_cast<float>(_wtoi(sub.c_str())) / 100.f : 0.f;
        const float lines = d(sub.empty() || progressKind ? 18.f : 36.f), bar = progressKind ? d(11.f) : 0.f;
        const float wide = progressKind ? measure(head, strong_.get()) + d(12.f) + measure(sub, body_.get()) : std::max(measure(head, strong_.get()), measure(sub, body_.get()));
        const float textW = std::min(std::max(d(230.f - 66.f), wide + 1), width(l.stage) - d(32.f + 66.f));
        const auto card = centred(scx, l.stage.top + d(view.fullscreen ? 94.f : 20.f) + (std::max(d(26.f), lines + bar) + d(24.f)) / 2,
                                  textW + d(66.f), std::max(d(26.f), lines + bar) + d(24.f));
        for (int i = 1; i <= 5; ++i) { // A soft shadow: a few faint, growing, lowered layers.
            const float grow = d(2.5f * static_cast<float>(i));
            rounded({card.left - grow, card.top - grow + d(8), card.right + grow, card.bottom + grow + d(8)}, d(10) + grow, colour(RGB(0, 0, 0), 0.07f));
        }
        rounded(card, d(10), colour(theme::raised));
        outline(card, d(10), colour(theme::border));
        const D2D1_POINT_2F c{card.left + d(16 + 13), middle(card)};
        const D2D1_COLOR_F dark = colour(theme::background);
        const auto stroke = [&](float x1, float y1, float x2, float y2, D2D1_COLOR_F colourOf, float thickness) {
            dc->DrawLine({c.x + d(x1), c.y + d(y1)}, {c.x + d(x2), c.y + d(y2)}, paint(colourOf), d(thickness), round_.get());
        };
        const auto point = [&](float y, D2D1_COLOR_F colourOf) { dc->FillEllipse({{c.x, c.y + d(y)}, d(1.3f), d(1.3f)}, paint(colourOf)); };
        if (progressKind) { // A ring filling clockwise from 12 o'clock.
            const float r = d(13.f - 1.5f), turn = std::clamp(fraction, 0.f, 1.f);
            dc->DrawEllipse({c, r, r}, paint(colour(theme::border)), d(3.f));
            if (turn >= 0.999f) {
                dc->DrawEllipse({c, r, r}, paint(accent), d(3.f));
            } else if (turn > 0.f) {
                winrt::com_ptr<ID2D1PathGeometry> arc;
                winrt::com_ptr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(factory->CreatePathGeometry(arc.put())) && SUCCEEDED(arc->Open(sink.put()))) {
                    const float angle = turn * 6.2831853f;
                    sink->BeginFigure({c.x, c.y - r}, D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddArc({{c.x + r * std::sin(angle), c.y - r * std::cos(angle)}, {r, r}, 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  turn > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL});
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    if (SUCCEEDED(sink->Close())) dc->DrawGeometry(arc.get(), paint(accent), d(3.f), round_.get());
                }
            }
        } else if (view.toastKind == ToastKind::Notice) { // "i" in an accent-edged disc.
            dc->FillEllipse({c, d(13), d(13)}, paint(colour(theme::border)));
            dc->DrawEllipse({c, d(12.25f), d(12.25f)}, paint(accent), d(1.5f));
            point(-5.f, ink);
            stroke(0, -1.5f, 0, 5.5f, ink, 1.9f);
        } else if (view.toastKind == ToastKind::Partial) { // "!" on accent.
            dc->FillEllipse({c, d(13), d(13)}, paint(accent));
            stroke(0, -6.5f, 0, 1.5f, dark, 1.9f);
            point(5.5f, dark);
        } else if (view.toastKind == ToastKind::Error) { // A cross on soft red.
            dc->FillEllipse({c, d(13), d(13)}, paint(colour(theme::danger)));
            stroke(-4, -4, 4, 4, dark, 1.9f);
            stroke(4, -4, -4, 4, dark, 1.9f);
        } else { // A tick on accent.
            dc->FillEllipse({c, d(13), d(13)}, paint(accent));
            const auto tick = [&](float x, float y) { return D2D1_POINT_2F{c.x - d(7) + x * d(14) / 24, c.y - d(7) + y * d(14) / 24}; };
            dc->DrawLine(tick(5, 12.5f), tick(9.5f, 17), paint(dark), d(1.9f), round_.get());
            dc->DrawLine(tick(9.5f, 17), tick(19, 7.5f), paint(dark), d(1.9f), round_.get());
        }
        const float textLeft = card.left + d(16 + 26 + 12), top = card.top + d(12) + (std::max(d(26.f), lines + bar) - lines - bar) / 2;
        text(head, strong_.get(), {textLeft, top, textLeft + textW, top + d(18)}, ink);
        if (progressKind) text(sub, body_.get(), {textLeft, top, textLeft + textW, top + d(18)}, muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
        else text(sub, body_.get(), {textLeft, top + d(18), textLeft + textW, top + d(36)}, muted);
        if (progressKind) {
            const float barTop = top + lines + d(7);
            rounded({textLeft, barTop, textLeft + textW, barTop + d(4)}, d(2), colour(theme::border));
            rounded({textLeft, barTop, textLeft + textW * std::clamp(fraction, 0.f, 1.f), barTop + d(4)}, d(2), accent);
        }
    }

    if (view.fullscreen) {
        if (!view.controls) return;
        shade(l.bar.top - d(60), l.stage.bottom, 0.f, 0.82f);
    }
    const D2D1_COLOR_F rail = view.fullscreen ? D2D1_COLOR_F{1, 1, 1, 0.22f} : colour(theme::border);
    const D2D1_COLOR_F buttonFill = view.fullscreen ? D2D1_COLOR_F{1, 1, 1, 0.12f} : colour(theme::raised);
    const D2D1_COLOR_F pillFill = view.fullscreen ? D2D1_COLOR_F{0.07f, 0.075f, 0.09f, 0.72f} : colour(theme::surface);
    const D2D1_COLOR_F pillStroke = view.fullscreen ? D2D1_COLOR_F{1, 1, 1, 0.14f} : colour(theme::border);
    const auto bubble = [&](double time, float x, float bottom) {
        const auto label = formatTime(time, view.editing);
        const float w = measure(label, small_.get()) + d(16);
        const float left = std::clamp(x - w / 2, l.bar.left, l.bar.right - w);
        const D2D1_RECT_F r{left, bottom - d(22), left + w, bottom};
        rounded(r, d(6), colour(theme::raised));
        outline(r, d(6), colour(theme::border));
        text(label, small_.get(), r, ink, DWRITE_TEXT_ALIGNMENT_CENTER);
    };

    if (!view.editing) {
        // Seek bar: thin rail, accent progress, a knob while it is in use.
        const auto scale = seekScale(l, status);
        const bool using_ = active(Part::Seek) || view.pressed == Part::Seek;
        const float thickness = using_ ? d(6) : d(4), y = middle(l.seek);
        const float px = static_cast<float>(scale.x(playback.position));
        rounded({l.seek.left, y - thickness / 2, l.seek.right, y + thickness / 2}, thickness / 2, rail);
        if (px > l.seek.left) rounded({l.seek.left, y - thickness / 2, px, y + thickness / 2}, thickness / 2, accent);
        if (using_) {
            if (view.hoverTime >= 0 && view.pressed != Part::Seek) {
                const float hx = static_cast<float>(scale.x(view.hoverTime));
                line(hx, y - thickness / 2, hx, y + thickness / 2, ink, d(2));
                bubble(view.hoverTime, hx, y - d(10));
            }
            dc->FillEllipse({{px, y}, d(7), d(7)}, paint(accent));
            dc->DrawEllipse({{px, y}, d(7), d(7)}, paint(colour(stageColour, 0.6f)), d(1.5f));
            if (view.pressed == Part::Seek) bubble(playback.position, px, y - d(12));
        }
    } else {
        // Trim timeline: thumbnails, the kept range between accent handles, the playhead.
        const auto scale = stripScale(l, status);
        const float xi = static_cast<float>(scale.x(view.in)), xo = static_cast<float>(scale.x(view.out));
        const float xp = static_cast<float>(scale.x(playback.position));
        auto& clip = geometry_[StripClip];
        if (!clip) {
            winrt::com_ptr<ID2D1RoundedRectangleGeometry> shape;
            factory->CreateRoundedRectangleGeometry({l.strip, d(6), d(6)}, shape.put());
            clip = shape;
        }
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip.get()), nullptr);
        dc->FillRectangle(l.strip, paint(colour(stageColour)));
        float tw = 0, th = 0;
        const int count = thumbnailCount(l, status, tw, th);
        for (const auto& thumbnail : thumbnails) {
            if (!thumbnail.bitmap || count <= 0 || status.duration <= 0) continue;
            const int index = std::clamp(static_cast<int>(thumbnail.time / status.duration * count), 0, count - 1);
            const float x = l.strip.left + index * tw;
            dc->DrawBitmap(thumbnail.bitmap.get(), {x, l.strip.top, x + tw, l.strip.top + th}, 1.f, D2D1_INTERPOLATION_MODE_LINEAR);
        }
        dc->FillRectangle({l.strip.left, l.strip.top, xi, l.strip.bottom}, paint({0, 0, 0, 0.64f}));
        dc->FillRectangle({xo, l.strip.top, l.strip.right, l.strip.bottom}, paint({0, 0, 0, 0.64f}));
        dc->PopLayer();
        // Keyframes, where a trimmed copy can start.
        for (const double key : status.keyframes) {
            const float x = static_cast<float>(scale.x(key));
            line(x, l.strip.bottom + d(3), x, l.strip.bottom + d(6), colour(theme::muted, 0.45f), std::max(1.f, d(1)));
        }
        outline({xi, l.strip.top, xo, l.strip.bottom}, d(4), accent, d(2));
        const auto handle = [&](float x, bool left, Part part) {
            const auto r = left ? D2D1_RECT_F{x - d(12), l.strip.top, x + d(2), l.strip.bottom}
                                : D2D1_RECT_F{x - d(2), l.strip.top, x + d(12), l.strip.bottom};
            const bool lit = active(part) || view.pressed == part;
            rounded(r, d(5), colour(lit ? accentHover : theme::accent));
            const float cx = left ? x - d(5) : x + d(5);
            line(cx - d(1.5f), middle(r) - d(7), cx - d(1.5f), middle(r) + d(7), colour(theme::background, 0.8f), d(1.4f));
            line(cx + d(1.5f), middle(r) - d(7), cx + d(1.5f), middle(r) + d(7), colour(theme::background, 0.8f), d(1.4f));
        };
        handle(xi, true, Part::InHandle);
        handle(xo, false, Part::OutHandle);
        if (view.hoverTime >= 0 && (active(Part::Timeline)) && view.pressed == Part::None) {
            const float hx = static_cast<float>(scale.x(view.hoverTime));
            line(hx, l.strip.top, hx, l.strip.bottom, {1, 1, 1, 0.45f}, d(1));
        }
        line(xp, l.strip.top - d(5), xp, l.strip.bottom + d(5), ink, d(2));
        dc->FillEllipse({{xp, l.strip.top - d(6)}, d(4.5f), d(4.5f)}, paint(ink));
        if (view.pressed == Part::Timeline || view.pressed == Part::InHandle || view.pressed == Part::OutHandle)
            bubble(playback.position, xp, l.strip.top - d(10));
    }

    // Controls row.
    const bool playing = playback.playing;
    const float pcx = (l.play.left + l.play.right) / 2, pcy = middle(l.play);
    dc->FillEllipse({{pcx, pcy}, width(l.play) / 2, width(l.play) / 2}, paint(colour(active(Part::Play) ? accentHover : theme::accent)));
    const auto dark = colour(theme::background);
    if (playing) {
        rounded({pcx - d(6), pcy - d(7), pcx - d(2), pcy + d(7)}, d(1), dark);
        rounded({pcx + d(2), pcy - d(7), pcx + d(6), pcy + d(7)}, d(1), dark);
    } else {
        polygon(Play, {{pcx - d(4), pcy - d(8)}, {pcx + d(8), pcy}, {pcx - d(4), pcy + d(8)}}, dark);
    }
    runs({{formatTime(playback.position, view.editing), ink}, {L"  /  " + formatTime(status.duration, view.editing), muted}},
         body_.get(), l.time, false);

    const auto iconButton = [&](const D2D1_RECT_F& r, Part part) {
        if (active(part) || view.pressed == part) rounded(r, d(8), buttonFill);
        return active(part) ? ink : (view.fullscreen ? ink : muted);
    };
    const auto pill = [&](const D2D1_RECT_F& r, Part part, bool primary) {
        const bool lit = active(part) || view.pressed == part;
        if (primary) rounded(r, d(7), colour(view.saving ? theme::raised : lit ? accentHover : theme::accent));
        else {
            rounded(r, d(7), lit ? buttonFill : pillFill);
            outline(r, d(7), lit ? buttonFill : pillStroke);
        }
    };

    if (width(l.track) > 0) {
        pill(l.track, Part::Track, false);
        const auto& name = status.audioLabel;
        runs({{L"audio  ", muted}, {name, ink}}, body_.get(), {l.track.left + d(12), l.track.top, l.track.right - d(26), l.track.bottom}, false);
        const float cx = l.track.right - d(15), cy = middle(l.track);
        line(cx - d(4), cy - d(2), cx, cy + d(2), muted, d(1.5f));
        line(cx, cy + d(2), cx + d(4), cy - d(2), muted, d(1.5f));
    }
    if (l.sourceCount > 0) {
        rounded(l.group, d(8), pillFill);
        outline(l.group, d(8), pillStroke);
        for (int i = 0; i < l.sourceCount; ++i) {
            const auto part = static_cast<Part>(static_cast<int>(Part::Source0) + i);
            const auto& cell = l.sources[i];
            const bool on = i < static_cast<int>(status.checked.size()) && status.checked[i];
            const bool lit = active(part) || view.pressed == part;
            const bool included = on && i < static_cast<int>(status.included.size()) && status.included[i];
            if (included) outline(cell, d(6), colour(theme::accent, lit ? 0.5f : 0.3f)); // Linked, not switched: outline, no fill.
            else if (on) rounded(cell, d(6), colour(theme::accent, lit ? 0.32f : 0.2f));
            else if (lit) rounded(cell, d(6), buttonFill);
            const auto c = included ? colour(theme::accent, 0.6f) : on ? accent : lit ? ink : muted;
            const float cx = (cell.left + cell.right) / 2, cy = middle(cell), stroke = d(1.5f);
            const auto seg = [&](float x1, float y1, float x2, float y2) { line(cx + d(x1), cy + d(y1), cx + d(x2), cy + d(y2), c, stroke); };
            const auto box = [&](float x1, float y1, float x2, float y2, float radius) {
                dc->DrawRoundedRectangle({{cx + d(x1), cy + d(y1), cx + d(x2), cy + d(y2)}, d(radius), d(radius)}, paint(c), stroke);
            };
            const auto& name = status.tracks[i];
            if (name == L"desktop") {         // Monitor on its stand.
                box(-7, -6, 7, 3, 1.5f);
                seg(0, 3, 0, 6); seg(-3.5f, 6.5f, 3.5f, 6.5f);
            } else if (name == L"microphone") { // Capsule, cradle, stem.
                box(-2.5f, -7, 2.5f, 1, 2.5f);
                seg(-5, -1, -5, 1); seg(-5, 1, -3, 4); seg(-3, 4, 3, 4); seg(3, 4, 5, 1); seg(5, 1, 5, -1);
                seg(0, 4, 0, 7); seg(-3, 7, 3, 7);
            } else if (name == L"discord") {  // A headset (not Discord's logo): band, two ear cups, boom.
                seg(-6, 0, -6, -2); seg(-6, -2, -4, -5); seg(-4, -5, 0, -6.5f); seg(0, -6.5f, 4, -5); seg(4, -5, 6, -2); seg(6, -2, 6, 0);
                box(-8, 0, -4.5f, 5, 1.5f); box(4.5f, 0, 8, 5, 1.5f);
                seg(6, 5, 4, 7.5f); seg(4, 7.5f, 0, 7.5f);
            } else if (name == L"game" || name == L"game output") {     // Gamepad: body, d-pad, two buttons.
                box(-8, -4.5f, 8, 5, 4);
                seg(-5, 0.5f, -2, 0.5f); seg(-3.5f, -1, -3.5f, 2);
                dc->FillEllipse({{cx + d(3), cy - d(1)}, d(1.1f), d(1.1f)}, paint(c));
                dc->FillEllipse({{cx + d(5.5f), cy + d(1.8f)}, d(1.1f), d(1.1f)}, paint(c));
            } else {
                dc->DrawEllipse({{cx, cy}, d(6), d(6)}, paint(c), stroke);
            }
            if (!on) seg(-8, -8, 8, 8); // Off: a diagonal slash through the glyph.
        }
        // Hint over the hovered switch: "<name> on" / "<name> off".
        const int hovered_ = sourceIndex(view.hover);
        if (hovered_ >= 0 && hovered_ < l.sourceCount && view.pressed == Part::None) {
            const bool on = hovered_ < static_cast<int>(status.checked.size()) && status.checked[hovered_];
            const bool included = on && hovered_ < static_cast<int>(status.included.size()) && status.included[hovered_];
            const auto label = included ? status.tracks[hovered_] + L" · part of desktop audio" : status.tracks[hovered_] + (on ? L" on" : L" off");
            const float w = measure(label, small_.get()) + d(16);
            const float right = std::min(l.group.right, l.controls.right);
            const D2D1_RECT_F r{right - w, l.group.top - d(30), right, l.group.top - d(8)};
            rounded(r, d(6), colour(theme::raised));
            outline(r, d(6), colour(theme::border));
            text(label, small_.get(), r, ink, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    if (!view.editing) {
        {   // Speaker with waves for the level; a cross when muted.
            const auto c = iconButton(l.mute, Part::Mute);
            const float cx = (l.mute.left + l.mute.right) / 2 - d(3), cy = middle(l.mute);
            polygon(Speaker, {{cx - d(7), cy - d(3)}, {cx - d(3), cy - d(3)}, {cx + d(2), cy - d(7)}, {cx + d(2), cy + d(7)},
                     {cx - d(3), cy + d(3)}, {cx - d(7), cy + d(3)}}, c);
            const int level = view.muted || view.volume == 0 ? -1 : view.volume < 40 ? 0 : 1;
            if (level < 0) {
                line(cx + d(5), cy - d(3), cx + d(11), cy + d(3), c, d(1.5f));
                line(cx + d(11), cy - d(3), cx + d(5), cy + d(3), c, d(1.5f));
            } else {
                for (int wave = 0; wave <= level; ++wave) {
                    const float r = d(5.f + wave * 4.f);
                    const auto arc = cached(wave ? WaveFar : WaveNear, [&](ID2D1GeometrySink* sink) {
                        sink->BeginFigure({cx + d(2) + r * 0.6f, cy - r * 0.8f}, D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddArc({{cx + d(2) + r * 0.6f, cy + r * 0.8f}, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    });
                    if (arc) dc->DrawGeometry(arc, paint(c), d(1.5f));
                }
            }
        }
        {   // Volume rail.
            const float y = middle(l.volume), level = view.muted ? 0.f : view.volume / 100.f;
            const float x = l.volume.left + width(l.volume) * level;
            rounded({l.volume.left, y - d(2), l.volume.right, y + d(2)}, d(2), rail);
            if (x > l.volume.left) rounded({l.volume.left, y - d(2), x, y + d(2)}, d(2), view.muted ? muted : accent);
            const float knob = active(Part::Volume) || view.pressed == Part::Volume ? d(6.5f) : d(5);
            dc->FillEllipse({{x, y}, knob, knob}, paint(view.muted ? muted : accent));
        }
        {   // Trim: scissors in the accent colour.
            iconButton(l.trim, Part::Trim);
            const float cx = (l.trim.left + l.trim.right) / 2, cy = middle(l.trim) - d(1);
            dc->DrawEllipse({{cx - d(4), cy + d(5)}, d(2.6f), d(2.6f)}, paint(accent), d(1.5f));
            dc->DrawEllipse({{cx + d(4), cy + d(5)}, d(2.6f), d(2.6f)}, paint(accent), d(1.5f));
            line(cx - d(2.4f), cy + d(3), cx + d(4), cy - d(7), accent, d(1.5f));
            line(cx + d(2.4f), cy + d(3), cx - d(4), cy - d(7), accent, d(1.5f));
        }
        {   // Open in folder: a folder outline with a raised tab.
            const auto c = iconButton(l.folder, Part::Folder);
            const float cx = (l.folder.left + l.folder.right) / 2, cy = middle(l.folder);
            const auto stroke = [&](float x1, float y1, float x2, float y2) { line(cx + d(x1), cy + d(y1), cx + d(x2), cy + d(y2), c, d(1.5f)); };
            stroke(-8, 6, -8, -6); stroke(-8, -6, -2, -6); stroke(-2, -6, 0, -3);
            stroke(0, -3, 8, -3); stroke(8, -3, 8, 6); stroke(8, 6, -8, 6);
            stroke(-8, -1, 8, -1);
        }
        {   // Fullscreen corners (inward while fullscreen).
            const auto c = iconButton(l.fullscreen, Part::Fullscreen);
            const float cx = (l.fullscreen.left + l.fullscreen.right) / 2, cy = middle(l.fullscreen), r = d(7), k = d(4);
            for (int corner = 0; corner < 4; ++corner) {
                const float sx = corner % 2 ? 1.f : -1.f, sy = corner / 2 ? 1.f : -1.f;
                const float x = cx + sx * r, y = cy + sy * r;
                const float inward = view.fullscreen ? -1.f : 1.f;
                const float ox = view.fullscreen ? x - sx * k : x, oy = view.fullscreen ? y - sy * k : y;
                line(ox, oy, ox - sx * k * inward, oy, c, d(1.6f));
                line(ox, oy, ox, oy - sy * k * inward, c, d(1.6f));
            }
        }
    } else {
        {   // "compress & copy": a box that is ticked, unticked or (while a compression runs) unavailable.
            const bool lit = !view.compressBusy && (active(Part::CompressCopy) || view.pressed == Part::CompressCopy);
            const bool on = view.compressCopy && !view.compressBusy;
            const D2D1_RECT_F box{l.compress.left + d(4), middle(l.compress) - d(8), l.compress.left + d(20), middle(l.compress) + d(8)};
            if (on) rounded(box, d(4), colour(lit ? accentHover : theme::accent));
            else outline(box, d(4), view.compressBusy ? colour(theme::muted, 0.45f) : lit ? ink : pillStroke, d(1.5f));
            if (on) {
                const float bx = (box.left + box.right) / 2, by = middle(box);
                line(bx - d(4), by, bx - d(1), by + d(3), dark, d(1.8f));
                line(bx - d(1), by + d(3), bx + d(4.5f), by - d(3.5f), dark, d(1.8f));
            }
            text(l.compressShort ? L"copy 20mb" : L"compress & copy", body_.get(), {box.right + d(8), l.compress.top, l.compress.right, l.compress.bottom},
                 view.compressBusy ? colour(theme::muted, 0.55f) : lit || on ? ink : muted);
        }
        pill(l.cancel, Part::Cancel, false);
        text(L"cancel", body_.get(), l.cancel, ink, DWRITE_TEXT_ALIGNMENT_CENTER);
        pill(l.replace, Part::Replace, false);
        text(L"replace", body_.get(), l.replace, view.saving ? muted : ink, DWRITE_TEXT_ALIGNMENT_CENTER);
        pill(l.save, Part::Save, true);
        text(view.saving ? L"saving…" : L"save", strong_.get(), l.save,
             view.saving ? muted : colour(theme::background), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

} // namespace refarter::player
