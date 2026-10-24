#include "auto_capture_card.h"
#include "theme.h"


namespace refarter {
namespace {
using namespace theme;
constexpr wchar_t className[] = L"RefarterAutoCaptureCard";
// Timers run only while a card exists: a short delay, re-raising while it is
// up (games that make themselves topmost when entering fullscreen would cover
// it), and the close.
constexpr UINT_PTR appearTimer = 1, raiseTimer = 2, closeTimer = 3;
constexpr UINT appearMilliseconds = 1000, raiseMilliseconds = 500, showMilliseconds = 8000;
constexpr int cardWidth = 360, cardHeight = 116, margin = 16;

} // namespace

void AutoCaptureCard::show(HINSTANCE instance, HMONITOR display, std::wstring label, Answered answered) {
    hide();
    if (!registerWindowClass(className, proc, 0)) return;
    label_ = std::move(label);
    answered_ = std::move(answered);
    hovered_ = 0;
    // Bottom-right of the game's display, over borderless games; it is never
    // activated, so the game keeps keyboard and mouse focus.
    MONITORINFO monitor{sizeof(monitor)};
    if (!display || !GetMonitorInfoW(display, &monitor))
        GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, className, L"refarter auto-capture",
        WS_POPUP, monitor.rcWork.right - 1, monitor.rcWork.bottom - 1, 1, 1, nullptr, nullptr, instance, this);
    if (!window_) return;
    dpi_ = GetDpiForWindow(window_);
    title_ = font(dpi_, 14, FW_SEMIBOLD);
    body_ = font(dpi_, 12, FW_NORMAL);
    roundWindowCorners(window_);
    layout();
    SetWindowPos(window_, HWND_TOPMOST, monitor.rcWork.right - px(cardWidth + margin), monitor.rcWork.bottom - px(cardHeight + margin),
                 px(cardWidth), px(cardHeight), SWP_NOACTIVATE);
    SetTimer(window_, appearTimer, appearMilliseconds, nullptr);
}

void AutoCaptureCard::raise() {
    // Back above other topmost windows without activation or a move.
    SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void AutoCaptureCard::hide() {
    if (!window_) return;
    const HWND window = window_;
    window_ = nullptr;
    KillTimer(window, appearTimer);
    KillTimer(window, raiseTimer);
    KillTimer(window, closeTimer);
    DestroyWindow(window);
    buffer_.release();
    if (title_) DeleteObject(title_);
    if (body_) DeleteObject(body_);
    title_ = body_ = nullptr;
}

void AutoCaptureCard::layout() {
    add_ = {px(margin), px(70), px(margin + 90), px(100)};
    decline_ = {px(margin + 100), px(70), px(margin + 220), px(100)};
    close_ = {px(cardWidth - 10 - 24), px(10), px(cardWidth - 10), px(34)};
}

void AutoCaptureCard::finish(Answer answer) {
    auto answered = std::move(answered_);
    hide(); // First: the answer may show another card.
    if (answered) answered(answer);
}

void AutoCaptureCard::paint(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    fill(dc, client, background);
    roundRect(dc, client, px(8), surface, border);
    drawText(dc, title_, L"auto-capture " + label_ + L"?", {px(margin), px(12), close_.left - px(6), px(36)}, ink);
    drawText(dc, body_, L"replay starts by itself whenever it is in front", {px(margin), px(36), client.right - px(margin), px(56)}, muted);
    // Close: dismiss for now, without remembering "not this one".
    if (hovered_ == 3) roundRect(dc, close_, px(6), raised, raised);
    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(color(hovered_ == 3 ? ink : muted), static_cast<float>(px(2)) * 0.75f);
        const float inset = static_cast<float>(px(8));
        const float l = close_.left + inset, t = close_.top + inset, r = close_.right - inset, b = close_.bottom - inset;
        graphics.DrawLine(&pen, l, t, r, b);
        graphics.DrawLine(&pen, l, b, r, t);
    }
    roundRect(dc, add_, px(7), accent, accent);
    drawText(dc, title_, L"add", add_, background, DT_CENTER);
    roundRect(dc, decline_, px(7), hovered_ == 2 ? raised : surface, hovered_ == 2 ? raised : border);
    drawText(dc, title_, L"not this one", decline_, ink, DT_CENTER);
}

LRESULT CALLBACK AutoCaptureCard::proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* card = reinterpret_cast<AutoCaptureCard*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        card = static_cast<AutoCaptureCard*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(card));
    }
    if (!card || card->window_ != window) return DefWindowProcW(window, message, wParam, lParam);
    return card->handle(message, wParam, lParam);
}

LRESULT AutoCaptureCard::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        const POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
        const int hovered = PtInRect(&add_, point) ? 1 : PtInRect(&decline_, point) ? 2 : PtInRect(&close_, point) ? 3 : 0;
        if (hovered != hovered_) { hovered_ = hovered; InvalidateRect(window_, nullptr, FALSE); }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hovered_) { hovered_ = 0; InvalidateRect(window_, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        const POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
        if (PtInRect(&add_, point)) finish(Answer::Add);
        else if (PtInRect(&decline_, point)) finish(Answer::Decline);
        else if (PtInRect(&close_, point)) finish(Answer::Closed);
        return 0;
    }
    case WM_TIMER:
        if (wParam == appearTimer) {
            KillTimer(window_, appearTimer);
            ShowWindow(window_, SW_SHOWNOACTIVATE);
            raise();
            SetTimer(window_, raiseTimer, raiseMilliseconds, nullptr);
            SetTimer(window_, closeTimer, showMilliseconds, nullptr);
        } else if (wParam == raiseTimer) raise();
        else if (wParam == closeTimer) finish(Answer::TimedOut);
        return 0;
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
