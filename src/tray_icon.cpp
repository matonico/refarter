#include "tray_icon.h"

#include <shellapi.h>

namespace refarter {

void TrayIcon::create(HWND window, UINT callback) {
    window_ = window;
    callback_ = callback;
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
}

bool TrayIcon::add(HICON icon, const std::wstring& tip) {
    if (present_) return true;
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window_;
    data.uID = id;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = callback_;
    data.hIcon = icon;
    tip_ = tip;
    wcscpy_s(data.szTip, tip_.c_str());
    if (!Shell_NotifyIconW(NIM_ADD, &data)) return false;
    data.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &data)) {
        Shell_NotifyIconW(NIM_DELETE, &data);
        return false;
    }
    present_ = true;
    return true;
}

void TrayIcon::remove() {
    if (!present_) return;
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window_;
    data.uID = id;
    Shell_NotifyIconW(NIM_DELETE, &data);
    present_ = false;
}

void TrayIcon::setTip(const std::wstring& tip) {
    if (!present_ || tip == tip_) return;
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window_;
    data.uID = id;
    data.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcscpy_s(data.szTip, tip.c_str());
    if (Shell_NotifyIconW(NIM_MODIFY, &data)) tip_ = tip;
}

void TrayIcon::setIcon(HICON icon) {
    if (!present_) return;
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window_;
    data.uID = id;
    data.uFlags = NIF_ICON;
    data.hIcon = icon;
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

} // namespace refarter
