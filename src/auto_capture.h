#pragma once

#include <windows.h>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace refarter {

// One game the user chose to auto-capture, identified by its full executable path.
struct AutoCaptureGame {
    std::wstring path;  // Compared case-insensitively; a moved install needs adding again.
    std::wstring label; // Shown in menus, the card, the tray and the status line.
};

// The explicit auto-capture list plus the apps the user answered "not this one" for.
struct AutoCaptureList {
    std::vector<AutoCaptureGame> games;
    std::vector<std::wstring> declined;

    const AutoCaptureGame* find(const std::wstring& path) const;
    bool isDeclined(const std::wstring& path) const;
    void add(std::wstring path, std::wstring label); // Also forgets an earlier "not this one".
    void remove(const std::wstring& path);
    void decline(std::wstring path);
};

std::filesystem::path autoCapturePath();
std::wstring formatAutoCapture(const AutoCaptureList& list);
// Malformed text is an error rather than a partial list, so it is never overwritten.
std::optional<AutoCaptureList> parseAutoCapture(const std::wstring& text);
// A missing file is an empty list; an unreadable or malformed one is an error.
std::optional<AutoCaptureList> readAutoCapture(const std::filesystem::path& file);
bool writeAutoCapture(const std::filesystem::path& file, const AutoCaptureList& list);

// Whether a fullscreen app may be offered for auto-capture: not Refarter, not
// a browser, video player, screenshot tool or overlay, and not Windows' own
// programs. Users decline the rest.
bool offerable(const std::filesystem::path& executable);
// Overlays and capture tools are tool or non-activating windows; games are ordinary app windows.
bool appWindowStyle(LONG_PTR extendedStyle);
// Only confirmed process exit clears automatic replay; Alt-Tab/window closure and
// manual sessions retain it. A held process handle keeps instance identity stable.
bool exitedAutoCapture(bool automatic, HANDLE process);
// The window covers its whole monitor: borderless or exclusive fullscreen,
// not merely maximised above a visible taskbar.
bool coversMonitor(const RECT& window, const RECT& monitor);

} // namespace refarter
