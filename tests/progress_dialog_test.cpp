// Exercise the real modal loop with hidden windows. Focus/show APIs are replaced so this test cannot interrupt a desktop.
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <iostream>

namespace {
HWND testOwnerWindow = nullptr, testSiblingWindow = nullptr, testDialogWindow = nullptr, foreground = nullptr;
HWND frontAtFinish = nullptr; // What the user is in when the dialog closes.
const HWND game = reinterpret_cast<HWND>(static_cast<UINT_PTR>(1)); // Not a window of this process.
int ownerActivations = 0, fallbacks = 0;
bool cancel = false;
constexpr UINT finishTest = WM_APP + 0x400;
BOOL WINAPI testShowWindow(HWND, int) { return FALSE; }
HWND WINAPI testSetFocus(HWND) { return nullptr; }
BOOL WINAPI testVisible(HWND window) { return window == testOwnerWindow || IsWindowVisible(window); }
HWND WINAPI testForeground() { return foreground; }
BOOL WINAPI testActivate(HWND window) {
    if (window == testOwnerWindow) ++ownerActivations;
    foreground = window; return TRUE;
}
int WINAPI testMessageBox(HWND, LPCWSTR, LPCWSTR, UINT) { ++fallbacks; return IDCANCEL; }
}

#define ShowWindow testShowWindow
#define SetFocus testSetFocus
#define IsWindowVisible testVisible
#define GetForegroundWindow testForeground
#define SetForegroundWindow testActivate
#define MessageBoxW testMessageBox
#include "../src/message_box.cpp"
#undef ShowWindow
#undef SetFocus
#undef IsWindowVisible
#undef GetForegroundWindow
#undef SetForegroundWindow
#undef MessageBoxW

namespace {
LRESULT CALLBACK ownerProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == finishTest) {
        if (!testDialogWindow) // A dialog without a handle (choiceBox): the open window owned by the owner.
            EnumThreadWindows(GetCurrentThreadId(), [](HWND found, LPARAM) -> BOOL {
                if (found == testSiblingWindow || GetWindow(found, GW_OWNER) != testOwnerWindow) return TRUE;
                testDialogWindow = found;
                return FALSE;
            }, 0);
        foreground = frontAtFinish ? frontAtFinish : testDialogWindow;
        SendMessageW(testDialogWindow, cancel ? WM_CLOSE : refarter::progressDone, 0, 0);
        return 0;
    }
    return DefWindowProcW(window,message,wp,lp);
}
}

int main() {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput input; ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token,&input,nullptr);
    WNDCLASSW type{}; type.hInstance=GetModuleHandleW(nullptr); type.lpfnWndProc=ownerProc;
    type.lpszClassName=L"RefarterProgressTestOwner";
    RegisterClassW(&type);
    testOwnerWindow=CreateWindowW(type.lpszClassName,L"",WS_OVERLAPPED,0,0,640,480,nullptr,nullptr,type.hInstance,nullptr);
    // Another window of Refarter's (a flyout, a nested dialog): being in it is still being in Refarter.
    testSiblingWindow=CreateWindowW(type.lpszClassName,L"",WS_POPUP,0,0,10,10,testOwnerWindow,nullptr,type.hInstance,nullptr);
    if (!testOwnerWindow || !testSiblingWindow) return 2;
    int failures=0;
    const auto check=[&](bool ok,const char* text) { if (!ok) { std::cerr<<text<<'\n'; ++failures; } };
    for (HWND front : {HWND{}, testSiblingWindow, game}) for (bool cancelled : {false,true}) {
        frontAtFinish=front; cancel=cancelled; ownerActivations=0; foreground=testOwnerWindow;
        PostMessageW(testOwnerWindow,finishTest,0,0); // Dispatched by the actual progress dialog after its handle is available.
        const bool completed=refarter::progressBox(testOwnerWindow,L"test",L"test",L"cancel",&testDialogWindow);
        check(completed==!cancelled,"completion must distinguish the worker from cancellation");
        check(ownerActivations==(front==game ? 0 : 1),"only a user still in Refarter gets the owner back");
        check(IsWindowEnabled(testOwnerWindow) && !testDialogWindow,"dialog cleanup must re-enable the owner and clear its handle");
        if (front==game) check(foreground==game,"the game must keep focus");
    }
    // An ordinary question follows the same rule: closed while a game is in front, it leaves the game there.
    for (HWND front : {HWND{}, game}) {
        frontAtFinish=front; cancel=true; ownerActivations=0; foreground=testOwnerWindow; testDialogWindow=nullptr;
        PostMessageW(testOwnerWindow,finishTest,0,0);
        const int answer=refarter::choiceBox(testOwnerWindow,L"test",L"test",{L"yes",L"no"});
        check(answer==-1,"closing a question dismisses it");
        check(ownerActivations==(front==game ? 0 : 1),"a question only gives the owner back to a user still in Refarter");
        check(IsWindowEnabled(testOwnerWindow),"a question re-enables its owner");
        if (front==game) check(foreground==game,"the game must keep focus after a question");
    }
    testDialogWindow=nullptr;
    refarter::suppressDialogs(true);
    const bool suppressed=refarter::progressBox(testOwnerWindow,L"test",L"test",L"cancel",&testDialogWindow);
    refarter::suppressDialogs(false);
    check(!suppressed && !testDialogWindow,"suppressed progress grants no installation permission");
    check(!fallbacks,"the actual custom dialog must have run, not its fallback");
    DestroyWindow(testSiblingWindow); DestroyWindow(testOwnerWindow); Gdiplus::GdiplusShutdown(token); CoUninitialize();
    return failures ? 1 : 0;
}
