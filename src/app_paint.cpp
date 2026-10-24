// App: everything drawn by hand. Fonts and icons, the page and footer, owner-drawn buttons and sliders, the switch animation.
#include "app_internal.h"
#include "resources.h"
#include "text.h"
#include "version.h"

#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <algorithm>

namespace refarter {
namespace {
// Every page starts with its title; the library's filter row and the other pages' cards follow it.
constexpr int pageTitleTop = 20, pageTitleHeight = 40;
// Row buttons drawn as dropdowns (text after " · " is the shown value, a chevron marks the menu);
// the on/off switches are ui::toggleIds.
constexpr int dropdownIds[] = {DesktopAudio, Microphone, Resolution, FrameRate, QualityPreset, VideoCodec, AutoCaptureSource, StorageBehaviour};
constexpr ULONGLONG switchMillis = 100;
} // namespace

namespace internal {
void icon(HDC dc, int kind, int x, int y, int size, COLORREF value) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float unit = static_cast<float>(size) / 24;
    g.TranslateTransform(static_cast<float>(x), static_cast<float>(y));
    g.ScaleTransform(unit, unit);
    Gdiplus::Pen pen(color(value), kind < 0 ? 33.6f / static_cast<float>(size) : 1.6f); // The meter icons: a 1.4 px stroke at any size.
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    if (kind == meterSpeakerIcon) {
        const Gdiplus::PointF body[] = {{3.f, 9.f}, {8.f, 9.f}, {13.f, 4.f}, {13.f, 20.f}, {8.f, 15.f}, {3.f, 15.f}};
        g.DrawPolygon(&pen, body, 6);
        g.DrawArc(&pen, 10.f, 7.f, 8.f, 10.f, -60.f, 120.f);
        g.DrawArc(&pen, 8.f, 3.f, 14.f, 18.f, -60.f, 120.f);
    } else if (kind == meterMicIcon) {
        Gdiplus::GraphicsPath capsule;
        roundPath(capsule, 9.f, 2.f, 6.f, 12.f, 6.f);
        g.DrawPath(&pen, &capsule);
        g.DrawArc(&pen, 5.f, 6.f, 14.f, 12.f, 0.f, 180.f);
        g.DrawLine(&pen, 12.f, 18.f, 12.f, 20.f);
    } else if (kind == Library) {
        g.DrawRectangle(&pen, 3.f, 4.f, 18.f, 16.f);
        g.DrawLine(&pen, 3.f, 9.f, 21.f, 9.f);
        g.DrawLine(&pen, 8.f, 4.f, 8.f, 9.f);
        g.DrawLine(&pen, 16.f, 4.f, 16.f, 9.f);
    } else if (kind == ClipSearch) {
        g.DrawEllipse(&pen, 4.f, 4.f, 12.f, 12.f);
        g.DrawLine(&pen, 14.5f, 14.5f, 20.f, 20.f);
    } else if (kind == Recording) {
        g.DrawEllipse(&pen, 3.f, 3.f, 18.f, 18.f);
        Gdiplus::SolidBrush brush(color(value));
        g.FillEllipse(&brush, 8.f, 8.f, 8.f, 8.f);
    } else if (kind == Settings) {
        for (int row = 0; row < 3; ++row) {
            const float at = 6.f + row * 6.f;
            g.DrawLine(&pen, 3.f, at, 21.f, at);
            const float knob = row == 1 ? 14.f : 7.f;
            Gdiplus::SolidBrush brush(color(sidebar));
            g.FillEllipse(&brush, knob, at - 2, 4.f, 4.f);
            g.DrawEllipse(&pen, knob, at - 2, 4.f, 4.f);
        }
    } else {
        const Gdiplus::PointF points[] = {{9.f, 6.f}, {18.f, 12.f}, {9.f, 18.f}};
        Gdiplus::SolidBrush brush(color(value));
        g.FillPolygon(&brush, points, 3);
    }
}
} // namespace internal

void App::label(HDC dc, const std::wstring& value, int x, int y, int w, int h,
                Font font, COLORREF textColor, UINT alignment) const {
    drawText(dc, fontOf(font), value, RECT{px(x), px(y), px(x + w), px(y + h)}, textColor, alignment);
}

std::wstring App::footerStatus() const {
    const auto session = sessionLine();
    return session.status + L"  ·  " + (session.source.empty() ? L"no source selected" : session.source);
}

// The icon and the measured status text, in device pixels: the version on the right is outside it.
RECT App::footerStatusBounds() const {
    const int extent = smallTextExtent(footerStatus());
    RECT client{};
    GetClientRect(window_, &client);
    const int top = client.bottom - px(footerHeight);
    return {px(14), top, px(38) + std::min<int>(extent, px(MulDiv(client.right, 96, static_cast<int>(dpi_)) - 38 - footerReserve())), client.bottom};
}

