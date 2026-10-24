#include "popup_menu.h"
#include "theme.h"

#include <windowsx.h>
#include <algorithm>
#include <cwctype>
#include <memory>
#include <string>
#include <vector>

namespace refarter {

namespace {
using namespace theme;
constexpr wchar_t className[] = L"RefarterPopupMenu";
constexpr int rowHeight = 34, separatorHeight = 9, edge = 6;

struct Item {
    std::wstring text;
    UINT id = 0;
    bool separator = false, disabled = false, checked = false;
    HMENU submenu = nullptr;
    RECT rect{};
};

struct Level {
    HWND window = nullptr;
    std::vector<Item> items;
    int hover = -1;
    int open = -1; // Item whose submenu is showing.
};

// "&&" is a literal ampersand; a single "&" marks a mnemonic and is not shown.
std::wstring plain(const std::wstring& text) {
    std::wstring result;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'&' && i + 1 < text.size()) ++i;
        else if (text[i] == L'&') continue;
        result += text[i];
    }
    return result;
}

class Session {
public:
    Session(HWND owner, UINT dpi) : owner_(owner), dpi_(dpi) {
        font_ = theme::font(dpi_, 14, FW_NORMAL);
    }
    ~Session() {
        close(0);
        if (font_) DeleteObject(font_);
    }

    UINT run(HMENU menu, POINT at) {
        if (!open(menu, {at.x, at.y, at.x, at.y}, false)) return 0;
        SetCapture(levels_[0]->window);
        const HWND foreground = GetForegroundWindow();
        while (!done_) {
            MSG message{};
            if (!GetMessageW(&message, nullptr, 0, 0)) {
                PostQuitMessage(static_cast<int>(message.wParam)); // Keep the app's quit for its own loop.
                break;
            }
            if (handle(message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
            // Switched apps, or another window took the mouse: dismiss like a native menu.
            if (!done_ && (GetCapture() != levels_[0]->window || GetForegroundWindow() != foreground)) done_ = true;
        }
        if (GetCapture() == (levels_.empty() ? nullptr : levels_[0]->window)) ReleaseCapture();
        return result_;
    }

    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        auto* session = reinterpret_cast<Session*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        switch (message) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
            if (session) {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(window, &ps);
                session->paint(window, dc);
                EndPaint(window, &ps);
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            if (session && !session->levels_.empty() && window == session->levels_[0]->window) session->done_ = true;
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

private:
    int px(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }

    bool open(HMENU menu, RECT anchor, bool beside) {
        auto level = std::make_unique<Level>();
        const int count = GetMenuItemCount(menu);
        HDC dc = GetDC(owner_);
        const auto previous = SelectObject(dc, font_);
        int width = 0, y = px(edge);
        for (int i = 0; i < count; ++i) {
            wchar_t text[512]{};
            MENUITEMINFOW info{sizeof(info)};
            info.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
            info.dwTypeData = text;
            info.cch = static_cast<UINT>(std::size(text));
            if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info)) continue;
            Item item;
            item.separator = (info.fType & MFT_SEPARATOR) != 0;
            item.disabled = (info.fState & (MFS_DISABLED | MFS_GRAYED)) != 0;
            item.checked = (info.fState & MFS_CHECKED) != 0;
            item.id = info.wID;
            item.submenu = info.hSubMenu;
            item.text = plain(text);
            const int height = px(item.separator ? separatorHeight : rowHeight);
            item.rect = {0, y, 0, y + height};
            y += height;
            if (!item.separator) {
                RECT measure{};
                DrawTextW(dc, item.text.c_str(), -1, &measure, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                width = std::max(width, static_cast<int>(measure.right));
            }
            level->items.push_back(std::move(item));
        }
        SelectObject(dc, previous);
        ReleaseDC(owner_, dc);
        if (level->items.empty()) return false;

        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint({anchor.left, anchor.top}, MONITOR_DEFAULTTONEAREST), &monitor);
        const RECT work = monitor.rcWork;
        const int maximum = std::max(px(200), std::min(px(720), static_cast<int>(work.right - work.left) - px(32)));
        const int w = std::clamp(width + px(36 + 40), px(200), maximum);
        const int h = y + px(edge);
        for (auto& item : level->items) item.rect.right = w;
        int x = beside ? anchor.right + px(2) : anchor.left;
        int top = beside ? anchor.top - px(edge) : anchor.top + px(4);
        if (x + w > work.right) x = beside ? anchor.left - w - px(2) : work.right - w - px(4);
        if (top + h > work.bottom) top = beside ? work.bottom - h : anchor.top - h - px(4);
        x = std::max(x, static_cast<int>(work.left) + px(4));
        top = std::max(top, static_cast<int>(work.top) + px(4));

        if (!registerWindowClass(className, proc, CS_DROPSHADOW)) return false;
        level->window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, className, L"menu", WS_POPUP,
                                        x, top, w, h, owner_, nullptr, GetModuleHandleW(nullptr), this);
        if (!level->window) return false;
        roundWindowCorners(level->window, 3); // DWMWCP_ROUNDSMALL
        setWindowBorderColour(level->window, border);
        ShowWindow(level->window, SW_SHOWNOACTIVATE);
        levels_.push_back(std::move(level));
        return true;
    }

