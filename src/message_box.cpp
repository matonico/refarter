#include "message_box.h"
#include "theme.h"

#include <commctrl.h>
#include <initguid.h> // Defines the accessibility GUIDs and property ids declared in oleacc.h.
#include <oleacc.h>
#include <algorithm>
#include <atomic>
#include <cwchar>
#include <string>
#include <vector>

namespace refarter {

namespace {
using namespace theme;
constexpr wchar_t className[] = L"RefarterMessageBox";
// Word-wrap that also breaks over-long words mid-word; the ellipsis only shows when the height is capped.
constexpr UINT wrapFlags = DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS;
constexpr int padding = 24, maxTextWidth = 360, minWidth = 320, buttonHeight = 34, buttonMinWidth = 88, buttonGap = 8;
constexpr COLORREF accentPressed = RGB(255, 170, 119); // Held down: slightly lighter than the resting accent, so the press shows.
// The heading and the message are real static controls; the buttons use the dialog result as their id
// (IDOK, IDYES, IDNO, IDCANCEL), which is also what UI Automation reports as their automation id.
constexpr int headingId = 200, messageId = 201, checkId = 202;
constexpr int checkBox = 16, checkGap = 8, checkToButtons = 16; // The optional checkbox: box size, box-to-label gap, gap to the buttons.
constexpr COLORREF checkOutline = RGB(58, 62, 72);
constexpr int choiceBase = 101; // Result of the first custom button in choiceBox.

struct Button {
    std::wstring label;
    int result = 0;
    RECT rect{};
    HWND window = nullptr;
};

// The dialog is made of real child controls (two statics and the push buttons) that only paint themselves in
// Refarter's style, so UI Automation and screen readers see a dialog with text and named buttons, the
// default button, and the focused one. The system classes provide names, roles, focus and invoke.
class Dialog {
public:
    Dialog(HWND owner, const wchar_t* text, const wchar_t* caption, UINT type)
        : text_(text ? text : L""), type_(type) {
        // Every message is a sentence, including the ones built from an error text.
        if (!text_.empty() && !wcschr(L".!?…", text_.back())) text_ += L'.';
        if (caption && *caption && _wcsicmp(caption, L"refarter") != 0) heading_ = caption;
        const HWND root = owner ? GetAncestor(owner, GA_ROOT) : nullptr;
        if (root && IsWindowVisible(root) && !IsIconic(root)) root_ = root;
        switch (type & MB_TYPEMASK) {
        case MB_OKCANCEL: buttons_ = {{L"ok", IDOK}, {L"cancel", IDCANCEL}}; cancel_ = IDCANCEL; break;
        case MB_YESNO: buttons_ = {{L"yes", IDYES}, {L"no", IDNO}}; cancel_ = IDNO; break;
        case MB_YESNOCANCEL: buttons_ = {{L"yes", IDYES}, {L"no", IDNO}, {L"cancel", IDCANCEL}}; cancel_ = IDCANCEL; break;
        default: buttons_ = {{L"ok", IDOK}}; cancel_ = IDOK; break;
        }
        const int preferred = static_cast<int>((type & MB_DEFMASK) >> 8);
        focus_ = preferred < static_cast<int>(buttons_.size()) ? preferred : 0;
        primary_ = focus_;
        result_ = cancel_;
    }
    // Custom buttons: the choices in order (results 101 and up), the last one a dismiss that Esc also answers.
    Dialog(HWND owner, const wchar_t* text, const wchar_t* caption, const std::vector<std::wstring>& choices)
        : Dialog(owner, text, caption, static_cast<UINT>(MB_OK | MB_ICONQUESTION)) {
        buttons_.clear();
        for (size_t i = 0; i < choices.size(); ++i)
            buttons_.push_back({choices[i], i + 1 == choices.size() ? IDCANCEL : choiceBase + static_cast<int>(i)});
        cancel_ = IDCANCEL;
        focus_ = primary_ = static_cast<int>(buttons_.size()) - 1; // The dismiss is the default: Enter never persists a choice.
        result_ = cancel_;
    }
    // Adds a checkbox in the button row; `checked` is read as the starting state and written when the dialog closes.
    void addCheck(const wchar_t* label, bool* checked) {
        checkLabel_ = label ? label : L"";
        checked_ = checked;
        checkValue_ = checked && *checked;
        if (checkLabel_.empty() || !checked) checked_ = nullptr;
    }
    ~Dialog() {
        if (small_) DeleteObject(small_);
        if (body_) DeleteObject(body_);
        if (title_) DeleteObject(title_);
    }
    Dialog(const Dialog&) = delete;
    Dialog& operator=(const Dialog&) = delete;