void App::createFonts() {
    for (auto font : fonts_) if (font) DeleteObject(font);
    textExtents_.clear(); // Measured with the old small font.
    constexpr int sizes[] = {12, 14, 17, 28}; // By Font.
    for (size_t i = 0; i < fonts_.size(); ++i) {
        fonts_[i] = theme::font(dpi_, sizes[i], i >= static_cast<size_t>(Font::Heading) ? FW_SEMIBOLD : FW_NORMAL);
    }
    for (const auto& c : controls_) if (c.window) SendMessageW(c.window, WM_SETFONT,
        reinterpret_cast<WPARAM>(fontOf(Font::Body)), TRUE);
    clipGrid_.setStyle(dpi_, fontOf(Font::Body), fontOf(Font::Small), state_.smallTiles);
    // Tab widths depend only on the font: border, padding on each side, a 16 icon and its 8 gap.
    const HDC dc = GetDC(window_);
    const auto previous = SelectObject(dc, fontOf(Font::Body));
    for (size_t i = 0; i < std::size(viewTabs); ++i) {
        const wchar_t* name = viewTabs[i] == AllClips ? L"all clips" : viewTabs[i] == Favourites ? L"favourites" : L"compressed";
        SIZE extent{};
        GetTextExtentPoint32W(dc, name, static_cast<int>(wcslen(name)), &extent);
        tabWidths_[i] = MulDiv(extent.cx, 96, static_cast<int>(dpi_)) + 2 * 17 + 16 + 8;
    }
    SelectObject(dc, previous);
    ReleaseDC(window_, dc);
}

void App::loadIcons() {
    const auto load = [&](int metric) {
        const int size = GetSystemMetricsForDpi(metric, dpi_);
        return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_REFARTER),
            IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    };
    if (auto replacement = load(SM_CXICON)) {
        SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(replacement));
        if (largeIcon_) DestroyIcon(largeIcon_);
        largeIcon_ = replacement;
    }
    if (auto replacement = load(SM_CXSMICON)) {
        SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(replacement));
        if (smallIcon_) DestroyIcon(smallIcon_);
        smallIcon_ = replacement;
        tray_.setIcon(smallIcon_);
    }
}

float App::switchPosition(int id) const {
    const auto index = std::find(std::begin(toggleIds), std::end(toggleIds), id) - std::begin(toggleIds);
    const auto& motion = switches_[static_cast<size_t>(index)];
    if (!motion.running) return toggled(id) ? 1.f : 0.f;
    const float t = std::min(1.f, static_cast<float>(GetTickCount64() - motion.start) / static_cast<float>(switchMillis));
    const float eased = 1.f - (1.f - t) * (1.f - t) * (1.f - t); // ease-out cubic
    return motion.from + (motion.to - motion.from) * eased;
}

void App::animateSwitch(int id, float from) {
    const auto index = std::find(std::begin(toggleIds), std::end(toggleIds), id) - std::begin(toggleIds);
    auto& motion = switches_[static_cast<size_t>(index)];
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    const float to = toggled(id) ? 1.f : 0.f;
    if (!animations || from == to) {
        motion.running = false;
    } else {
        motion = {from, to, GetTickCount64(), true};
        SetTimer(window_, switchTimer, 15, nullptr); // Stopped by stepSwitches when none is left.
    }
    InvalidateRect(control(id), nullptr, FALSE);
    UpdateWindow(control(id)); // Paint the first frame now, before any other work.
}

void App::stepSwitches() {
    bool any = false;
    const ULONGLONG now = GetTickCount64();
    for (size_t i = 0; i < switches_.size(); ++i) {
        auto& motion = switches_[i];
        if (!motion.running) continue;
        const bool done = now - motion.start >= switchMillis;
        InvalidateRect(control(toggleIds[i]), nullptr, FALSE); // Draws the position at this tick (the last frame too).
        if (done) motion.running = false; else any = true;
    }
    if (!any) KillTimer(window_, switchTimer);
}

