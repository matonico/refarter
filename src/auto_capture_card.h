#pragma once

#include "auto_session_rules.h"
#include "theme.h"

#include <windows.h>
#include <functional>
#include <string>

namespace refarter {

// "auto-capture <game>?" above the tray. It never takes focus from the game:
// clicks work without activating it, and it closes itself after a few seconds.
// The tray menu offers the same choice for keyboard users.
class AutoCaptureCard {
public:
    using Answer = OfferAnswer; // Closed: the X, "not now".
    using Answered = std::function<void(Answer)>;

    AutoCaptureCard() = default;
    AutoCaptureCard(const AutoCaptureCard&) = delete;
    AutoCaptureCard& operator=(const AutoCaptureCard&) = delete;
    ~AutoCaptureCard() { hide(); }

    // Appears above the tray of `monitor` (the game's display) after a short
    // delay, so a game finishing its switch to fullscreen does not cover it.
    void show(HINSTANCE instance, HMONITOR monitor, std::wstring label, Answered answered);
    void hide();
    bool visible() const { return window_ != nullptr; }

private:
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    void paint(HDC dc);
    void layout();
    void raise();
    void finish(Answer answer);
    int px(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }

    HWND window_ = nullptr;
    theme::BackBuffer buffer_;
    std::wstring label_;
    Answered answered_;
    UINT dpi_ = 96;
    HFONT title_ = nullptr, body_ = nullptr;
    RECT add_{}, decline_{}, close_{};
    int hovered_ = 0; // 1 add, 2 decline, 3 close.
};

} // namespace refarter
