#include "value_bubble.h"
#include "theme.h"

namespace refarter {

using namespace theme;

namespace {
constexpr wchar_t className[] = L"RefarterValueBubble";
}

void ValueBubble::show(HWND owner, RECT anchor, const std::wstring& text, HFONT font, UINT dpi) {
    present(owner, anchor, text, font, dpi, false);
}

void ValueBubble::showNote(HWND owner, RECT anchor, const std::wstring& text, HFONT font, UINT dpi) {
    present(owner, anchor, text, font, dpi, true);
}

void ValueBubble::present(HWND owner, RECT anchor, const std::wstring& text, HFONT font, UINT dpi, bool note) {
    if (!registerWindowClass(className, proc)) return;
    if (!window_) {
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, className, L"", WS_POPUP,
                                  0, 0, 1, 1, owner, nullptr, GetModuleHandleW(nullptr), this);
        if (!window_) return;
        roundWindowCorners(window_, 3); // DWMWCP_ROUNDSMALL
        setWindowBorderColour(window_, border);
    }
    const auto px = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    const bool changed = text != text_ || font != font_ || note != note_;
    // A drag reports every mouse move: nothing to measure or move while the text and the anchor are as they were.
    if (!changed && dpi == dpi_ && EqualRect(&anchor, &anchor_) && IsWindowVisible(window_)) return;
    dpi_ = dpi;
    anchor_ = anchor;
    text_ = text;
    font_ = font;
    note_ = note;
    padding_ = {px(9), px(4)};
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &monitor);
    SIZE extent{};
    const HDC dc = GetDC(window_);
    const auto previous = SelectObject(dc, font);
    if (note) {
        // As wide as the text needs, up to a limit (and the work area); beyond that it wraps, and a third line is cut.
        GetTextExtentPoint32W(dc, L"0", 1, &extent);
        const int widest = (std::min)(px(260), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left)) - 2 * padding_.cx;
        RECT measured{0, 0, (std::max)(widest, 1), 0};
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        extent.cx = measured.right;
        extent.cy = (std::min)(measured.bottom, 2 * extent.cy);
    } else {
        // Wide enough for the largest value, so the box does not resize while a value changes.
        GetTextExtentPoint32W(dc, L"200%", 4, &extent);
    }
    SelectObject(dc, previous);
    ReleaseDC(window_, dc);
    const SIZE size{extent.cx + 2 * padding_.cx, extent.cy + 2 * padding_.cy};
    const POINT origin = placeValueBubble(anchor, size, px(4), monitor.rcWork);
    SetWindowPos(window_, HWND_TOPMOST, origin.x, origin.y, size.cx, size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (changed) InvalidateRect(window_, nullptr, FALSE);
}

void ValueBubble::hide() {
    if (window_) ShowWindow(window_, SW_HIDE);
}

void ValueBubble::destroy() {
    if (!window_) return;
    const HWND window = window_;
    window_ = nullptr;
    DestroyWindow(window);
}

LRESULT CALLBACK ValueBubble::proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
    }
    auto* self = reinterpret_cast<ValueBubble*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCHITTEST: return HTTRANSPARENT; // Never takes the mouse from the slider.
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        const HDC dc = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);
        fill(dc, client, raised);
        if (self && self->note_) {
            // The box was sized to the text, so it sits inside the padding; a third line is cut with an ellipsis.
            const auto previous = SelectObject(dc, self->font_);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, ink);
            RECT text = client;
            InflateRect(&text, -self->padding_.cx, -self->padding_.cy);
            DrawTextW(dc, self->text_.c_str(), static_cast<int>(self->text_.size()), &text,
                      DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, previous);
        } else if (self) {
            drawText(dc, self->font_, self->text_, client, ink, DT_CENTER);
        }
        EndPaint(window, &ps);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace refarter
