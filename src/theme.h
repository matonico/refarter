#pragma once

#include <algorithm>
#include <string>
#include <windows.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>

// Shared palette and drawing helpers for the window and its custom controls.
namespace refarter::theme {

constexpr COLORREF background = RGB(17, 19, 23);
constexpr COLORREF sidebar = RGB(22, 24, 29);
constexpr COLORREF surface = RGB(25, 28, 33);
constexpr COLORREF raised = RGB(33, 36, 43);
constexpr COLORREF border = RGB(43, 46, 54);
constexpr COLORREF ink = RGB(239, 239, 242);
constexpr COLORREF muted = RGB(153, 159, 172);
constexpr COLORREF accent = RGB(255, 158, 100);
constexpr COLORREF peak = RGB(255, 209, 179); // The loudest meter segments.
constexpr COLORREF danger = RGB(240, 123, 114); // The error toast's icon.
constexpr COLORREF accentHover = RGB(255, 182, 138); // A primary button under the pointer (the dialog's value; the tray flyout had its own).
constexpr COLORREF accentSurface = RGB(55, 39, 32);
constexpr COLORREF library = RGB(21, 24, 29);

inline Gdiplus::Color color(COLORREF value) {
    return {255, GetRValue(value), GetGValue(value), GetBValue(value)};
}

inline void fill(HDC dc, RECT rect, COLORREF value) {
    const COLORREF previous = SetDCBrushColor(dc, value);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetDCBrushColor(dc, previous);
}

inline void roundPath(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float d) {
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

inline void roundRect(Gdiplus::Graphics& graphics, RECT rect, int radius, COLORREF value, COLORREF outline) {
    Gdiplus::GraphicsPath path;
    roundPath(path, static_cast<float>(rect.left) + 0.5f, static_cast<float>(rect.top) + 0.5f,
              static_cast<float>(rect.right - rect.left) - 1, static_cast<float>(rect.bottom - rect.top) - 1,
              static_cast<float>(radius * 2));
    Gdiplus::SolidBrush brush(color(value));
    Gdiplus::Pen pen(color(outline));
    graphics.FillPath(&brush, &path);
    graphics.DrawPath(&pen, &path);
}

inline void roundRect(HDC dc, RECT rect, int radius, COLORREF value, COLORREF outline) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    roundRect(graphics, rect, radius, value, outline);
}

// A focus ring that hugs the control's own outline. `rect` is the control's shape and `radius` its corner
// radius in 96-dpi px; `inset` moves the ring inside (positive) or outside (negative) of the shape, keeping
// the corners concentric. `soft` adds the faint outer layers (needs room around the ring to spread).
// Static strokes only: focus never starts an animation or an idle timer.
inline void focusGlow(HDC dc, RECT rect, float scale, float radius = 7.f, float inset = 3.f, bool soft = true) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float gap = inset * scale;
    const float corner = (std::max)(radius * scale - gap, 1.f);
    Gdiplus::GraphicsPath path;
    roundPath(path, rect.left + gap, rect.top + gap, rect.right - rect.left - gap * 2 - 1,
              rect.bottom - rect.top - gap * 2 - 1, corner * 2.f);
    for (int layer = soft ? 3 : 1; layer >= 1; --layer) {
        Gdiplus::Pen halo(Gdiplus::Color(static_cast<BYTE>(layer == 1 ? 190 : 28),
            255, 158, 100), (layer == 1 ? 1.5f : layer * 2.f - 1.f) * scale);
        graphics.DrawPath(&halo, &path);
    }
}

// A 1 px accent outline on the control's own shape, for keyboard focus. No halo layers.
inline void focusOutline(Gdiplus::Graphics& graphics, RECT rect, float scale, float radius = 7.f) {
    Gdiplus::GraphicsPath path;
    roundPath(path, rect.left + 0.5f, rect.top + 0.5f, static_cast<float>(rect.right - rect.left) - 1,
              static_cast<float>(rect.bottom - rect.top) - 1, radius * scale * 2.f);
    Gdiplus::Pen pen(Gdiplus::Color(255, 255, 158, 100), 1.f);
    graphics.DrawPath(&pen, &path);
}

inline void focusOutline(HDC dc, RECT rect, float scale, float radius = 7.f) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    focusOutline(graphics, rect, scale, radius);
}


// Blend of two colours; t is 0 for a and 1 for b.
inline COLORREF mix(COLORREF a, COLORREF b, float t) {
    const auto channel = [t](int x, int y) { return static_cast<BYTE>(x + (y - x) * t + 0.5f); };
    return RGB(channel(a & 0xFF, b & 0xFF), channel((a >> 8) & 0xFF, (b >> 8) & 0xFF), channel((a >> 16) & 0xFF, (b >> 16) & 0xFF));
}