void App::drawButton(const DRAWITEMSTRUCT& item) {
    const int id = static_cast<int>(item.CtlID);
    const bool nav = id == Library || id == Recording || id == Settings;
    const bool active = (id == Library && state_.page == Page::Library) ||
        (id == Recording && state_.page == Page::Recording) ||
        (id == Settings && state_.page == Page::Settings) ||
        (id == AllClips && state_.clipView == ClipView::All) || (id == Favourites && state_.clipView == ClipView::Favourites) ||
        (id == Compressed && state_.clipView == ClipView::Compressed) ||
        (id == ThirtySeconds && state_.replayInputValid && state_.replaySeconds == 30) ||
        (id == OneMinute && state_.replayInputValid && state_.replaySeconds == 60) ||
        (id == ThreeMinutes && state_.replayInputValid && state_.replaySeconds == 180) ||
        (id == FiveMinutes && state_.replayInputValid && state_.replaySeconds == 300) ||
        (id == LargeTiles && !state_.smallTiles) || (id == SmallTiles && state_.smallTiles) || (id == GroupFolders && state_.groupFolders) ||
        (id == AssignHotkey && assigning_ && assigningShortcut_ == saveShortcut) ||
        (id == AssignScreenshot && assigning_ && assigningShortcut_ == screenshotShortcut);
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    // One orange button: enable replay while it is off, then save replay (stop turns plain).
    const bool primary = ((id == StartReplay && recording_ == capture::RecorderState::Off) || id == SaveReplay) && !disabled;
    COLORREF bg = nav ? sidebar : surface;
    COLORREF fg = disabled ? muted : ink;
    COLORREF stroke = nav ? sidebar : border;
    if (active) { bg = accentSurface; fg = accent; stroke = accentSurface; }
    if (primary) { bg = accent; fg = background; stroke = accent; }
    if (hovered_ == id && !disabled && !primary && !active) { bg = raised; stroke = raised; }
    if ((item.itemState & ODS_SELECTED) && !disabled) { bg = raised; fg = ink; }
    // Rounded corners must blend into the actual parent surface, independent of IDs.
    const auto* registered = entry(id);
    const auto backdrop = registered && registered->window == item.hwndItem ? registered->backdrop : ButtonBackdrop::Canvas;
    const COLORREF behind = backdrop == ButtonBackdrop::Sidebar ? sidebar : backdrop == ButtonBackdrop::Card ? surface : background;
    fill(item.hDC, item.rcItem, behind);
    const bool focusShown = keyboardFocus_ && (item.itemState & ODS_FOCUS) != 0;
    const float scale = static_cast<float>(dpi_) / 96.f;
    Gdiplus::Graphics graphics(item.hDC);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    if (contains(toggleIds, id)) {
        // A switch: accent track with the knob at the right when on. Its row names it.
        const float t = switchPosition(id); // 0 off .. 1 on, mid-way while animating
        const int trackHeight = px(22);
        const int top = (item.rcItem.top + item.rcItem.bottom - trackHeight) / 2;
        const RECT track{item.rcItem.right - px(40), top, item.rcItem.right, top + trackHeight};
        const COLORREF onColour = disabled ? accentSurface : accent;
        const COLORREF offColour = hovered_ == id && !disabled ? border : raised;
        const COLORREF fillColour = mix(offColour, onColour, t);
        roundRect(graphics, track, trackHeight / 2, fillColour, mix(border, onColour, t));
        const float knob = 16.f * scale, inset = 3.f * scale;
        const float knobX = track.left + inset + t * (track.right - track.left - 2 * inset - knob);
        Gdiplus::SolidBrush brush(color(mix(disabled ? border : muted, background, t)));
        graphics.FillEllipse(&brush, knobX, top + inset, knob, knob);
        if (focusShown) focusOutline(graphics, track, scale, 11.f);
        return;
    }
    if (id == SidebarToggle) {
        // A handle on the sidebar's edge: its left half is over the sidebar, its right half over the page.
        const RECT& r = item.rcItem;
        const int mid = (r.left + r.right) / 2;
        fill(item.hDC, {r.left, r.top, mid, r.bottom}, sidebar);
        fill(item.hDC, {mid, r.top, r.right, r.bottom}, background);
        const bool hot = hovered_ == id;
        roundRect(graphics, r, px(12), hot ? raised : sidebar, border);
        // A 14 px chevron (on a 16 grid) pointing the way the sidebar will move.
        Gdiplus::Pen pen(color(hot ? ink : muted), 1.4f * scale);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        const float u = 14.f * scale / 16.f;
        const float x0 = (r.left + r.right) / 2.f - 7.f * scale, y0 = (r.top + r.bottom) / 2.f - 7.f * scale;
        const float a = state_.sidebarCollapsed ? 6.f : 10.f, b = state_.sidebarCollapsed ? 10.f : 6.f;
        const Gdiplus::PointF chevron[] = {{x0 + a * u, y0 + 3.f * u}, {x0 + b * u, y0 + 8.f * u}, {x0 + a * u, y0 + 13.f * u}};
        graphics.DrawLines(&pen, chevron, 3);
        if (focusShown) focusOutline(graphics, item.rcItem, scale, 12.f);
        return;
    }
    if (contains(viewTabs, id)) {
        // A folder tab on the clip card. The chosen one is a pixel taller and open at the bottom,
        // so it merges into the card's top border.
        const bool chosen = id == tabFor(state_.clipView);
        const bool hot = !chosen && (hovered_ == id || (item.itemState & ODS_SELECTED));
        const RECT& r = item.rcItem;
        const float left = r.left + 0.5f, right = r.right - 0.5f, top = r.top + 0.5f, bottom = static_cast<float>(r.bottom);
        const float d = 20.f * scale;
        if (chosen || hot) {
            Gdiplus::GraphicsPath body;
            body.AddArc(left, top, d, d, 180.f, 90.f);
            body.AddArc(right - d, top, d, d, 270.f, 90.f);
            body.AddLine(right, bottom, left, bottom);
            body.CloseFigure();
            Gdiplus::SolidBrush brush(color(chosen ? library : surface));
            graphics.FillPath(&brush, &body);
        }
        if (chosen) {
            Gdiplus::GraphicsPath edge;
            edge.AddLine(left, bottom, left, top + d / 2);
            edge.AddArc(left, top, d, d, 180.f, 90.f);
            edge.AddArc(right - d, top, d, d, 270.f, 90.f);
            edge.AddLine(right, top + d / 2, right, bottom);
            Gdiplus::Pen outline(color(border));
            graphics.DrawPath(&outline, &edge);
        }
        const COLORREF tint = chosen ? accent : hot ? ink : muted;
        const float cy = r.top + (chosen ? 20.f : 19.5f) * scale;
        graphics.TranslateTransform(r.left + 17.f * scale, cy - 8.f * scale);
        Gdiplus::Pen pen(color(tint), 1.6f);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        Gdiplus::SolidBrush ink16(color(tint));
        if (id == AllClips) {
            graphics.ScaleTransform(0.8f * scale, 0.8f * scale);
            for (const float y : {2.5f, 11.5f})
                for (const float x : {2.5f, 11.5f}) graphics.FillRectangle(&ink16, x, y, 6.f, 6.f);
        } else if (id == Favourites) {
            graphics.ScaleTransform(scale * 16.f / 24.f, scale * 16.f / 24.f);
            const Gdiplus::PointF star[] = {{12.f, 2.5f}, {14.59f, 8.44f}, {21.04f, 9.06f}, {16.18f, 13.36f}, {17.58f, 19.69f},
                {12.f, 16.4f}, {6.42f, 19.69f}, {7.82f, 13.36f}, {2.96f, 9.06f}, {9.41f, 8.44f}};
            graphics.DrawPolygon(&pen, star, 10);
        } else {
            graphics.ScaleTransform(scale, scale);
            graphics.DrawLine(&pen, 2.f, 8.f, 14.f, 8.f);
            const Gdiplus::PointF top2[] = {{5.f, 2.5f}, {8.f, 5.5f}, {11.f, 2.5f}}, bottom2[] = {{5.f, 13.5f}, {8.f, 10.5f}, {11.f, 13.5f}};
            graphics.DrawLines(&pen, top2, 3);
            graphics.DrawLines(&pen, bottom2, 3);
        }
        graphics.ResetTransform();
        wchar_t title[32];
        GetWindowTextW(item.hwndItem, title, 32);
        RECT text{r.left + px(41), r.top + px(1), r.right, r.top + px(38)};
        const auto previous = SelectObject(item.hDC, fontOf(Font::Body));
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, chosen || hot ? ink : muted);
        DrawTextW(item.hDC, title, -1, &text, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_LEFT);
        SelectObject(item.hDC, previous);
        if (focusShown) focusOutline(graphics, item.rcItem, scale, 10.f);
        return;
    }
    if (id == DesktopReset || id == MicrophoneReset) {
        // Half faded while replay is on, like the device menu beside it.
        const auto fade = [](COLORREF a) {
            const auto half = [](int x, int y) { return static_cast<BYTE>((x + y) / 2); };
            return RGB(half(a & 0xFF, surface & 0xFF), half((a >> 8) & 0xFF, (surface >> 8) & 0xFF), half((a >> 16) & 0xFF, (surface >> 16) & 0xFF));
        };
        const bool hot = hovered_ == id && !disabled;
        roundRect(graphics, item.rcItem, px(7), hot ? raised : surface, disabled ? fade(border) : hot ? raised : border);
        // A counter-clockwise arrow on a 24 grid, drawn 16 px.
        graphics.TranslateTransform((item.rcItem.left + item.rcItem.right) / 2.f - 8.f * scale, (item.rcItem.top + item.rcItem.bottom) / 2.f - 8.f * scale);
        graphics.ScaleTransform(scale * 16.f / 24.f, scale * 16.f / 24.f);
        Gdiplus::Pen pen(color(disabled ? fade(muted) : hot ? accent : muted), 1.6f);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        Gdiplus::GraphicsPath loop;
        loop.AddArc(3.f, 3.f, 18.f, 18.f, 180.f, -270.f);
        loop.AddArc(12.03f - 9.75f, 12.75f - 9.75f, 19.5f, 19.5f, -90.f, -44.f);
        loop.AddLine(5.26f, 5.74f, 3.f, 8.f);
        graphics.DrawPath(&pen, &loop);
        const Gdiplus::PointF head[] = {{3.f, 3.f}, {3.f, 8.f}, {8.f, 8.f}};
        graphics.DrawLines(&pen, head, 3);
        graphics.ResetTransform();
        if (focusShown) focusOutline(graphics, item.rcItem, scale);
        return;
    }
    roundRect(graphics, item.rcItem, px(7), bg, stroke);
    if (contains(libraryIconIds, id)) {
        // Drawn on a 20-unit square: 2×2 large tiles, 3×3 small tiles, a folder (folders by game), or a folder
        // with an arrow out of it (open the clip folder in Explorer).
        const float unit = scale, size = 20.f * unit;
        graphics.TranslateTransform((item.rcItem.left + item.rcItem.right - size) / 2.f, (item.rcItem.top + item.rcItem.bottom - size) / 2.f);
        graphics.ScaleTransform(unit, unit);
        Gdiplus::SolidBrush brush(color(fg));
        Gdiplus::Pen pen(color(fg), 1.6f);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        if (id == GroupFolders) {
            const Gdiplus::PointF folder[] = {{2.5f, 5.f}, {8.f, 5.f}, {10.f, 7.f}, {17.5f, 7.f}, {17.5f, 16.f}, {2.5f, 16.f}};
            graphics.DrawPolygon(&pen, folder, 6);
        } else if (id == OpenFolder) {
            // The folder is open at its top right, where the arrow leaves it.
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            const Gdiplus::PointF folder[] = {{11.f, 7.f}, {9.5f, 7.f}, {7.5f, 5.f}, {2.5f, 5.f}, {2.5f, 16.f}, {16.5f, 16.f}, {16.5f, 11.5f}};
            graphics.DrawLines(&pen, folder, 7);
            graphics.DrawLine(&pen, 9.5f, 12.5f, 17.5f, 3.5f);
            const Gdiplus::PointF head[] = {{12.5f, 3.5f}, {17.5f, 3.5f}, {17.5f, 8.5f}};
            graphics.DrawLines(&pen, head, 3);
        } else {
            const int tiles = id == LargeTiles ? 2 : 3;
            const float gap = id == LargeTiles ? 3.f : 2.f, tile = (15.f - gap * (tiles - 1)) / tiles;
            for (int row = 0; row < tiles; ++row)
                for (int column = 0; column < tiles; ++column)
                    graphics.FillRectangle(&brush, 2.5f + column * (tile + gap), 2.5f + row * (tile + gap), tile, tile);
        }
        graphics.ResetTransform();
        if (focusShown) focusOutline(graphics, item.rcItem, scale);
        return;
    }
    // The replay buttons' icons, the same collapsed and expanded: an arrow down onto a line to save, a record
    // dot to start, a square to stop or clear.
    const auto replayIcon = [&](int left, int top, int size) {
        if (id == SaveReplay) {
            Gdiplus::Pen pen(color(fg), 1.8f * scale);
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            const float c = left + size / 2.f, u = scale;
            graphics.DrawLine(&pen, c, top + 3 * u, c, top + 13 * u);
            const Gdiplus::PointF head[] = {{c - 4.5f * u, top + 8.5f * u}, {c, top + 13 * u}, {c + 4.5f * u, top + 8.5f * u}};
            graphics.DrawLines(&pen, head, 3);
            graphics.DrawLine(&pen, c - 6 * u, top + 17 * u, c + 6 * u, top + 17 * u);
        } else {
            Gdiplus::SolidBrush brush(color(fg));
            const float c = left + size / 2.f, m = top + size / 2.f, r = 6.f * scale;
            if (recording_ == capture::RecorderState::Off) graphics.FillEllipse(&brush, c - r, m - r, 2 * r, 2 * r);
            else graphics.FillRectangle(&brush, c - r + scale, m - r + scale, 2 * r - 2 * scale, 2 * r - 2 * scale);
        }
    };
    const bool replayButton = id == StartReplay || id == SaveReplay;
    if (state_.sidebarCollapsed && (nav || replayButton)) {
        // Icon only; the tooltip and the button text (for screen readers) name it.
        const int size = px(20);
        const int left = (item.rcItem.left + item.rcItem.right - size) / 2, top = (item.rcItem.top + item.rcItem.bottom - size) / 2;
        if (nav) icon(item.hDC, id, left, top, size, active ? accent : muted);
        else replayIcon(left, top, size);
        if (focusShown) focusOutline(graphics, item.rcItem, scale);
        return;
    }
    if (nav) icon(item.hDC, id, px(15), px(13), px(20), active ? accent : muted);
    else if (replayButton) replayIcon(item.rcItem.left + px(15), (item.rcItem.top + item.rcItem.bottom - px(20)) / 2, px(20));
    wchar_t title[128];
    GetWindowTextW(item.hwndItem, title, 128);
    const bool dropdown = contains(dropdownIds, id);
    // Dropdown text is "<setting> · <value>" for screen readers; the row already names the setting.
    const wchar_t* shown = title;
    if (const auto split = dropdown ? wcsstr(title, L" · ") : nullptr) shown = split + 3;
    // The two shortcut buttons are named for screen readers ("assign save shortcut"); the row already says which.
    else if ((id == AssignHotkey || id == AssignScreenshot) && !wcsncmp(title, L"assign", 6)) shown = L"assign hotkey";
    RECT text = item.rcItem;
    text.left += px(nav ? 49 : replayButton ? 44 : dropdown ? 12 : 9);
    text.right -= px(dropdown ? 30 : 9);
    auto previous = SelectObject(item.hDC, fontOf(Font::Body));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, fg);
    DrawTextW(item.hDC, shown, -1, &text, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | (nav || replayButton || dropdown ? DT_LEFT : DT_CENTER));
    SelectObject(item.hDC, previous);
    if (dropdown) {
        // A small chevron at the right marks a menu.
        Gdiplus::Pen pen(color(disabled ? muted : fg), 1.6f * scale);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        const float cx = item.rcItem.right - 17.f * scale;
        const float cy = (item.rcItem.top + item.rcItem.bottom) / 2.f;
        const Gdiplus::PointF chevron[] = {{cx - 4.f * scale, cy - 2.f * scale}, {cx, cy + 2.f * scale}, {cx + 4.f * scale, cy - 2.f * scale}};
        graphics.DrawLines(&pen, chevron, 3);
    }
    if (focusShown) focusOutline(graphics, item.rcItem, scale);
}