    int run() {
        if (!create()) return MessageBoxW(root_, text_.c_str(), heading_.empty() ? L"refarter" : heading_.c_str(), type_);
        // Only re-enable an owner this dialog disabled, so nested boxes leave it as they found it.
        const bool disabled = root_ && IsWindowEnabled(root_);
        if (disabled) EnableWindow(root_, FALSE);
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
        SetFocus(focusWindow()); // The safe (default) button has the focus, for keyboard and screen readers.
        const UINT icon = type_ & MB_ICONMASK;
        if (icon != 0) MessageBeep(icon);
        while (!done_) {
            MSG message{};
            const BOOL status = GetMessageW(&message, nullptr, 0, 0);
            if (status == 0) {
                PostQuitMessage(static_cast<int>(message.wParam)); // Keep the app's quit for its own loop.
                break;
            }
            if (status < 0) break;
            if (key(message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (checked_) *checked_ = checkValue_;
        if (disabled) EnableWindow(root_, TRUE);
        clearDescription();
        DestroyWindow(window_);
        if (disabled) SetForegroundWindow(root_);
        return result_;
    }

    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        auto* dialog = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (dialog) {
            switch (message) {
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(window, &ps);
                dialog->paint(dc);
                EndPaint(window, &ps);
                return 0;
            }
            case WM_PRINTCLIENT: dialog->paint(reinterpret_cast<HDC>(wParam)); return 0;
            case WM_CLOSE: dialog->finish(dialog->cancel_); return 0;
            case WM_COMMAND:
                // A button was pressed, with the mouse, Enter/Space, or an accessibility client's invoke.
                if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == checkId) {
                    dialog->checkValue_ = SendMessageW(dialog->checkWindow_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    InvalidateRect(dialog->checkWindow_, nullptr, FALSE);
                    return 0;
                }
                if (HIWORD(wParam) == BN_CLICKED) {
                    for (const auto& button : dialog->buttons_)
                        if (button.result == LOWORD(wParam)) { dialog->finish(button.result); return 0; }
                }
                break;
            case WM_ACTIVATE:
                // The dialog manager would restore its own idea of the focus; the safe button gets it.
                DefDlgProcW(window, message, wParam, lParam);
                if (LOWORD(wParam) != WA_INACTIVE && dialog->focus_ < static_cast<int>(dialog->buttons_.size()) && dialog->focusWindow())
                    SetFocus(dialog->focusWindow());
                return 0;
            }
        }
        return DefDlgProcW(window, message, wParam, lParam);
    }

private:
    int px(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }

    static int measure(HDC dc, HFONT font, const std::wstring& text, int width, UINT flags, int& height) {
        const auto previous = SelectObject(dc, font);
        RECT box{0, 0, width, 0};
        DrawTextW(dc, text.c_str(), -1, &box, DT_CALCRECT | DT_NOPREFIX | flags);
        SelectObject(dc, previous);
        height = box.bottom;
        return box.right;
    }

    bool create() {
        dpi_ = root_ ? GetDpiForWindow(root_) : GetDpiForSystem();
        if (!dpi_) dpi_ = 96;
        body_ = theme::font(dpi_, 14, FW_NORMAL);
        title_ = theme::font(dpi_, 17, FW_SEMIBOLD);
        small_ = theme::font(dpi_, 13, FW_NORMAL);
        RECT bounds{};
        MONITORINFO monitor{sizeof(monitor)};
        if (root_) {
            GetWindowRect(root_, &bounds);
            GetMonitorInfoW(MonitorFromWindow(root_, MONITOR_DEFAULTTONEAREST), &monitor);
        } else {
            GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
            bounds = monitor.rcWork;
        }
        const RECT work = monitor.rcWork;
        HDC dc = GetDC(nullptr);
        int headingHeight = 0, textHeight = 0, buttonRow = 0, content = 0;
        // DT_EDITCONTROL breaks over-long words (clip names without spaces) instead of widening the dialog.
        const int maxWidth = px(maxTextWidth);
        if (!heading_.empty()) content = std::min(maxWidth, measure(dc, title_, heading_, maxWidth, wrapFlags, headingHeight));
        if (!text_.empty()) content = std::max(content, std::min(maxWidth, measure(dc, body_, text_, maxWidth, wrapFlags, textHeight)));
        for (auto& button : buttons_) {
            int unused = 0;
            const int labelWidth = measure(dc, body_, button.label, px(200), DT_SINGLELINE, unused);
            button.rect = {0, 0, std::max(px(buttonMinWidth), labelWidth + px(32)), px(buttonHeight)};
            buttonRow += button.rect.right + (buttonRow ? px(buttonGap) : 0);
        }
        int checkWidth = 0;
        if (checked_) {
            int unused = 0;
            checkWidth = px(checkBox + checkGap) + measure(dc, small_, checkLabel_, px(220), DT_SINGLELINE, unused);
            buttonRow += px(checkToButtons) + checkWidth; // The box shares the row with the buttons.
        }
        ReleaseDC(nullptr, dc);

        const int width = std::max(px(minWidth), std::max(content, buttonRow) + px(padding * 2));
        const int textWidth = width - px(padding * 2);
        // Keep very long text within ~60% of the work area; the last visible line gets an ellipsis.
        const int fixedHeight = px(padding * 3 + 8 + buttonHeight);
        const int maxTextBlock = std::max<int>(px(80), (work.bottom - work.top) * 6 / 10 - fixedHeight);
        headingHeight = std::min(headingHeight, maxTextBlock / 3);
        textHeight = std::min(textHeight, maxTextBlock - headingHeight);
        int y = px(padding);
        headingRect_ = {px(padding), y, px(padding) + textWidth, y + headingHeight};
        if (headingHeight) y += headingHeight + px(8);
        textRect_ = {px(padding), y, px(padding) + textWidth, y + textHeight};
        y += textHeight + (textHeight || headingHeight ? px(padding) : 0);
        int x = width - px(padding);
        for (size_t i = buttons_.size(); i-- > 0;) {
            const int w = buttons_[i].rect.right;
            buttons_[i].rect = {x - w, y, x, y + px(buttonHeight)};
            x -= w + px(buttonGap);
        }
        if (checked_) {
            const int top = y + (px(buttonHeight) - px(checkBox)) / 2;
            checkRect_ = {px(padding), top, px(padding) + checkWidth, top + px(checkBox)};
        }
        const int height = y + px(buttonHeight) + px(padding);

        int left = (bounds.left + bounds.right - width) / 2, top = (bounds.top + bounds.bottom - height) / 2;
        left = std::max<int>(work.left, std::min<int>(left, work.right - width));
        top = std::max<int>(work.top, std::min<int>(top, work.bottom - height));

        if (!registerClass()) return false;
        const DWORD extra = root_ ? 0 : WS_EX_APPWINDOW | WS_EX_TOPMOST;
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        // WS_CLIPCHILDREN: the controls paint themselves; the dialog paints only the gaps.
        window_ = CreateWindowExW(extra, className, heading_.empty() ? L"refarter" : heading_.c_str(), WS_POPUP | WS_CLIPCHILDREN,
                                  left, top, width, height, root_, nullptr, instance, this);
        if (!window_) return false;
        roundedByDwm_ = roundWindowCorners(window_);
        setWindowBorderColour(window_, border);
        createControls(instance);
        describe();
        return true;
    }

    // A dialog class (DLGWINDOWEXTRA, DefDlgProc) lets the system treat the window as a dialog box.
    static bool registerClass() {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW existing{sizeof(existing)};
        if (GetClassInfoExW(instance, className, &existing)) return true;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = proc;
        wc.cbWndExtra = DLGWINDOWEXTRA;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = className;
        return RegisterClassExW(&wc) != 0;
    }

    void createControls(HINSTANCE instance) {
        const auto addText = [&](const std::wstring& text, const RECT& rect, int id) {
            if (text.empty() || rect.bottom <= rect.top) return;
            // A plain left-aligned static, so it is exposed as text; the subclass only changes how it paints.
            const HWND control = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, window_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
            if (!control) return;
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(id == headingId ? title_ : body_), FALSE);
            SetWindowSubclass(control, textProc, static_cast<UINT_PTR>(id), reinterpret_cast<DWORD_PTR>(this));
        };
        addText(heading_, headingRect_, headingId);
        addText(text_, textRect_, messageId);
        for (size_t i = 0; i < buttons_.size(); ++i) {
            auto& button = buttons_[i];
            // The default button is a real BS_DEFPUSHBUTTON, so it reports as the default to assistive technology.
            button.window = CreateWindowExW(0, L"BUTTON", button.label.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | (static_cast<int>(i) == primary_ ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                button.rect.left, button.rect.top, button.rect.right - button.rect.left, button.rect.bottom - button.rect.top,
                window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(button.result)), instance, nullptr);
            if (!button.window) continue;
            SendMessageW(button.window, WM_SETFONT, reinterpret_cast<WPARAM>(body_), FALSE);
            SetWindowSubclass(button.window, buttonProc, static_cast<UINT_PTR>(i), reinterpret_cast<DWORD_PTR>(this));
        }
        if (checked_) {
            checkWindow_ = CreateWindowExW(0, L"BUTTON", checkLabel_.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                checkRect_.left, checkRect_.top, checkRect_.right - checkRect_.left, checkRect_.bottom - checkRect_.top,
                window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(checkId)), instance, nullptr);
            if (checkWindow_) {
                SendMessageW(checkWindow_, WM_SETFONT, reinterpret_cast<WPARAM>(small_), FALSE);
                SendMessageW(checkWindow_, BM_SETCHECK, checkValue_ ? BST_CHECKED : BST_UNCHECKED, 0);
                SetWindowSubclass(checkWindow_, checkProc, 0, reinterpret_cast<DWORD_PTR>(this));
            }
        }
        // The dialog's default push button (also what Enter would press in a system dialog).
        SendMessageW(window_, DM_SETDEFID, static_cast<WPARAM>(buttons_[primary_].result), 0);
    }

