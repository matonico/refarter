#pragma once

#include "theme.h"

#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace refarter {

// What the tray flyout shows; built by the app when the icon is right-clicked.
struct TrayFlyoutModel {
    std::wstring status;        // e.g. "replay on · apex legends"
    bool live = false;          // Replay is buffering: the status gets an orange dot.
    std::wstring action;        // "start replay · <source>", "stop replay" or "clear retained replay".
    bool actionEnabled = true;
    bool actionPrimary = true;  // Starting is the accent button; stopping and clearing are neutral.
    bool useAutoCaptureEnabled = false; // Returning to listed games never discards an active or retained replay.
    std::wstring detected;      // The game in front; empty hides the auto-capture row.
    bool autoCapture = false;   // That game is on the auto-capture list.
    bool autoCaptureEnabled = true;
};

enum class TrayCommand { Action, AutoCapture, UseAutoCapture, Open, Quit };

// A small flyout above the tray in the app's colours, replacing the classic
// context menu. Mouse and keyboard (arrows/Tab, Enter/Space, Esc); it closes
// when it loses activation. Nothing runs while it is closed.
class TrayFlyout {
public:
    using Chosen = std::function<void(TrayCommand)>;

    TrayFlyout() = default;
    TrayFlyout(const TrayFlyout&) = delete;
    TrayFlyout& operator=(const TrayFlyout&) = delete;
    ~TrayFlyout() { hide(); }

    void show(HINSTANCE instance, POINT anchor, TrayFlyoutModel model, Chosen chosen);
    void hide();
    bool visible() const { return window_ != nullptr; }

private:
    struct Item { TrayCommand command; RECT rect; bool enabled; };
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    int layout(); // Returns the height in pixels.
    void paint(HDC dc);
    void choose(TrayCommand command);
    void move(int step);
    int px(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }

    HWND window_ = nullptr;
    theme::BackBuffer buffer_;
    TrayFlyoutModel model_;
    Chosen chosen_;
    UINT dpi_ = 96;
    HFONT title_ = nullptr, body_ = nullptr, small_ = nullptr;
    std::vector<Item> items_;
    int hovered_ = -1, focused_ = -1;
    bool keyboard_ = false; // Focus outlines appear only after keyboard use.
};

} // namespace refarter