void App::drawVolumeSlider(HWND slider, HDC dc) {
    RECT rect{}, channel{}, thumb{};
    GetClientRect(slider, &rect);
    SendMessageW(slider, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
    SendMessageW(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    fill(dc, rect, surface);
    const bool enabled = IsWindowEnabled(slider) != FALSE;
    const int center = (thumb.left + thumb.right) / 2;
    const int y = (thumb.top + thumb.bottom) / 2;
    const int radius = px(2);
    roundRect(dc, {channel.left, y - radius, channel.right, y + radius}, radius, border, border);
    if (center > channel.left + px(4))
        roundRect(dc, {channel.left, y - radius, center, y + radius}, radius,
                  enabled ? accent : muted, enabled ? accent : muted);
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float knobRadius = static_cast<float>(px(6));
    Gdiplus::SolidBrush brush(color(enabled ? accent : muted));
    graphics.FillEllipse(&brush, center - knobRadius, y - knobRadius, knobRadius * 2, knobRadius * 2);
    if (keyboardFocus_ && GetFocus() == slider) focusOutline(dc, rect, static_cast<float>(dpi_) / 96.f, 4.f);
}

void App::paintVolumeSlider(HWND slider, HDC dc) {
    // Keep drawing in the native paint buffer, but remove its per-item clip.
    // This prevents the old thumb/channel clip from cutting holes in our fill.
    const int saved = SaveDC(dc);
    if (saved) SelectClipRgn(dc, nullptr);
    RECT rect{};
    GetClientRect(slider, &rect);
    if (rect.right > 0 && rect.bottom > 0) {
        BackBuffer buffer;
        buffer.paint(dc, rect.right, rect.bottom, [&](HDC target) { drawVolumeSlider(slider, target); });
    }
    if (saved) RestoreDC(dc, saved);
}

void App::paint(HDC dc, const RECT& dirty) {
    RECT client;
    GetClientRect(window_, &client);
    const int w = MulDiv(client.right, 96, static_cast<int>(dpi_));
    const int h = MulDiv(client.bottom, 96, static_cast<int>(dpi_));
    const int bottom = h - footerHeight; // Sidebar and pages end above the footer.
    // Whether anything of `area` (pixels, or logical pixels for the second form) needs drawing.
    const auto touches = [&](RECT area) { RECT hit; return IntersectRect(&hit, &dirty, &area) != FALSE; };
    const auto touchesLogical = [&](int x, int y, int width, int height) {
        return touches({px(x), px(y), px(x + width), px(y + height)});
    };
    fill(dc, dirty, background);
    const int side = sidebarWidth();
    fill(dc, {0, 0, px(side), px(bottom)}, sidebar);
    fill(dc, {px(side - 1), 0, px(side), px(bottom)}, border);
    const auto box = [&](int x, int y, int width, int height, COLORREF bg = surface) {
        if (touchesLogical(x, y, width, height)) roundRect(dc, {px(x), px(y), px(x + width), px(y + height)}, px(10), bg, border);
    };
    const auto line = [&](int x, int y, int width) { fill(dc, {px(x), px(y), px(x + width), px(y + 1)}, border); };
    const bool buffering = recording_ == capture::RecorderState::Buffering;
    if (touchesLogical(0, 0, side, bottom)) {
        const bool collapsed = state_.sidebarCollapsed;
        // The mark; expanded, the name and tagline follow. Collapsed, the toggle and buttons carry tooltips.
        const int markLeft = logoLeft(collapsed);
        roundRect(dc, {px(markLeft), px(logoTop), px(markLeft + logoSize), px(logoTop + logoSize)}, px(9), accent, accent);
        icon(dc, 0, px(markLeft + logoMarkInset), px(logoTop + logoMarkInset), px(logoIconSize), background);
        if (!collapsed) {
            label(dc, L"refarter", logoNameLeft, logoTop - 1, logoNameWidth, 28, Font::Heading, ink);
            label(dc, L"keep the good part.", 25, 78, 173, 20, Font::Small, muted);
        }
    }
    paintSidebarMeters(dc, dirty);
    // The footer, always visible: recording state and source on the left, version on the right.
    if (touchesLogical(0, bottom, w, footerHeight)) {
        fill(dc, {0, px(bottom), client.right, client.bottom}, sidebar);
        line(0, bottom, w);
        const COLORREF statusColour = footerHot_ ? ink : buffering ? accent : muted;
        icon(dc, Recording, px(14), px(bottom + 5), px(16), statusColour);
        label(dc, footerStatus(), 38, bottom, w - 38 - footerReserve(), footerHeight, Font::Small, statusColour);
        if (const RECT update = footerUpdateBounds(); !IsRectEmpty(&update))
            drawText(dc, fontOf(Font::Small), footerUpdateText(), {update.left, px(bottom), update.right + px(2), client.bottom}, updateHot_ ? ink : accent);
        label(dc, footerVersion(), w - 216, bottom, 200, footerHeight, Font::Small, muted, DT_RIGHT);
    }
    // Scrolled settings never paint over the footer.
    IntersectClipRect(dc, 0, 0, client.right, px(bottom));

    const int x = contentLeft();
    const int width = w - x - contentGap;
    // Pages have no header: each starts with its content.
    if (playerOpen_) return; // The player fills the content area.
    if (state_.page == Page::Library) {
        label(dc, L"your clips.", x, pageTitleTop, width, pageTitleHeight, Font::Title, ink, DT_LEFT);
        // Under the card, before the folder button at the right edge: folder problems, a short note, or the folder size.
        const int textWidth = width - footerButton - 10;
        if (!clipGrid_.error().empty()) label(dc, clipGrid_.error(), x, bottom - 38, textWidth, footerButton, Font::Small, accent, DT_RIGHT);
        else if (!clipGrid_.note().empty()) label(dc, clipGrid_.note(), x, bottom - 38, textWidth, footerButton, Font::Small, ink, DT_RIGHT);
        else if (clipGrid_.scanned()) {
            const auto strip = storageStrip(); // Folder size, or use of the storage limit; the alert colour from 90%.
            label(dc, strip.text, x, bottom - 38, textWidth, footerButton, Font::Small, strip.alert ? accent : muted, DT_RIGHT);
        }
        if (clipGrid_.scanned()) {
            const auto count = clipGrid_.count();
            // Ends just before the folders toggle (see placeControls()).
            label(dc, count == 1 ? L"1 clip" : std::to_wstring(count) + L" clips",
                w - contentGap - libraryButtonsWidth - countGap - countWidth, tileButtonTop, countWidth, tileButton, Font::Small, muted, DT_RIGHT);
        }
        // The search field's frame (the accent while typing in it) with the magnifier inside, before the text.
        if (const HWND search = control(ClipSearch); IsWindowVisible(search)) {
            const RECT frame = fieldFrame(ClipSearch);
            if (touches(frame)) {
                roundRect(dc, frame, px(7), background, GetFocus() == search ? accent : border);
                const int size = px(16);
                icon(dc, ClipSearch, frame.left + px(9), (frame.top + frame.bottom - size) / 2, size, clipGrid_.searching() ? ink : muted);
            }
        }
        // A 1 × 18 divider between two neighbouring tabs that are both unchosen.
        for (int i = 0, tabX = x + 8; i < 2; ++i) {
            tabX += tabWidth(viewTabs[i]);
            if (!tabDivider(state_.clipView, i)) continue;
            fill(dc, {px(tabX), px(libraryRowTop + 10), px(tabX + 1), px(libraryRowTop + 28)}, border);
            ++tabX;
        }
        const int top = libraryCardTop;
        // The card runs down to the folder strip above the footer.
        box(x, top, width, bottom - libraryCardBottom - top, library);
        // Empty states only; clips are drawn by the grid child window.
        if (clipGrid_.scanned() && clipGrid_.count() == 0) {
            const int center = x + width / 2;
            const int middle = top + 52;
            const bool favourites = state_.clipView == ClipView::Favourites, compressed = state_.clipView == ClipView::Compressed;
            if (clipGrid_.searching()) {
                // Nothing matches the search; the tab itself may have clips.
                if (touchesLogical(x, top, width, bottom - libraryCardBottom - top)) {
                    label(dc, L"no clips match.", x + 24, middle + 4, width - 48, 40, Font::Title, ink, DT_CENTER);
                    label(dc, L"every word must appear in a clip's name or game.", x + 24, middle + 50, width - 48, 28, Font::Body, muted, DT_CENTER);
                    label(dc, L"esc in the search field clears it.", x + 24, middle + 79, width - 48, 26, Font::Small, muted, DT_CENTER);
                }
            } else if (touchesLogical(x, top, width, bottom - libraryCardBottom - top)) {
                roundRect(dc, {px(center - 38), px(middle - 34), px(center + 28), px(middle + 15)}, px(8), background, border);
                roundRect(dc, {px(center - 27), px(middle - 23), px(center + 39), px(middle + 26)}, px(8), raised, RGB(67, 61, 56));
                icon(dc, 0, px(center - 9), px(middle - 15), px(33), accent);
                label(dc, favourites ? L"no favourites. yet." : compressed ? L"nothing compressed. yet." : L"nothing clipped. yet.", x + 24, middle + 44, width - 48, 40, Font::Title, ink, DT_CENTER);
                label(dc, favourites ? L"star a clip under all clips to keep it here." : compressed ? L"compress a clip from its ⋯ menu to keep a copy under 20 mb here." : L"choose a source and enable replay to save your first clip.",
                    x + 24, middle + 90, width - 48, 28, Font::Body, muted, DT_CENTER);
                if (!favourites && !compressed) label(dc, L"clips in " + lower(clipFolder_.wstring()) + L" show up here.", x + 24, middle + 119, width - 48, 26, Font::Small, muted, DT_CENTER);
            }
        }
    } else {
        // Recording and Settings: titled cards of rows, scrolled as one page (from the cache layout() made).
        const int dpi = static_cast<int>(dpi_);
        const int scroll = scroll_[static_cast<int>(state_.page)];
        label(dc, state_.page == Page::Recording ? L"ready for the next play." : L"make yourself at home.",
            x, pageTitleTop - scroll, width, pageTitleHeight, Font::Title, ink, DT_LEFT);
        for (const auto& section : pages_[static_cast<size_t>(state_.page)]) {
            const int left = x + section.x, top = section.top - scroll;
            if (!touchesLogical(left, top, section.width, section.bottom - section.top)) continue;
            label(dc, section.title, left + 4, top, section.width - 8, 26, Font::Heading, ink);
            box(left, top + headingHeight, section.width, section.bottom - section.top - headingHeight);
            for (size_t i = 0; i < section.rows.size(); ++i) {
                const auto& row = section.rows[i];
                const int y = row.y - scroll;
                if (!touchesLogical(left, y, section.width, row.height)) continue;
                if (i > 0) line(left + 16, y, section.width - 32);
                const COLORREF name = IsWindowEnabled(control(row.ids.front())) ? ink : muted;
                if (row.note.empty()) {
                    label(dc, row.name, left + 20, y, row.textWidth, row.height, Font::Body, name);
                } else {
                    label(dc, row.name, left + 20, y + 9, row.textWidth, 22, Font::Body, name);
                    label(dc, row.note, left + 20, y + 31, row.textWidth, 20, Font::Small, row.alert ? accent : muted);
                }
            }
        }
        // Number fields: an input frame (accent while invalid or focused) and, for the gate, a caption before it.
        for (const int id : numberIds) {
            const auto field = control(id);
            if (!IsWindowVisible(field)) continue;
            // The unit sits inside the frame, after the value.
            const auto unit = fieldUnit(id);
            const RECT frame = fieldFrame(id);
            const int valueRight = MulDiv(frame.right - px(unit.width + 4), 96, dpi) - 8;
            const auto* gate = gateFieldOf(id);
            // The gate's caption sits to the left of the frame.
            RECT painted = frame;
            if (gate) painted.left -= px(50);
            if (!touches(painted)) continue;
            const bool valid = gate ? gateFieldValid_[gate - std::begin(gateFields)] :
                id == Bitrate ? bitrateValid_ : id == StorageLimitField ? true : state_.replayInputValid;
            roundRect(dc, frame, px(7), background, !valid || GetFocus() == field ? accent : border);
            const int top = MulDiv(frame.top, 96, dpi), height = MulDiv(frame.bottom - frame.top, 96, dpi);
            label(dc, unit.text, valueRight + 4, top, unit.width + 2, height, Font::Small, muted);
            if (gate)
                label(dc, gate->name, MulDiv(frame.left, 96, dpi) - 50, top, 46, height, Font::Small, muted, DT_RIGHT);
        }

        // A source's typed percentage shows a frame only under the pointer or with focus (never the accent).
        for (const int id : typedVolumeIds) {
            const auto field = control(id);
            if (!IsWindowVisible(field) || (hoverVolume_ != id && GetFocus() != field)) continue;
            const RECT frame = fieldFrame(id);
            if (!touches(frame)) continue;
            roundRect(dc, frame, px(6), surface, border);
        }

        if (state_.page == Page::Recording) paintSliderMeters(dc, dirty);
        // A thin position indicator in the right margin while the page is taller than the window.
        if (const int range = scrollMax(state_.page); range > 0) {
            const int track = bottom - 2 * pageTop;
            const int thumb = std::max(32, track * bottom / pageHeight(state_.page));
            const int top = pageTop + (track - thumb) * scroll / range;
            if (touchesLogical(w - 22, top, 4, thumb)) roundRect(dc, {px(w - 22), px(top), px(w - 18), px(top + thumb)}, px(2), border, border);
        }
    }
}

} // namespace refarter