    // The dialog's description is its message, for clients that read a dialog as a whole.
    void describe() const {
        IAccPropServices* services = nullptr;
        if (FAILED(CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&services))) || !services) return;
        if (!text_.empty())
            services->SetHwndPropStr(window_, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, PROPID_ACC_DESCRIPTION, text_.c_str());
        services->Release();
    }

    // The description lives in the accessibility property store, keyed by the window handle: remove it before the
    // handle goes (and could be reused), so nothing outlives the dialog.
    void clearDescription() const {
        if (!window_ || text_.empty()) return;
        IAccPropServices* services = nullptr;
        if (FAILED(CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&services))) || !services) return;
        const MSAAPROPID properties[] = {PROPID_ACC_DESCRIPTION};
        services->ClearHwndProps(window_, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, properties, static_cast<int>(std::size(properties)));
        services->Release();
    }

    static LRESULT CALLBACK textProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data) {
        auto* dialog = reinterpret_cast<Dialog*>(data);
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window, &ps);
            dialog->paintText(window, static_cast<int>(id), dc);
            EndPaint(window, &ps);
            return 0;
        }
        case WM_PRINTCLIENT: dialog->paintText(window, static_cast<int>(id), reinterpret_cast<HDC>(wParam)); return 0;
        case WM_NCDESTROY: RemoveWindowSubclass(window, textProc, id); break;
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK checkProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data) {
        auto* dialog = reinterpret_cast<Dialog*>(data);
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window, &ps);
            dialog->paintCheck(window, dc);
            EndPaint(window, &ps);
            return 0;
        }
        case WM_PRINTCLIENT: dialog->paintCheck(window, reinterpret_cast<HDC>(wParam)); return 0;
        case WM_SETFOCUS: dialog->focus_ = -1; return repainted(window, message, wParam, lParam);
        case WM_KILLFOCUS: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_CAPTURECHANGED:
        case BM_SETSTATE: case BM_SETSTYLE: case BM_SETCHECK: case WM_ENABLE:
            return repainted(window, message, wParam, lParam);
        case WM_NCDESTROY: RemoveWindowSubclass(window, checkProc, id); break;
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK buttonProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR index, DWORD_PTR data) {
        auto* dialog = reinterpret_cast<Dialog*>(data);
        const int i = static_cast<int>(index);
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window, &ps);
            dialog->paintButton(window, i, dc);
            EndPaint(window, &ps);
            return 0;
        }
        case WM_PRINTCLIENT: dialog->paintButton(window, i, reinterpret_cast<HDC>(wParam)); return 0;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            dialog->setHover(i);
            break;
        }
        case WM_MOUSELEAVE: if (dialog->hover_ == i) dialog->setHover(-1); break; // Not a neighbour's hover.
        case WM_SETFOCUS: dialog->focus_ = i; return repainted(window, message, wParam, lParam);
        // The classic button draws its pressed, focused and default states straight onto the screen, bypassing
        // WM_PAINT (no visual-styles manifest here). Each of these changes one of them: let it run, then paint over.
        case WM_KILLFOCUS: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_CAPTURECHANGED:
        case BM_SETSTATE: case BM_SETSTYLE: case WM_ENABLE:
            return repainted(window, message, wParam, lParam);
        case WM_NCDESTROY: RemoveWindowSubclass(window, buttonProc, index); break;
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    static LRESULT repainted(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        return result;
    }

    void setHover(int index) {
        if (hover_ == index) return;
        const int previous = hover_;
        hover_ = index;
        for (const int i : {previous, index})
            if (i >= 0 && i < static_cast<int>(buttons_.size()) && buttons_[i].window) InvalidateRect(buttons_[i].window, nullptr, FALSE);
    }

    void finish(int result) {
        result_ = result;
        done_ = true;
        if (window_) PostMessageW(window_, WM_NULL, 0, 0); // Wakes the loop when the answer arrived as a sent message.
    }

    // The window holding the keyboard focus: the checkbox (focus_ -1) or a button.
    HWND focusWindow() const { return focus_ < 0 ? checkWindow_ : buttons_[focus_].window; }

    // Tab order: the checkbox (when there is one), then the buttons.
    void moveFocus(int step) {
        const int first = checkWindow_ ? -1 : 0, count = static_cast<int>(buttons_.size()) - first;
        focus_ = first + (focus_ - first + step + count) % count;
        keyboard_ = true;
        SetFocus(focusWindow());
        for (const auto& button : buttons_) if (button.window) InvalidateRect(button.window, nullptr, FALSE);
        if (checkWindow_) InvalidateRect(checkWindow_, nullptr, FALSE);
    }

    // Keys are taken in this loop, so the app's own Esc/Enter/Tab routing never sees them.
    bool key(const MSG& message) {
        switch (message.message) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            switch (message.wParam) {
            case VK_ESCAPE: finish(cancel_); break;
            case VK_F4: if (message.message == WM_SYSKEYDOWN) finish(cancel_); break;
            case VK_RETURN: finish(buttons_[focus_ < 0 ? primary_ : focus_].result); break; // Enter on the box presses the default.
            case VK_SPACE:
                if (focus_ < 0) { keyboard_ = true; SendMessageW(checkWindow_, BM_CLICK, 0, 0); } // Toggles the box.
                else finish(buttons_[focus_].result);
                break;
            case VK_TAB: moveFocus(GetKeyState(VK_SHIFT) < 0 ? -1 : 1); break;
            case VK_LEFT: moveFocus(-1); break;
            case VK_RIGHT: moveFocus(1); break;
            }
            return true;
        case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR:
            return true;
        }
        return false;
    }

    // The dialog surface and its outline; the controls paint themselves over it.
    void paint(HDC target) {
        RECT client{};
        GetClientRect(window_, &client);
        const HDC dc = buffer_.acquire(target, client.right, client.bottom);
        if (!dc) return;
        fill(dc, client, surface);
        if (!roundedByDwm_) {
            const auto brush = CreateSolidBrush(border);
            FrameRect(dc, &client, brush);
            DeleteObject(brush);
        }
        buffer_.present(target);
    }

    // The heading and the message, drawn as before inside the static control's rectangle.
    void paintText(HWND window, int id, HDC target) {
        RECT rect{};
        GetClientRect(window, &rect);
        const HDC dc = itemBuffer_.acquire(target, rect.right, rect.bottom);
        if (!dc) return;
        fill(dc, rect, surface);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ink);
        const bool heading = id == headingId;
        const auto oldFont = SelectObject(dc, heading ? title_ : body_);
        const auto& text = heading ? heading_ : text_;
        DrawTextW(dc, text.c_str(), -1, &rect, wrapFlags | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        itemBuffer_.present(target);
    }

    // A 16 px box with 4 px corners and its label: raised with an outline when clear, accent with a dark tick when ticked.
    void paintCheck(HWND window, HDC target) {
        RECT rect{};
        GetClientRect(window, &rect);
        const HDC dc = itemBuffer_.acquire(target, rect.right, rect.bottom);
        if (!dc) return;
        fill(dc, rect, surface);
        const bool on = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const int size = px(checkBox);
        const RECT box{0, (rect.bottom - size) / 2, size, (rect.bottom - size) / 2 + size};
        roundRect(dc, box, px(4), on ? accent : raised, on ? accent : checkOutline);
        if (on) {
            Gdiplus::Graphics graphics(dc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen pen(color(background), std::max(1.5f, static_cast<float>(dpi_) / 96.f * 1.75f));
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            const auto at = [&](float x, float y) {
                return Gdiplus::PointF(box.left + x * size / 16.f, box.top + y * size / 16.f);
            };
            const Gdiplus::PointF tick[] = {at(4.2f, 8.4f), at(6.8f, 11.0f), at(11.8f, 5.2f)};
            graphics.DrawLines(&pen, tick, 3);
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, on ? ink : muted);
        const auto oldFont = SelectObject(dc, small_);
        RECT label{size + px(checkGap), rect.top, rect.right, rect.bottom};
        DrawTextW(dc, checkLabel_.c_str(), -1, &label, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        if (keyboard_ && GetFocus() == window) focusGlow(dc, box, static_cast<float>(dpi_) / 96.f, 4.f);
        itemBuffer_.present(target);
    }

    void paintButton(HWND window, int i, HDC target) {
        RECT rect{};
        GetClientRect(window, &rect);
        const HDC dc = itemBuffer_.acquire(target, rect.right, rect.bottom);
        if (!dc) return;
        fill(dc, rect, surface);
        const bool primary = i == primary_, lit = i == hover_;
        // Held down (mouse button or space): a lighter accent or the border fill, with the label one pixel lower.
        const bool pressed = (SendMessageW(window, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
        const COLORREF fillColour = primary ? (pressed ? accentPressed : lit ? accentHover : accent) : (pressed ? border : lit ? raised : surface);
        roundRect(dc, rect, px(7), fillColour, primary ? fillColour : border);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, primary ? background : ink);
        const auto oldFont = SelectObject(dc, body_);
        if (pressed) OffsetRect(&rect, 0, px(1));
        DrawTextW(dc, buttons_[i].label.c_str(), -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        if (pressed) OffsetRect(&rect, 0, -px(1));
        SelectObject(dc, oldFont);
        if (keyboard_ && GetFocus() == window) focusGlow(dc, rect, static_cast<float>(dpi_) / 96.f);
        itemBuffer_.present(target);
    }

    std::wstring text_, heading_, checkLabel_;
    theme::BackBuffer buffer_, itemBuffer_;
    UINT type_;
    HWND root_ = nullptr, window_ = nullptr;
    UINT dpi_ = 96;
    HFONT body_ = nullptr, title_ = nullptr, small_ = nullptr;
    bool* checked_ = nullptr;
    bool checkValue_ = false;
    HWND checkWindow_ = nullptr;
    RECT checkRect_{};
    std::vector<Button> buttons_;
    RECT headingRect_{}, textRect_{};
    int cancel_ = IDOK, result_ = IDOK;
    int focus_ = 0, primary_ = 0, hover_ = -1;
    bool keyboard_ = false, roundedByDwm_ = false, done_ = false;
};
} // namespace

namespace {
std::atomic<int> suppressed{0};
} // namespace

void suppressDialogs(bool on) {
    if (on) ++suppressed;
    else if (suppressed > 0) --suppressed;
}

int messageBox(HWND owner, const wchar_t* text, const wchar_t* caption, UINT type) {
    if (suppressed > 0) { // A modal dialog would hold a bounded shutdown wait open: answer as Esc would.
        switch (type & MB_TYPEMASK) {
        case MB_OKCANCEL: case MB_YESNOCANCEL: return IDCANCEL;
        case MB_YESNO: return IDNO;
        default: return IDOK;
        }
    }
    Dialog dialog(owner, text, caption, type);
    return dialog.run();
}

int messageBox(HWND owner, const wchar_t* text, const wchar_t* caption, UINT type, const wchar_t* checkLabel, bool* checked) {
    if (suppressed > 0) return messageBox(owner, text, caption, type); // Answers the safe choice; the box is left as it was.
    Dialog dialog(owner, text, caption, type);
    dialog.addCheck(checkLabel, checked);
    return dialog.run();
}

int choiceBox(HWND owner, const wchar_t* text, const wchar_t* caption, const std::vector<std::wstring>& choices) {
    if (suppressed > 0 || choices.size() < 2) return -1; // Nothing opens during a bounded wait: dismissed.
    Dialog dialog(owner, text, caption, choices);
    const int result = dialog.run();
    return result >= choiceBase && result < choiceBase + static_cast<int>(choices.size()) - 1 ? result - choiceBase : -1;
}

} // namespace refarter
