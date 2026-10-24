#pragma once

#include <windows.h>

#include <string>

namespace refarter {

// Shows a menu built with the Win32 menu API (text, checks, disabled rows,
// separators, submenus) as Refarter's own popup: dark, rounded, with a hover
// highlight and accent check marks. Modal like TrackPopupMenu: returns the
// chosen item's ID, or 0 when dismissed by Esc, a click elsewhere or switching
// apps. Opens below `at` (screen pixels), or above it near the screen bottom.
// Keyboard: arrows, Home/End, Enter/Space, Esc, ←/→ for submenus, and the
// first letter of an item. The owner keeps keyboard focus; nothing activates.
UINT showPopupMenu(HWND owner, HMENU menu, POINT at, UINT dpi);

// A menu described with the Win32 menu API and shown by showPopupMenu. Submenus are destroyed with the root.
// `append` takes literal text: ampersands are escaped so they are not read as mnemonics.
struct PopupMenu {
    HMENU root = CreatePopupMenu();
    PopupMenu() = default;
    PopupMenu(const PopupMenu&) = delete;
    PopupMenu& operator=(const PopupMenu&) = delete;
    ~PopupMenu() { if (root) DestroyMenu(root); }
    HMENU submenu() const { return CreatePopupMenu(); }
    void separator(HMENU menu) { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); }
    void append(HMENU menu, UINT id, std::wstring text, UINT state = MFS_ENABLED, HMENU child = nullptr) {
        for (size_t at = 0; (at = text.find(L'&', at)) != std::wstring::npos; at += 2) text.insert(at, 1, L'&');
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_ID | MIIM_STATE;
        item.fType = MFT_STRING;
        item.fState = state;
        item.wID = id;
        item.dwTypeData = text.data();
        if (child) { item.fMask |= MIIM_SUBMENU; item.hSubMenu = child; }
        InsertMenuItemW(menu, static_cast<UINT>(GetMenuItemCount(menu)), TRUE, &item);
    }
};

} // namespace refarter