    void closeAbove(size_t depth) {
        while (levels_.size() > depth + 1) {
            DestroyWindow(levels_.back()->window);
            levels_.pop_back();
        }
        if (depth < levels_.size()) levels_[depth]->open = -1;
    }

    void close(UINT result) {
        result_ = result;
        done_ = true;
        for (auto it = levels_.rbegin(); it != levels_.rend(); ++it) DestroyWindow((*it)->window);
        levels_.clear();
    }

    // The deepest level under a screen point, and the item there.
    bool at(POINT screen, size_t& depth, int& index) const {
        for (size_t d = levels_.size(); d-- > 0;) {
            RECT bounds{};
            GetWindowRect(levels_[d]->window, &bounds);
            if (!PtInRect(&bounds, screen)) continue;
            depth = d;
            index = -1;
            POINT client = screen;
            ScreenToClient(levels_[d]->window, &client);
            const auto& items = levels_[d]->items;
            for (int i = 0; i < static_cast<int>(items.size()); ++i)
                if (!items[i].separator && PtInRect(&items[i].rect, client)) index = i;
            return true;
        }
        return false;
    }

    void setHover(size_t depth, int index) {
        auto& level = *levels_[depth];
        if (level.hover == index) return;
        level.hover = index;
        InvalidateRect(level.window, nullptr, FALSE);
    }

    void openSubmenu(size_t depth, int index) {
        auto& level = *levels_[depth];
        if (level.open == index) return;
        closeAbove(depth);
        const auto& item = level.items[index];
        if (!item.submenu || item.disabled) return;
        RECT row = item.rect;
        MapWindowPoints(level.window, nullptr, reinterpret_cast<POINT*>(&row), 2);
        if (open(item.submenu, row, true)) level.open = index;
    }

    void choose(size_t depth, int index) {
        const auto& item = levels_[depth]->items[index];
        if (item.disabled || item.separator) return;
        if (item.submenu) {
            openSubmenu(depth, index);
            if (levels_.size() > depth + 1) moveHover(depth + 1, 1, true);
            return;
        }
        close(item.id);
    }

    // Keyboard movement to the next enabled row, wrapping around.
    void moveHover(size_t depth, int step, bool fromStart = false) {
        auto& level = *levels_[depth];
        const int count = static_cast<int>(level.items.size());
        int index = fromStart ? (step > 0 ? -1 : count) : level.hover;
        for (int tries = 0; tries < count; ++tries) {
            index = (index + step + count) % count;
            const auto& item = level.items[index];
            if (!item.separator && !item.disabled) { setHover(depth, index); return; }
        }
    }