// Sets a control's text only when it differs: SetWindowText repaints an owner-drawn button even when unchanged.
inline void setTextIfChanged(HWND window, const std::wstring& text) {
    wchar_t current[160]{};
    GetWindowTextW(window, current, static_cast<int>(std::size(current)));
    if (text != current) SetWindowTextW(window, text.c_str());
}

// Segoe UI at `size` px for a 96-dpi layout, scaled to `dpi`. The caller deletes it.
inline HFONT font(UINT dpi, int size, int weight) {
    return CreateFontW(-MulDiv(size, static_cast<int>(dpi), 96), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

// One line of text, vertically centred in `rect`, with an ellipsis and no mnemonic prefixes.
inline void drawText(HDC dc, HFONT font, const std::wstring& value, RECT rect, COLORREF colour, UINT alignment = DT_LEFT) {
    const auto previous = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, colour);
    DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &rect,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | alignment);
    SelectObject(dc, previous);
}

// Windows 11 rounded corners for a popup window (DWMWCP_ROUND = 2, DWMWCP_ROUNDSMALL = 3).
// Returns whether the system accepted it; older Windows keeps square corners.
inline bool roundWindowCorners(HWND window, DWORD preference = 2) {
    constexpr DWORD windowCornerPreference = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
    return SUCCEEDED(DwmSetWindowAttribute(window, windowCornerPreference, &preference, sizeof(preference)));
}

// The system-drawn 1 px outline of a popup window (Windows 11).
inline void setWindowBorderColour(HWND window, COLORREF value) {
    constexpr DWORD borderColour = 34; // DWMWA_BORDER_COLOR
    DwmSetWindowAttribute(window, borderColour, &value, sizeof(value));
}

// Registers a window class with the arrow cursor once; later calls with the same name succeed without re-registering.
inline bool registerWindowClass(const wchar_t* name, WNDPROC procedure, UINT style = 0) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW existing{sizeof(existing)};
    if (GetClassInfoExW(instance, name, &existing)) return true;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = style;
    wc.lpfnWndProc = procedure;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = name;
    return RegisterClassExW(&wc) != 0;
}

// An off-screen bitmap for flicker-free painting, kept between paints and re-made when the size changes.
// Allocation happens on invalidation only, never in a render loop.
class BackBuffer {
public:
    BackBuffer() = default;
    BackBuffer(const BackBuffer&) = delete;
    BackBuffer& operator=(const BackBuffer&) = delete;
    ~BackBuffer() { release(); }

    // A memory DC of width x height compatible with `target`, cleared to black; nullptr when it cannot be made.
    HDC acquire(HDC target, int width, int height) {
        if (width <= 0 || height <= 0) return nullptr;
        if (dc_ && width == width_ && height == height_) {
            SelectClipRgn(dc_, nullptr); // A kept DC would otherwise carry the last caller's clip.
            PatBlt(dc_, 0, 0, width, height, BLACKNESS);
            return dc_;
        }
        release();
        dc_ = CreateCompatibleDC(target);
        bitmap_ = dc_ ? CreateCompatibleBitmap(target, width, height) : nullptr;
        if (!bitmap_) {
            release();
            return nullptr;
        }
        original_ = SelectObject(dc_, bitmap_);
        width_ = width;
        height_ = height;
        return dc_;
    }

    // Copies the acquired buffer to `target`.
    void present(HDC target) const {
        if (dc_) BitBlt(target, 0, 0, width_, height_, dc_, 0, 0, SRCCOPY);
    }

    // Draws with `draw(HDC)` into the buffer and copies it to `target` (straight to `target` if no buffer is available).
    template <class Draw>
    void paint(HDC target, int width, int height, Draw&& draw) {
        if (width <= 0 || height <= 0) return;
        if (const HDC dc = acquire(target, width, height)) {
            draw(dc);
            present(target);
        } else {
            draw(target);
        }
    }

    void release() {
        if (dc_ && original_) SelectObject(dc_, original_);
        if (bitmap_) DeleteObject(bitmap_);
        if (dc_) DeleteDC(dc_);
        dc_ = nullptr;
        bitmap_ = nullptr;
        original_ = nullptr;
        width_ = height_ = 0;
    }

private:
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ original_ = nullptr;
    int width_ = 0, height_ = 0;
};
} // namespace refarter::theme
