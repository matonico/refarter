#include "combine_dialog.h"
#include "theme.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>

namespace refarter {
namespace {
constexpr wchar_t className[] = L"RefarterCombineDialog";
enum { List = 200, Up, Down };
class OrderDialog {
public:
    HWND window = nullptr, owner = nullptr, list = nullptr;
    std::vector<std::filesystem::path> clips;
    HFONT font = nullptr;
    HBRUSH brush = CreateSolidBrush(theme::background);
    int dpi = 96;
    UINT dragMessage = RegisterWindowMessageW(DRAGLISTMSGSTRING);
    int dragFrom = -1, dropBefore = -1;
    bool draggable = false;
    bool done = false, accepted = false;
    ~OrderDialog() { if (font) DeleteObject(font); DeleteObject(brush); }
    int px(int n) const { return MulDiv(n, dpi, 96); }
    int insertionAt(POINT screen, bool scroll) const {
        POINT at = screen; ScreenToClient(list,&at);
        RECT client{}; GetClientRect(list,&client);
        if (at.x < 0 || at.x >= client.right) return -1;
        const int item = LBItemFromPt(list,screen,scroll);
        const int count = static_cast<int>(clips.size());
        if (at.y < 0) return static_cast<int>(SendMessageW(list,LB_GETTOPINDEX,0,0));
        if (at.y >= client.bottom) {
            const int top = static_cast<int>(SendMessageW(list,LB_GETTOPINDEX,0,0));
            return std::min(count,top + (static_cast<int>(client.bottom) + px(32) - 1) / px(32));
        }
        if (item < 0) return count;
        RECT row{}; SendMessageW(list,LB_GETITEMRECT,item,reinterpret_cast<LPARAM>(&row));
        return item + (at.y >= (row.top + row.bottom) / 2 ? 1 : 0);
    }
    void clearDrag() {
        dragFrom = dropBefore = -1;
        InvalidateRect(list,nullptr,FALSE);
    }
    LRESULT drag(const DRAGLISTINFO& info) {
        switch (info.uNotification) {
        case DL_BEGINDRAG:
            dragFrom = LBItemFromPt(list,info.ptCursor,FALSE);
            if (dragFrom < 0) return FALSE;
            dropBefore = -1;
            SendMessageW(list,LB_SETCURSEL,dragFrom,0); SetFocus(list); buttons();
            return TRUE;
        case DL_DRAGGING:
            dropBefore = insertionAt(info.ptCursor,true);
            InvalidateRect(list,nullptr,FALSE);
            return dropBefore < 0 ? DL_STOPCURSOR : DL_MOVECURSOR;
        case DL_DROPPED: {
            POINT point = info.ptCursor; ScreenToClient(list,&point);
            RECT client{}; GetClientRect(list,&client);
            const int before = PtInRect(&client,point) ? insertionAt(info.ptCursor,false) : -1;
            const int from = dragFrom;
            clearDrag();
            if (from < 0 || before < 0) return 0;
            const int to = before > from ? before - 1 : before;
            if (to != from) {
                // Move the item, keeping the relative order of every other clip.
                if (to < from) std::rotate(clips.begin()+to,clips.begin()+from,clips.begin()+from+1);
                else std::rotate(clips.begin()+from,clips.begin()+from+1,clips.begin()+to+1);
                rows(to);
            }
            return 0;
        }
        case DL_CANCELDRAG: clearDrag(); return 0;
        }
        return 0;
    }
    void rows(int selected) {
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (std::size_t i = 0; i < clips.size(); ++i) {
            const auto text = std::to_wstring(i + 1) + L".  " + clips[i].filename().wstring();
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        }
        SendMessageW(list, LB_SETCURSEL, selected, 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, FALSE);
        buttons();
    }
    void buttons() {
        const int index = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        EnableWindow(GetDlgItem(window, Up), index > 0);
        EnableWindow(GetDlgItem(window, Down), index >= 0 && index + 1 < static_cast<int>(clips.size()));
    }
    void move(int direction) {
        const int at = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0)), next = at + direction;
        if (at < 0 || next < 0 || next >= static_cast<int>(clips.size())) return;
        std::swap(clips[at], clips[next]); rows(next);
        // A move at the end disables its button; a disabled focused control cannot receive Esc.
        if (!IsWindowEnabled(GetFocus())) SetFocus(list);
    }
    void layout() {
        auto next = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        const int ids[] = {List, Up, Down, IDOK, IDCANCEL};
        for (int id : ids) SendMessageW(GetDlgItem(window, id), WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
        if (font) DeleteObject(font); font = next;
        RECT r{}; GetClientRect(window, &r);
        const int w = r.right, h = r.bottom;
        MoveWindow(list, px(24), px(92), w - px(48), h - px(172), TRUE);
        SendMessageW(list, LB_SETITEMHEIGHT, 0, px(32));
        MoveWindow(GetDlgItem(window, Up), px(24), h - px(56), px(90), px(34), TRUE);
        MoveWindow(GetDlgItem(window, Down), px(122), h - px(56), px(104), px(34), TRUE);
        MoveWindow(GetDlgItem(window, IDOK), w - px(234), h - px(56), px(104), px(34), TRUE);
        MoveWindow(GetDlgItem(window, IDCANCEL), w - px(122), h - px(56), px(98), px(34), TRUE);
    }
    static LRESULT CALLBACK proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* d = reinterpret_cast<OrderDialog*>(GetWindowLongPtrW(w, GWLP_USERDATA));
        if (!d) return DefWindowProcW(w, msg, wp, lp);
        if (d->dragMessage && msg == d->dragMessage && lp) {
            const auto& info = *reinterpret_cast<DRAGLISTINFO*>(lp);
            if (info.hWnd == d->list) return d->drag(info);
        }
        switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps; auto dc = BeginPaint(w, &ps);
            RECT r{}; GetClientRect(w, &r); theme::fill(dc, r, theme::surface);
            SetBkMode(dc, TRANSPARENT); const auto oldFont = SelectObject(dc, d->font); SetTextColor(dc, theme::ink);
            RECT title{d->px(24), d->px(22), r.right - d->px(24), d->px(45)};
            DrawTextW(dc, L"combine clips", -1, &title, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
            SetTextColor(dc, theme::muted);
            RECT hint{d->px(24), d->px(52), r.right - d->px(24), d->px(86)};
            DrawTextW(dc, d->draggable ? L"drag clips to reorder · or use move up / move down" :
                L"starts oldest first · select a clip to move it up or down", -1, &hint, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, oldFont); // layout() deletes the previous font on a DPI change.
            EndPaint(w, &ps); return 0;
        }
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            auto r = item->rcItem; const bool selected = (item->itemState & ODS_SELECTED) != 0;
            const bool disabled = (item->itemState & ODS_DISABLED) != 0;
            const auto old = SelectObject(item->hDC, d->font); SetBkMode(item->hDC, TRANSPARENT);
            if (item->CtlID == List) {
                theme::fill(item->hDC, r, selected ? theme::accentSurface : theme::background);
                if (item->itemID != static_cast<UINT>(-1) && item->itemID < d->clips.size()) {
                    r.left += d->px(10); r.right -= d->px(10);
                    const auto text = std::to_wstring(item->itemID + 1) + L".  " + d->clips[item->itemID].filename().wstring();
                    SetTextColor(item->hDC, selected ? theme::accent : theme::ink);
                    DrawTextW(item->hDC, text.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                    if (d->dragFrom >= 0 && (static_cast<int>(item->itemID) == d->dropBefore ||
                        static_cast<int>(item->itemID) + 1 == d->dropBefore)) {
                        RECT line = item->rcItem;
                        if (static_cast<int>(item->itemID) == d->dropBefore) line.bottom = line.top + d->px(2);
                        else line.top = line.bottom - d->px(2);
                        theme::fill(item->hDC,line,theme::accent);
                    }
                }
            } else {
                theme::fill(item->hDC, r, theme::surface);
                Gdiplus::Graphics graphics(item->hDC); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                const bool primary = item->CtlID == IDOK;
                theme::roundRect(graphics, r, d->px(7), primary ? (selected ? theme::accentHover : theme::accent) : theme::raised, theme::border);
                SetTextColor(item->hDC, disabled ? theme::muted : primary ? theme::background : theme::ink);
                wchar_t text[48]{}; GetWindowTextW(item->hwndItem, text, 48);
                DrawTextW(item->hDC, text, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                if (item->itemState & ODS_FOCUS) theme::focusOutline(item->hDC, r, d->dpi / 96.f);
            }
            SelectObject(item->hDC, old); return TRUE;
        }
        case WM_CTLCOLORLISTBOX: SetBkColor(reinterpret_cast<HDC>(wp), theme::background); return reinterpret_cast<LRESULT>(d->brush);
        case WM_COMMAND:
            switch (LOWORD(wp)) {
            case List: if (HIWORD(wp) == LBN_SELCHANGE) d->buttons(); break;
            case Up: d->move(-1); break;
            case Down: d->move(1); break;
            case IDOK: d->accepted = true; d->done = true; break;
            case IDCANCEL: d->done = true; break;
            } return 0;
        case WM_CLOSE: d->done = true; return 0;
        case WM_DESTROY: d->done = true; return 0;
        case WM_DPICHANGED: {
            d->dpi = HIWORD(wp); auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(w, nullptr, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            d->layout(); InvalidateRect(w, nullptr, FALSE); return 0;
        }
        }
        return DefWindowProcW(w, msg, wp, lp);
    }
};
}
bool orderCombinedClips(HWND owner, std::vector<std::filesystem::path>& clips) {
    if (clips.size() < 2 || !IsWindow(owner) || !IsWindowEnabled(owner)) return false;
    OrderDialog d; d.owner = owner; d.clips = clips; d.dpi = static_cast<int>(GetDpiForWindow(owner));
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)}; wc.hInstance = instance; wc.lpfnWndProc = OrderDialog::proc;
    wc.lpszClassName = className; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    RECT parent{}; GetWindowRect(owner, &parent);
    MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
    const int width = std::min(d.px(660), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
    const int desiredHeight = 172 + 32 * static_cast<int>(std::min<std::size_t>(clips.size(),9));
    const int height = std::min(d.px(desiredHeight), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
    const int x = std::clamp(static_cast<int>((parent.left + parent.right - width) / 2), static_cast<int>(monitor.rcWork.left), static_cast<int>(monitor.rcWork.right) - width);
    const int y = std::clamp(static_cast<int>((parent.top + parent.bottom - height) / 2), static_cast<int>(monitor.rcWork.top), static_cast<int>(monitor.rcWork.bottom) - height);
    d.window = CreateWindowExW(WS_EX_CONTROLPARENT, className, L"combine clips", WS_POPUP | WS_CLIPCHILDREN, x,y,width,height,owner,nullptr,instance,&d);
    if (!d.window) return false;
    theme::roundWindowCorners(d.window); theme::setWindowBorderColour(d.window, theme::border);
    d.list = CreateWindowExW(0, L"LISTBOX", L"clip order", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
        0,0,0,0,d.window,reinterpret_cast<HMENU>(List),instance,nullptr);
    SetWindowTheme(d.list, L"DarkMode_Explorer", nullptr);
    d.draggable = d.dragMessage && MakeDragList(d.list);
    for (auto [id, text] : {std::pair<int,const wchar_t*>{Up,L"move up"}, {Down,L"move down"}, {IDOK,L"combine"}, {IDCANCEL,L"cancel"}})
        CreateWindowExW(0,L"BUTTON",text,WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,0,0,0,0,d.window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
    if (!d.list || !GetDlgItem(d.window,Up) || !GetDlgItem(d.window,Down) || !GetDlgItem(d.window,IDOK) || !GetDlgItem(d.window,IDCANCEL)) {
        DestroyWindow(d.window); return false;
    }
    d.layout(); d.rows(0); EnableWindow(owner,FALSE); ShowWindow(d.window,SW_SHOW); SetFocus(GetDlgItem(d.window,IDCANCEL));
    while (!d.done) {
        MSG m{}; const int status = GetMessageW(&m,nullptr,0,0);
        if (status <= 0) { if (!status) PostQuitMessage(static_cast<int>(m.wParam)); break; }
        if (m.message == WM_KEYDOWN && (m.hwnd == d.window || IsChild(d.window,m.hwnd))) {
            if (d.dragFrom >= 0) {
                // Let the native drag list cancel on Esc without closing the ordering window.
                TranslateMessage(&m); DispatchMessageW(&m); continue;
            }
            if (m.wParam == VK_ESCAPE) { d.done = true; continue; }
            if (m.wParam == VK_RETURN) {
                const auto focus = GetFocus();
                if (focus != d.list) SendMessageW(d.window,WM_COMMAND,GetDlgCtrlID(focus),0);
                continue;
            }
        }
        if (!IsDialogMessageW(d.window,&m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    if (IsWindow(owner)) EnableWindow(owner,TRUE);
    if (IsWindow(d.window)) DestroyWindow(d.window);
    if (IsWindow(owner)) { SetForegroundWindow(owner); SetFocus(owner); }
    if (d.accepted) clips = std::move(d.clips);
    return d.accepted;
}
}