    bool handle(const MSG& message) {
        switch (message.message) {
        case WM_MOUSEMOVE: {
            size_t depth = 0;
            int index = -1;
            if (at(message.pt, depth, index)) {
                setHover(depth, index >= 0 && !levels_[depth]->items[index].disabled ? index : -1);
                if (index >= 0) openSubmenu(depth, index);
            } else if (!levels_.empty()) {
                setHover(levels_.size() - 1, -1);
            }
            return true;
        }
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN: {
            size_t depth = 0;
            int index = -1;
            if (!at(message.pt, depth, index)) close(0); // A click elsewhere dismisses; it is not passed on.
            return true;
        }
        case WM_LBUTTONUP: case WM_RBUTTONUP: {
            size_t depth = 0;
            int index = -1;
            if (at(message.pt, depth, index) && index >= 0) choose(depth, index);
            return true;
        }
        case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MOUSEWHEEL:
            return true;
        case WM_KEYDOWN: case WM_SYSKEYDOWN: {
            const size_t depth = levels_.size() - 1;
            auto& level = *levels_[depth];
            switch (message.wParam) {
            case VK_ESCAPE:
                if (depth > 0) closeAbove(depth - 1);
                else close(0);
                break;
            case VK_MENU: case VK_F10: close(0); break;
            case VK_DOWN: moveHover(depth, 1); break;
            case VK_UP: moveHover(depth, -1); break;
            case VK_HOME: moveHover(depth, 1, true); break;
            case VK_END: moveHover(depth, -1, true); break;
            case VK_LEFT: if (depth > 0) closeAbove(depth - 1); break;
            case VK_RIGHT:
                if (level.hover >= 0 && level.items[level.hover].submenu) choose(depth, level.hover);
                break;
            case VK_RETURN: case VK_SPACE:
                if (level.hover >= 0) choose(depth, level.hover);
                break;
            default: {
                // The first letter jumps to the next matching row; a single match is chosen.
                const auto typed = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(MapVirtualKeyW(static_cast<UINT>(message.wParam), MAPVK_VK_TO_CHAR))));
                if (!typed) break;
                std::vector<int> matches;
                for (int i = 0; i < static_cast<int>(level.items.size()); ++i) {
                    const auto& item = level.items[i];
                    if (!item.separator && !item.disabled && !item.text.empty() && std::towlower(item.text.front()) == typed) matches.push_back(i);
                }
                if (matches.size() == 1) choose(depth, matches.front());
                else if (!matches.empty()) {
                    const auto next = std::find_if(matches.begin(), matches.end(), [&](int i) { return i > level.hover; });
                    setHover(depth, next != matches.end() ? *next : matches.front());
                }
            }
            }
            return true;
        }
        case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR:
            return true;
        }
        return false;
    }

    void paint(HWND window, HDC target) {
        const auto found = std::find_if(levels_.begin(), levels_.end(), [&](const auto& l) { return l->window == window; });
        RECT client{};
        GetClientRect(window, &client);
        const HDC dc = buffer_.acquire(target, client.right, client.bottom);
        if (!dc) return;
        fill(dc, client, surface);
        if (found != levels_.end()) {
            const auto& level = **found;
            const auto oldFont = SelectObject(dc, font_);
            SetBkMode(dc, TRANSPARENT);
            Gdiplus::Graphics graphics(dc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const float scale = static_cast<float>(dpi_) / 96.f;
            for (int i = 0; i < static_cast<int>(level.items.size()); ++i) {
                const auto& item = level.items[i];
                if (item.separator) {
                    const int y = (item.rect.top + item.rect.bottom) / 2;
                    fill(dc, {px(12), y, item.rect.right - px(12), y + std::max(1, px(1))}, border);
                    continue;
                }
                const bool lit = i == level.hover || i == level.open;
                if (lit) roundRect(dc, {px(edge), item.rect.top + px(1), item.rect.right - px(edge), item.rect.bottom - px(1)}, px(6), raised, raised);
                if (item.checked) {
                    Gdiplus::Pen pen(color(accent), 1.8f * scale);
                    pen.SetStartCap(Gdiplus::LineCapRound);
                    pen.SetEndCap(Gdiplus::LineCapRound);
                    pen.SetLineJoin(Gdiplus::LineJoinRound);
                    const float cx = static_cast<float>(px(19)), cy = (item.rect.top + item.rect.bottom) / 2.f;
                    const Gdiplus::PointF tick[] = {{cx - 4.5f * scale, cy}, {cx - 1.2f * scale, cy + 3.5f * scale}, {cx + 5 * scale, cy - 3.5f * scale}};
                    graphics.DrawLines(&pen, tick, 3);
                }
                SetTextColor(dc, item.disabled ? muted : item.checked ? accent : ink);
                RECT text{px(36), item.rect.top, item.rect.right - px(item.submenu ? 30 : 16), item.rect.bottom};
                DrawTextW(dc, item.text.c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                if (item.submenu) {
                    Gdiplus::Pen pen(color(lit ? ink : muted), 1.5f * scale);
                    pen.SetStartCap(Gdiplus::LineCapRound);
                    pen.SetEndCap(Gdiplus::LineCapRound);
                    const float cx = static_cast<float>(item.rect.right - px(18)), cy = (item.rect.top + item.rect.bottom) / 2.f;
                    const Gdiplus::PointF chevron[] = {{cx - 2 * scale, cy - 4 * scale}, {cx + 2 * scale, cy}, {cx - 2 * scale, cy + 4 * scale}};
                    graphics.DrawLines(&pen, chevron, 3);
                }
            }
            SelectObject(dc, oldFont);
        }
        buffer_.present(target);
    }

    HWND owner_;
    UINT dpi_;
    HFONT font_ = nullptr;
    std::vector<std::unique_ptr<Level>> levels_;
    UINT result_ = 0;
    theme::BackBuffer buffer_;
    bool done_ = false;
};
} // namespace

UINT showPopupMenu(HWND owner, HMENU menu, POINT at, UINT dpi) {
    Session session(owner, dpi);
    return session.run(menu, at);
}

} // namespace refarter
