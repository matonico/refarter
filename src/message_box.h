#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace refarter {
// A modal message box in the app's own style, with MessageBoxW's signature and IDOK/IDYES/IDNO/IDCANCEL
// results. Supports MB_OK, MB_OKCANCEL, MB_YESNO and MB_YESNOCANCEL, MB_DEFBUTTONn and the icon beeps.
// It centres over the owner's top-level window; with no visible, non-minimised owner (e.g. in the tray)
// it centres on the primary monitor as a taskbar window. Esc and Alt+F4 answer cancel, or no. Every dialog here
// gives the owner the focus back as it closes only if one of the owner's windows is in front at that moment, so a
// dialog that ends while the user is in a game leaves the game in front: focus afterwards says where the user is.
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

// A modal dialog with `text`, a progress bar and one plain button (`cancelLabel`) while work runs elsewhere: the
// owner's messages are dispatched in its loop, so the work reports through them. `*handle` is the dialog's window
// while it is open, nullptr otherwise. Send it progressPercent (wParam 0-100) to move the bar and add " · N%" to the
// text, and progressDone to close it. Returns true when progressDone closed it; false for the button, Esc, the
// close key, a quit, or at once while dialogs are suppressed.
// Above the app's own WM_APP + n messages (app_internal.h), so the two can never be mistaken for each other.
inline constexpr UINT progressPercent = WM_APP + 0x100, progressDone = WM_APP + 0x101;
bool progressBox(HWND owner, const wchar_t* text, const wchar_t* caption, const wchar_t* cancelLabel, HWND* handle);

// A dialog whose body is a child window the caller makes: `content` is called with the dialog and the body's
// rectangle (device pixels) and returns the child it placed there. The body is at most `width` × `height` logical
// pixels, and smaller when the screen is. One button, `button`, closes it, as do Esc and Enter. The scrolling keys
// (Up/Down, Page Up/Down, Home/End) and the mouse wheel go to the body, so a long one scrolls inside the dialog.
// `text` is the body as plain text: the dialog's description for screen readers, and shown instead when the body
// cannot be made.
// When the body cannot be made, the plain text shows in a dialog fitted to its height.
// Opens nothing while dialogs are suppressed.
// The body takes contentBoxBodyId: a button's id is its answer (IDOK, ...), so a body must not use those.
inline constexpr int contentBoxBodyId = 203;
void contentBox(HWND owner, const wchar_t* caption, int width, int height,
                const std::function<HWND(HWND parent, RECT area)>& content, const wchar_t* button, const std::wstring& text);

// The same for a body that knows its own height: once the body is made, `fit` returns the height it needs (device
// pixels), and the dialog shrinks to it when that is less than `height`; it is asked again after a DPI change.
void contentBox(HWND owner, const wchar_t* caption, int width, int height,
                const std::function<HWND(HWND parent, RECT area)>& content, const std::function<int()>& fit,
                const wchar_t* button, const std::wstring& text);
} // namespace refarter
