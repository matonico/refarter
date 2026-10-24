#pragma once

#include <algorithm>
#include <string>
#include <windows.h>

namespace refarter {

// Where a value bubble goes: centred above `anchor` (the slider's knob column, in screen pixels, spanning the
// slider's full height so the bubble never covers the slider), clamped into `work`. With no room above, it
// goes below instead. `gap` separates it from the anchor.
inline POINT placeValueBubble(RECT anchor, SIZE size, int gap, RECT work) {
    const int centre = (anchor.left + anchor.right) / 2;
    const int minX = work.left, maxX = (std::max)(minX, static_cast<int>(work.right) - static_cast<int>(size.cx));
    const int x = std::clamp(centre - static_cast<int>(size.cx) / 2, minX, maxX);
    int y = anchor.top - gap - static_cast<int>(size.cy);
    if (y < work.top) y = anchor.bottom + gap;
    return {x, y};
}

// What is wrong with one of the noise gate's five number fields (open, close, attack, hold, release, in that
// order): the text for a bubble above it and the fields it spans (`first` to `last`). A close level above the
// open level is the pair's problem, so it spans both; any other problem is the one field's.
struct GateProblem {
    std::wstring text;
    int first = 0, last = 0;
};
inline GateProblem gateProblem(int field, bool closeAboveOpen, int levelMinDb, int levelMaxDb, int timeMaxMs) {
    if (field < 2) {
        if (closeAboveOpen) return {L"close must be at or below open", 0, 1};
        return {L"enter −" + std::to_wstring(-levelMinDb) + L" to " + std::to_wstring(levelMaxDb) + L" db", field, field};
    }
    return {L"enter 0–" + std::to_wstring(timeMaxMs) + L" ms", field, field};
}

// A small dark box with a value, shown above a slider's knob while it moves, or with a short note (up to two
// lines) above the field it is about. An owned popup that never takes focus or mouse input; created on first
// use, then only hidden until the app closes.
class ValueBubble {
public:
    ValueBubble() = default;
    ValueBubble(const ValueBubble&) = delete;
    ValueBubble& operator=(const ValueBubble&) = delete;
    ~ValueBubble() { destroy(); }

    // `anchor` in screen pixels; `font` stays owned by the caller and must outlive the bubble's use.
    void show(HWND owner, RECT anchor, const std::wstring& text, HFONT font, UINT dpi);
    // A note instead of a value: as wide as its text needs (up to a limit), wrapped to at most two lines.
    void showNote(HWND owner, RECT anchor, const std::wstring& text, HFONT font, UINT dpi);
    void hide();
    void destroy();

private:
    void present(HWND owner, RECT anchor, const std::wstring& text, HFONT font, UINT dpi, bool note);
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    HWND window_ = nullptr;
    std::wstring text_;
    HFONT font_ = nullptr;
    bool note_ = false;
    SIZE padding_{};
    RECT anchor_{};  // What the shown bubble was placed for; an unchanged call does nothing.
    UINT dpi_ = 0;
};

} // namespace refarter
