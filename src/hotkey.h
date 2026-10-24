#pragma once

#include "text.h"

#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>

namespace refarter {

// Global save shortcut as RegisterHotKey takes it: MOD_* flags and a virtual key.
struct Hotkey {
    UINT modifiers = MOD_CONTROL | MOD_SHIFT;
    UINT key = VK_F10;
    bool operator==(const Hotkey&) const = default;
};
inline constexpr Hotkey defaultHotkey{};
inline constexpr Hotkey defaultScreenshotHotkey{MOD_CONTROL | MOD_SHIFT, VK_F11};
inline constexpr UINT hotkeyModifiers = MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;

enum class HotkeyProblem { None, Invalid, ModifierOnly, NeedsModifier, Reserved };

inline constexpr bool isModifierKey(UINT key) {
    switch (key) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
        return true;
    default:
        return false;
    }
}

inline constexpr HotkeyProblem checkHotkey(Hotkey hotkey) {
    if ((hotkey.modifiers & ~hotkeyModifiers) || hotkey.key < 0x08 || hotkey.key > 0xFE) return HotkeyProblem::Invalid;
    if (isModifierKey(hotkey.key)) return HotkeyProblem::ModifierOnly;
    // RegisterHotKey reserves F12 for debuggers; Alt+F4 must keep closing windows.
    if (hotkey.key == VK_F12 || (hotkey.key == VK_F4 && (hotkey.modifiers & MOD_ALT))) return HotkeyProblem::Reserved;
    // A global hotkey swallows its keys everywhere, so ordinary keys need ctrl, alt or win.
    const bool standalone = (hotkey.key >= VK_F1 && hotkey.key <= VK_F24) || hotkey.key == VK_PAUSE || hotkey.key == VK_SCROLL;
    if (!standalone && !(hotkey.modifiers & (MOD_CONTROL | MOD_ALT | MOD_WIN))) return HotkeyProblem::NeedsModifier;
    return HotkeyProblem::None;
}

// Layout-independent names; empty for keys the keyboard layout should name.
inline std::wstring keyName(UINT key) {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::wstring(1, static_cast<wchar_t>(key + (key >= 'A' ? 'a' - 'A' : 0)));
    if (key >= VK_F1 && key <= VK_F24) return L"f" + std::to_wstring(key - VK_F1 + 1);
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) return L"num " + std::to_wstring(key - VK_NUMPAD0);
    switch (key) {
    case VK_PAUSE: return L"pause";
    case VK_SCROLL: return L"scroll lock";
    case VK_INSERT: return L"insert";
    case VK_DELETE: return L"delete";
    case VK_HOME: return L"home";
    case VK_END: return L"end";
    case VK_PRIOR: return L"page up";
    case VK_NEXT: return L"page down";
    case VK_LEFT: return L"left";
    case VK_RIGHT: return L"right";
    case VK_UP: return L"up";
    case VK_DOWN: return L"down";
    case VK_SPACE: return L"space";
    case VK_RETURN: return L"enter";
    case VK_TAB: return L"tab";
    case VK_BACK: return L"backspace";
    case VK_ESCAPE: return L"esc";
    case VK_MULTIPLY: return L"num *";
    case VK_ADD: return L"num plus";
    case VK_SUBTRACT: return L"num minus";
    case VK_DECIMAL: return L"num .";
    case VK_DIVIDE: return L"num /";
    default: return {};
    }
}

// "ctrl + shift + f10"; `key` overrides the name of the final key.
inline std::wstring hotkeyText(Hotkey hotkey, std::wstring key = {}) {
    std::wstring text;
    if (hotkey.modifiers & MOD_CONTROL) text += L"ctrl + ";
    if (hotkey.modifiers & MOD_ALT) text += L"alt + ";
    if (hotkey.modifiers & MOD_SHIFT) text += L"shift + ";
    if (hotkey.modifiers & MOD_WIN) text += L"win + ";
    if (key.empty()) key = keyName(hotkey.key);
    if (key.empty()) key = L"key " + std::to_wstring(hotkey.key);
    return text + key;
}

// The modifiers held so far while a shortcut is being assigned: "ctrl + shift + …"; empty when none are held.
inline std::wstring heldModifiersText(UINT modifiers) {
    std::wstring text = hotkeyText({modifiers & hotkeyModifiers, 0}, L"…");
    return (modifiers & hotkeyModifiers) ? text : std::wstring{};
}

// Reads the config form written by hotkeyText (no layout names): "ctrl + shift + f10", "alt + key 192".
inline std::optional<Hotkey> parseHotkey(std::wstring_view text) {
    Hotkey hotkey{0, 0};
    for (size_t start = 0; start <= text.size();) {
        const size_t end = std::min(text.find(L'+', start), text.size());
        std::wstring part;
        for (const auto c : text.substr(start, end - start)) if (c != L' ' && c != L'\t') part += c;
        part = lower(part);
        start = end + 1;
        if (part == L"ctrl") hotkey.modifiers |= MOD_CONTROL;
        else if (part == L"alt") hotkey.modifiers |= MOD_ALT;
        else if (part == L"shift") hotkey.modifiers |= MOD_SHIFT;
        else if (part == L"win") hotkey.modifiers |= MOD_WIN;
        else if (hotkey.key || part.empty()) return std::nullopt; // Exactly one non-modifier key.
        else if (part.starts_with(L"key") && part.size() > 3 && part.size() <= 6 &&
                 part.find_first_not_of(L"0123456789", 3) == std::wstring::npos) hotkey.key = std::stoul(part.substr(3));
        else {
            // Spaces were dropped above, so compare names without them too.
            for (UINT key = 0x08; key <= 0xFE && !hotkey.key; ++key) {
                auto name = keyName(key);
                std::erase(name, L' ');
                if (!name.empty() && name == part) hotkey.key = key;
            }
            if (!hotkey.key) return std::nullopt;
        }
    }
    if (checkHotkey(hotkey) != HotkeyProblem::None) return std::nullopt;
    return hotkey;
}
} // namespace refarter
