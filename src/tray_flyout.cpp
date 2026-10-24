#include "tray_flyout.h"
#include "theme.h"

#include <algorithm>

namespace refarter {
namespace {
using namespace theme;
constexpr wchar_t className[] = L"RefarterTrayFlyout";
constexpr int width = 300, padding = 16;

void ellipse(HDC dc, float x, float y, float size, COLORREF colour) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(color(colour));
    graphics.FillEllipse(&brush, x, y, size, size);
}
} // namespace

void TrayFlyout::show(HINSTANCE instance, POINT anchor, TrayFlyoutModel model, Chosen chosen) {
    hide();
    if (!registerWindowClass(className, proc, CS_DROPSHADOW)) return;
    model_ = std::move(model);
    chosen_ = std::move(chosen);
    hovered_ = focused_ = -1;
    keyboard_ = false;
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, className, L"refarter", WS_POPUP,
                              anchor.x, anchor.y, 1, 1, nullptr, nullptr, instance, this);
    if (!window_) return;
    dpi_ = GetDpiForWindow(window_);
    title_ = font(dpi_, 15, FW_SEMIBOLD);
    body_ = font(dpi_, 14, FW_NORMAL);
    small_ = font(dpi_, 12, FW_NORMAL);
    const int height = layout();
    // Above the click on the taskbar, inside the work area of that display.
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT work = monitor.rcWork;
    const int x = std::clamp(static_cast<int>(anchor.x) - px(width) / 2, static_cast<int>(work.left) + px(8),
                             static_cast<int>(work.right) - px(width) - px(8));
    int y = anchor.y - height - px(12);
    if (y < work.top + px(8)) y = anchor.y + px(12);
    y = std::clamp(y, static_cast<int>(work.top) + px(8), static_cast<int>(work.bottom) - height - px(8));
    roundWindowCorners(window_);
    setWindowBorderColour(window_, border);
    SetWindowPos(window_, HWND_TOPMOST, x, y, px(width), height, SWP_SHOWWINDOW);
    SetForegroundWindow(window_); // The tray click grants it; keys and click-away now reach the flyout.
}

void TrayFlyout::hide() {
    if (!window_) return;
    const HWND window = window_;
    window_ = nullptr;
    DestroyWindow(window);
    buffer_.release();
    for (auto* font : {&title_, &body_, &small_}) {
        if (*font) DeleteObject(*font);
        *font = nullptr;
    }
}

int TrayFlyout::layout() {
    items_.clear();
    int y = 68; // Below the header.
    items_.push_back({TrayCommand::Action, {px(padding), px(y), px(width - padding), px(y + 38)}, model_.actionEnabled});
    y += 38 + 12;
    items_.push_back({TrayCommand::UseAutoCapture, {px(padding), px(y), px(width - padding), px(y + 34)}, model_.useAutoCaptureEnabled});
    y += 34 + 12;
    if (!model_.detected.empty()) {
        items_.push_back({TrayCommand::AutoCapture, {px(padding), px(y), px(width - padding), px(y + 56)}, model_.autoCaptureEnabled});
        y += 56 + 12;
    }
    y += 13; // Separator.
    const int half = (width - padding * 2 - 8) / 2;
    items_.push_back({TrayCommand::Open, {px(padding), px(y), px(padding + half), px(y + 34)}, true});
    items_.push_back({TrayCommand::Quit, {px(width - padding - half), px(y), px(width - padding), px(y + 34)}, true});
    return px(y + 34 + padding);
}

