#pragma once

#include <windows.h>
#include <string>

namespace refarter {

// The notification-area icon: added only when the app lives in the tray, and re-added
// when Explorer restarts (TaskbarCreated). Nothing polls.
class TrayIcon {
public:
    static constexpr UINT id = 1; // The icon id; callbacks carry it in HIWORD(lParam).
    // Registers the TaskbarCreated message; `callback` is the message the shell sends for icon events.
    void create(HWND window, UINT callback);
    bool present() const { return present_; }
    bool add(HICON icon, const std::wstring& tip); // False: the shell refused the icon.
    void remove();
    void setTip(const std::wstring& tip); // Only when it changed.
    void setIcon(HICON icon);              // A new icon for a present one (DPI change).
    bool taskbarCreated(UINT message) const { return taskbarCreated_ && message == taskbarCreated_; }
    void forget() { present_ = false; }    // Explorer discarded the icon; add() may run again.

private:
    HWND window_ = nullptr;
    UINT callback_ = 0;
    UINT taskbarCreated_ = 0;
    bool present_ = false;
    std::wstring tip_;
};

} // namespace refarter
