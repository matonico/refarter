#pragma once

#include <windows.h>
#include <initguid.h> // Defines the accessibility GUIDs and property ids declared in oleacc.h (selectany: message_box.cpp does the same).
#include <oleacc.h>
#include <initializer_list>
#include <utility>

namespace refarter {

// Gives controls that carry no text of their own (a number field shows "180", a percentage "100%") the name a screen
// reader should say, through the accessibility property store that MSAA and UI Automation both read. The names live
// as long as the window handles, which for the main window's controls is the whole process. Needs COM on the thread.
inline void nameControls(std::initializer_list<std::pair<HWND, const wchar_t*>> names) {
    IAccPropServices* services = nullptr;
    if (FAILED(CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&services))) || !services) return;
    for (const auto& [window, name] : names)
        if (window) services->SetHwndPropStr(window, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, PROPID_ACC_NAME, name);
    services->Release();
}

} // namespace refarter
