#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace refarter {
// A modal message box in the app's own style, with MessageBoxW's signature and IDOK/IDYES/IDNO/IDCANCEL
// results. Supports MB_OK, MB_OKCANCEL, MB_YESNO and MB_YESNOCANCEL, MB_DEFBUTTONn and the icon beeps.
// It centres over the owner's top-level window; with no visible, non-minimised owner (e.g. in the tray)
// it centres on the primary monitor as a taskbar window. Esc and Alt+F4 answer cancel, or no.
int messageBox(HWND owner, const wchar_t* text, const wchar_t* caption, UINT type);

// The same, with a checkbox labelled `checkLabel` in the button row, left of the buttons. `*checked` is the starting
// state and receives the final one when the dialog closes, whatever button closed it (the caller decides whether a
// ticked box counts for that answer). Tab reaches the box first; Space toggles it. While dialogs are suppressed
// `*checked` is left as it was.
int messageBox(HWND owner, const wchar_t* text, const wchar_t* caption, UINT type, const wchar_t* checkLabel, bool* checked);

// While dialogs are suppressed (the bounded waits of a shutdown or a sleep, which pump messages) messageBox opens
// nothing and answers at once with the safe choice: ok, no, or cancel. Calls nest; every `true` needs its `false`.
void suppressDialogs(bool on);

// A question with custom buttons. `choices` lists the answers in order, the last being the dismiss button (Esc and
// the close key answer it too). Returns the index of the chosen answer, or -1 for the dismiss button or while dialogs
// are suppressed. The first answer is the default.
int choiceBox(HWND owner, const wchar_t* text, const wchar_t* caption, const std::vector<std::wstring>& choices);
} // namespace refarter
