#include "hotkey.h"
#include "raw_hotkey.h"
#include <array>
#include <iostream>

namespace {
std::array<bool, 256> keyboard{}; // What GetAsyncKeyState would report.
bool fakeKeyDown(UINT key) { return keyboard[key & 0xFF]; }
} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    using namespace refarter;
    using P = HotkeyProblem;
    check(checkHotkey(defaultHotkey) == P::None, "default accepted");
    check(defaultHotkey == Hotkey{MOD_CONTROL | MOD_SHIFT, VK_F10}, "default is ctrl + shift + f10");
    check(checkHotkey({0, VK_F9}) == P::None, "bare function key accepted");
    check(checkHotkey({MOD_SHIFT, VK_PAUSE}) == P::None, "shift + pause accepted");
    check(checkHotkey({MOD_ALT, 'R'}) == P::None, "alt + letter accepted");
    check(checkHotkey({MOD_WIN | MOD_SHIFT, '5'}) == P::None, "win + shift + digit accepted");
    check(checkHotkey({0, 'R'}) == P::NeedsModifier, "bare letter rejected");
    check(checkHotkey({MOD_SHIFT, 'R'}) == P::NeedsModifier, "shift + letter rejected");
    check(checkHotkey({MOD_CONTROL, VK_SHIFT}) == P::ModifierOnly, "modifier-only rejected");
    check(checkHotkey({MOD_CONTROL, VK_LWIN}) == P::ModifierOnly, "win key rejected as final key");
    check(checkHotkey({MOD_CONTROL, VK_F12}) == P::Reserved, "f12 reserved");
    check(checkHotkey({MOD_ALT, VK_F4}) == P::Reserved, "alt + f4 reserved");
    check(checkHotkey({MOD_CONTROL, VK_F4}) == P::None, "ctrl + f4 accepted");
    check(checkHotkey({MOD_CONTROL | MOD_NOREPEAT, 'R'}) == P::Invalid, "unknown modifier rejected");
    check(checkHotkey({MOD_CONTROL, VK_LBUTTON}) == P::Invalid, "mouse button rejected");
    check(parseHotkey(L"ctrl + shift + f10") == defaultHotkey, "default parses");
    check(parseHotkey(L"Shift+CTRL+F10") == defaultHotkey, "case and spacing ignored");
    check(parseHotkey(L"alt + win + page up") == Hotkey{MOD_ALT | MOD_WIN, VK_PRIOR}, "named key parses");
    check(parseHotkey(L"ctrl + num plus") == Hotkey{MOD_CONTROL, VK_ADD}, "numpad plus parses");
    check(parseHotkey(L"alt + key 192") == Hotkey{MOD_ALT, VK_OEM_3}, "numeric key parses");
    check(parseHotkey(L"f9") == Hotkey{0, VK_F9}, "bare function key parses");
    for (const auto invalid : {L"", L"ctrl", L"ctrl + shift", L"r", L"ctrl + r + t", L"ctrl + ", L"ctrl + banana",
                               L"alt + f4", L"ctrl + f12", L"ctrl + key", L"ctrl + key 9999999", L"ctrl + key 1"}) {
        check(!parseHotkey(invalid), "invalid config value accepted");
    }
    for (const Hotkey hotkey : {defaultHotkey, Hotkey{MOD_ALT | MOD_WIN, 'K'}, Hotkey{MOD_CONTROL, VK_ADD},
                                Hotkey{MOD_SHIFT, VK_SCROLL}, Hotkey{MOD_ALT, VK_OEM_3}, Hotkey{MOD_CONTROL, VK_NUMPAD7}}) {
        check(parseHotkey(hotkeyText(hotkey)) == hotkey, "config round trip");
    }
    check(hotkeyText(defaultHotkey) == L"ctrl + shift + f10", "default text");
    check(hotkeyText({MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN, VK_NUMPAD7}) == L"ctrl + alt + shift + win + num 7", "modifier order");
    check(hotkeyText({MOD_ALT, VK_OEM_3}, L"ö") == L"alt + ö", "layout key name");
    check(hotkeyText({MOD_ALT, VK_OEM_3}) == L"alt + key 192", "unnamed key fallback");
    check(heldModifiersText(0).empty(), "no modifiers held");
    check(heldModifiersText(MOD_SHIFT | MOD_CONTROL) == L"ctrl + shift + …", "held modifiers");
    // Raw Input events as delivered; the fake keyboard follows them unless a test says otherwise.
    HotkeyPress press(fakeKeyDown);
    const auto key = [&](UINT vk, bool down) { keyboard[vk] = down; return press.key(vk, down); };
    keyboard = {};
    press.reset({0, VK_F8});
    check(!key('A', true), "unrelated game keys do not save");
    check(key(VK_F8, true), "bare shortcut saves on first press");
    check(!key(VK_F8, true), "held shortcut does not repeat saves");
    check(!key(VK_F8, false) && key(VK_F8, true), "release rearms the shortcut");
    keyboard = {};
    press.reset({MOD_CONTROL | MOD_SHIFT, '9'});
    check(!key('9', true), "missing modifiers do not save");
    key(VK_LCONTROL, true);
    key(VK_RSHIFT, true);
    check(!key('9', true), "adding modifiers while shortcut held does not save");
    key('9', false);
    check(key('9', true), "mixed-side modifiers and a digit save without function keys");
    key('9', false);
    key(VK_LMENU, true);
    check(!key('9', true), "extra modifiers do not match");
    key('9', false);
    key(VK_LMENU, false);
    key(VK_RCONTROL, true);
    key(VK_LCONTROL, false);
    check(key('9', true), "releasing left ctrl preserves held right ctrl");
    keyboard = {};
    press.reset({MOD_ALT, VK_F8});
    key(VK_RMENU, true);
    check(key(VK_F8, true), "right alt is a supported modifier");
    keyboard = {};
    press.reset({0, VK_F8});
    check(key(VK_F8, true), "reset clears old press and modifier state");
    key(VK_F8, false);

    // Ctrl+Shift+Esc opens an elevated Task Manager: its key-ups never reach Raw Input.
    keyboard = {};
    press.reset({MOD_CONTROL, VK_F10});
    key(VK_LCONTROL, true);
    key(VK_LSHIFT, true);
    keyboard[VK_LCONTROL] = keyboard[VK_LSHIFT] = false;
    key(VK_LCONTROL, true);
    check(key(VK_F10, true), "modifier released unseen does not block the shortcut");
    key(VK_F10, false);
    key(VK_LCONTROL, false);
    check(!key(VK_F10, true), "stuck shift does not turn a bare key into ctrl + f10");
    key(VK_F10, false);
    // Win+L: the shortcut's own release happened on the lock screen.
    keyboard = {};
    press.reset({MOD_CONTROL, VK_F10});
    key(VK_LCONTROL, true);
    check(key(VK_F10, true), "ctrl + f10 saves");
    keyboard[VK_F10] = keyboard[VK_LCONTROL] = false;
    key('W', true);
    key('W', false);
    key(VK_LCONTROL, true);
    check(key(VK_F10, true), "missed shortcut release does not swallow the next press");
    key(VK_F10, false);
    key(VK_LCONTROL, false);
    // A key-down missed while an elevated window had focus.
    keyboard = {};
    press.reset({MOD_CONTROL | MOD_SHIFT, VK_F10});
    keyboard[VK_RCONTROL] = true;
    key(VK_LSHIFT, true);
    check(key(VK_F10, true), "modifier pressed unseen still matches");
    key(VK_F10, false);
    keyboard = {};
    // A modifier let go before the UI thread handled the shortcut message.
    keyboard = {};
    press.reset({MOD_CONTROL, VK_F10});
    key(VK_LCONTROL, true);
    keyboard[VK_F10] = true;
    keyboard[VK_LCONTROL] = false;
    check(press.key(VK_F10, true), "quick release after the press still saves");
    key(VK_F10, false);
    // Current state unavailable (all keys up): matching is exactly the tracked events.
    keyboard = {};
    press.reset({MOD_CONTROL, VK_F10});
    press.key(VK_LCONTROL, true);
    check(press.key(VK_F10, true), "tracked events alone still save");
    check(!press.key(VK_F10, true), "tracked events alone still ignore repeat");
    press.key(VK_F10, false);
    press.key(VK_LCONTROL, false);
    keyboard = {};

    PressReports reports;
    check(reports.accept(true, 1000) && !reports.accept(false, 1001), "hotkey duplicate of a raw press dropped");
    check(reports.accept(false, 1200) && !reports.accept(true, 1201), "raw duplicate of a hotkey press dropped");
    check(reports.accept(true, 1300), "quick next press after a pair saves");
    check(reports.accept(true, 1400) && reports.accept(true, 1500), "raw-only presses (hotkeys blocked) all save");
    check(reports.accept(false, 5000) && reports.accept(false, 5100), "hotkey-only presses (elevated window) all save");
    check(reports.accept(true, 9000) && reports.accept(false, 9600), "late report counts as a new press");
    return failures ? 1 : 0;
}