void TrayFlyout::paint(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    fill(dc, client, surface);

    // Header: the app mark, name and what recording is doing.
    roundRect(dc, {px(padding), px(18), px(padding + 30), px(48)}, px(7), accent, accent);
    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const float unit = static_cast<float>(px(30)) / 24, left = static_cast<float>(px(padding)), top = static_cast<float>(px(18));
        const Gdiplus::PointF play[] = {{left + 9.5f * unit, top + 7 * unit}, {left + 18 * unit, top + 12 * unit}, {left + 9.5f * unit, top + 17 * unit}};
        Gdiplus::SolidBrush brush(color(background));
        graphics.FillPolygon(&brush, play, 3);
    }
    drawText(dc, title_, L"refarter", {px(58), px(15), client.right - px(padding), px(34)}, ink);
    int statusLeft = 58;
    if (model_.live) {
        ellipse(dc, static_cast<float>(px(58)), static_cast<float>(px(39)), static_cast<float>(px(7)), accent);
        statusLeft += 13;
    }
    drawText(dc, small_, model_.status, {px(statusLeft), px(34), client.right - px(padding), px(51)}, model_.live ? accent : muted);

    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const auto& item = items_[i];
        const bool hot = item.enabled && (hovered_ == i || (keyboard_ && focused_ == i));
        switch (item.command) {
        case TrayCommand::Action: {
            const bool primary = model_.actionPrimary && item.enabled;
            const COLORREF fillColour = primary ? (hot ? accentHover : accent) : hot ? border : raised;
            roundRect(dc, item.rect, px(8), fillColour, fillColour);
            drawText(dc, title_, model_.action, {item.rect.left + px(12), item.rect.top, item.rect.right - px(12), item.rect.bottom},
                 !item.enabled ? muted : primary ? background : ink, DT_CENTER);
            break;
        }
        case TrayCommand::AutoCapture: {
            roundRect(dc, item.rect, px(8), hot ? border : raised, hot ? border : raised);
            const int switchWidth = px(40), switchHeight = px(22);
            const RECT track{item.rect.right - px(12) - switchWidth, (item.rect.top + item.rect.bottom - switchHeight) / 2,
                             item.rect.right - px(12), (item.rect.top + item.rect.bottom + switchHeight) / 2};
            drawText(dc, small_, model_.autoCapture ? L"auto-capture · on" : L"auto-capture · off",
                 {item.rect.left + px(12), item.rect.top + px(8), track.left - px(10), item.rect.top + px(26)}, muted);
            drawText(dc, body_, model_.detected, {item.rect.left + px(12), item.rect.top + px(26), track.left - px(10), item.rect.bottom - px(8)},
                 item.enabled ? ink : muted);
            const COLORREF trackColour = !item.enabled ? border : model_.autoCapture ? accent : muted;
            roundRect(dc, track, switchHeight / 2, trackColour, trackColour);
            const float knob = static_cast<float>(switchHeight - px(6));
            const float knobX = static_cast<float>(model_.autoCapture ? track.right - px(3) - static_cast<int>(knob) : track.left + px(3));
            ellipse(dc, knobX, static_cast<float>(track.top + px(3)), knob, model_.autoCapture ? background : surface);
            break;
        }
        case TrayCommand::UseAutoCapture:
            roundRect(dc, item.rect, px(8), hot ? border : raised, hot ? border : raised);
            drawText(dc, body_, L"use auto-capture", item.rect, item.enabled ? ink : muted, DT_CENTER);
            break;
        case TrayCommand::Open:
        case TrayCommand::Quit:
            roundRect(dc, item.rect, px(8), hot ? raised : surface, hot ? raised : border);
            drawText(dc, body_, item.command == TrayCommand::Open ? L"open refarter" : L"quit", item.rect,
                 item.command == TrayCommand::Open ? ink : muted, DT_CENTER);
            break;
        }
        if (keyboard_ && focused_ == i) focusGlow(dc, item.rect, static_cast<float>(dpi_) / 96.f, 8.f);
    }
    // Separator between the recording controls and open/quit.
    const int line = items_.back().rect.top - px(13);
    fill(dc, {px(padding), line, client.right - px(padding), line + 1}, border);
}

void TrayFlyout::choose(TrayCommand command) {
    auto chosen = std::move(chosen_);
    hide(); // First: the command may open a dialog or the main window.
    if (chosen) chosen(command);
}

void TrayFlyout::move(int step) {
    const int count = static_cast<int>(items_.size());
    int index = focused_;
    for (int tried = 0; tried < count; ++tried) {
        index = index < 0 ? (step > 0 ? 0 : count - 1) : (index + step + count) % count;
        if (!items_[index].enabled) continue;
        focused_ = index;
        break;
    }
    keyboard_ = true;
    InvalidateRect(window_, nullptr, FALSE);
}

LRESULT CALLBACK TrayFlyout::proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* flyout = reinterpret_cast<TrayFlyout*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        flyout = static_cast<TrayFlyout*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(flyout));
    }
    if (!flyout || flyout->window_ != window) return DefWindowProcW(window, message, wParam, lParam);
    return flyout->handle(message, wParam, lParam);
}

LRESULT TrayFlyout::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ACTIVATE:
        // Clicking anywhere else closes it, like a menu. Posted: never destroy during activation.
        if (LOWORD(wParam) == WA_INACTIVE) PostMessageW(window_, WM_CLOSE, 0, 0);
        return 0;
    case WM_CLOSE:
        hide();
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) hide();
        else if (wParam == VK_DOWN || wParam == VK_RIGHT || (wParam == VK_TAB && GetKeyState(VK_SHIFT) >= 0)) move(1);
        else if (wParam == VK_UP || wParam == VK_LEFT || wParam == VK_TAB) move(-1);
        else if ((wParam == VK_RETURN || wParam == VK_SPACE) && focused_ >= 0 && items_[focused_].enabled) choose(items_[focused_].command);
        return 0;
    case WM_MOUSEMOVE: {
        const POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
        int hovered = -1;
        for (int i = 0; i < static_cast<int>(items_.size()); ++i) if (items_[i].enabled && PtInRect(&items_[i].rect, point)) hovered = i;
        if (hovered != hovered_) { hovered_ = hovered; InvalidateRect(window_, nullptr, FALSE); }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hovered_ >= 0) { hovered_ = -1; InvalidateRect(window_, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        const POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
        for (const auto& item : items_) {
            if (!item.enabled || !PtInRect(&item.rect, point)) continue;
            choose(item.command);
            break;
        }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        const HDC dc = BeginPaint(window_, &ps);
        RECT client{};
        GetClientRect(window_, &client);
        buffer_.paint(dc, client.right, client.bottom, [&](HDC target) { paint(target); });
        EndPaint(window_, &ps);
        return 0;
    }
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

} // namespace refarter
