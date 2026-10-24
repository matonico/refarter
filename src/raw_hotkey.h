#pragma once

#include "hotkey.h"
#include <array>
#include <iterator>
#include <optional>

namespace refarter {

// Whether a key is down right now: GetAsyncKeyState in the app, a fake keyboard in tests.
using KeyDown = bool (*)(UINT key);
inline bool asyncKeyDown(UINT key) { return (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0; }

// Tracks only the shortcut and modifier keys. A held key produces one save,
// even if modifiers change or keyboard repeat follows the initial press.
class HotkeyPress {
public:
    explicit HotkeyPress(KeyDown live = asyncKeyDown) : live_(live) {}
    // Starts from the keys held right now, so a press already under way does not save.
    void reset(Hotkey shortcut) {
        shortcut_ = shortcut;
        modifiers_ = liveModifiers();
        held_ = live_(shortcut.key);
    }
    bool key(UINT key, bool down) {
        if (const UINT bit = modifierBit(key)) { if (down) modifiers_ |= bit; else modifiers_ &= ~bit; return false; }
        if (!down) { if (key == shortcut_.key) held_ = false; return false; }
        if (key != shortcut_.key) {
            if (held_ && !live_(shortcut_.key)) held_ = false; // Its release was missed.
            return false;
        }
        const bool first = !held_;
        held_ = true;
        // Raw Input misses keys while the secure desktop, the lock screen or an elevated window is
        // in front (Ctrl+Alt+Del, Win+L, Task Manager via Ctrl+Shift+Esc, UAC). A modifier released
        // there stays down in the tracked state, so the keyboard's current state may match instead.
        // It only adds matches: tracked state alone still decides wherever it did before.
        return first && (modifiers(modifiers_) == shortcut_.modifiers || modifiers(liveModifiers()) == shortcut_.modifiers);
    }
private:
    static constexpr UINT modifierKeys[] = {VK_LCONTROL, VK_RCONTROL, VK_LSHIFT, VK_RSHIFT, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN};
    static constexpr UINT modifierBit(UINT key) {
        for (UINT i = 0; i < std::size(modifierKeys); ++i) if (modifierKeys[i] == key) return 1u << i;
        return 0;
    }
    static constexpr UINT modifiers(UINT bits) {
        UINT modifiers = 0;
        if (bits & 3) modifiers |= MOD_CONTROL;
        if (bits & 12) modifiers |= MOD_SHIFT;
        if (bits & 48) modifiers |= MOD_ALT;
        if (bits & 192) modifiers |= MOD_WIN;
        return modifiers;
    }
    UINT liveModifiers() const {
        UINT bits = 0;
        for (const UINT key : modifierKeys) if (live_(key)) bits |= modifierBit(key);
        return bits;
    }
    KeyDown live_;
    Hotkey shortcut_{};
    UINT modifiers_ = 0;
    bool held_ = false;
};

// Raw Input and RegisterHotKey each report a press once, so most presses arrive twice. The first
// report saves and its partner is dropped; either path alone still saves every press.
class PressReports {
public:
    bool accept(bool raw, ULONGLONG now) {
        if (pending_ && raw != raw_ && now - at_ < 500) { pending_ = false; return false; }
        pending_ = true;
        raw_ = raw;
        at_ = now;
        return true;
    }
private:
    ULONGLONG at_ = 0;
    bool raw_ = false, pending_ = false;
};

// Background keyboard notifications only while a replay can be saved, for the save and screenshot shortcuts.
// No hook, injection, input suppression, timer, thread or keyboard log.
class RawHotkey {
public:
    static constexpr int count = 2;
    // By index; an empty one (not registered) never matches.
    using Shortcuts = std::array<std::optional<Hotkey>, count>;
    ~RawHotkey() { configure(nullptr, shortcuts_, false); }
    bool active() const { return active_; }
    void configure(HWND window, const Shortcuts& shortcuts, bool enabled) {
        if (enabled == enabled_ && (!enabled || (window == window_ && shortcuts == shortcuts_))) return;
        enabled_ = enabled;
        if (active_) {
            RAWINPUTDEVICE remove{1, 6, RIDEV_REMOVE, nullptr};
            RegisterRawInputDevices(&remove, 1, sizeof(remove));
            active_ = false;
        }
        window_ = window;
        shortcuts_ = shortcuts;
        if (!enabled) { prime(); return; }
        RAWINPUTDEVICE keyboard{1, 6, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window};
        active_ = RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard)) != FALSE;
        prime();
    }
    void prime() {
        for (int i = 0; i < count; ++i) if (shortcuts_[i]) presses_[i].reset(*shortcuts_[i]);
    }
    // The shortcut this key event presses: its index, or -1.
    int input(HRAWINPUT handle) {
        if (!active_) return -1;
        RAWINPUT input{};
        UINT size = sizeof(input);
        const UINT copied = GetRawInputData(handle, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER));
        if (copied == UINT(-1) || copied < sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD) ||
            input.header.dwType != RIM_TYPEKEYBOARD) return -1;
        const auto& keyboard = input.data.keyboard;
        UINT key = keyboard.VKey;
        if (key == 255) return -1; // Keyboard overrun, not a press.
        if (key == VK_SHIFT) key = MapVirtualKeyW(keyboard.MakeCode, MAPVK_VSC_TO_VK_EX);
        else if (key == VK_CONTROL) key = keyboard.Flags & RI_KEY_E0 ? VK_RCONTROL : VK_LCONTROL;
        else if (key == VK_MENU) key = keyboard.Flags & RI_KEY_E0 ? VK_RMENU : VK_LMENU;
        // Every tracker sees every key, so each keeps its own modifier and held state.
        int pressed = -1;
        for (int i = 0; i < count; ++i)
            if (shortcuts_[i] && presses_[i].key(key, !(keyboard.Flags & RI_KEY_BREAK)) && pressed < 0) pressed = i;
        return pressed;
    }
private:
    Shortcuts shortcuts_{};
    HotkeyPress presses_[count];
    HWND window_ = nullptr;
    bool active_ = false;
    bool enabled_ = false;
};

} // namespace refarter
