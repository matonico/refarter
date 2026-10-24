#include "app.h"
#include "crash_dumps.h"
#include "local_file.h"
#include "startup.h"
#include "version.h"
#include <objidl.h>
#include <gdiplus.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    const bool startup = refarter::startupLaunch(GetCommandLineW());
    const bool quit = refarter::quitLaunch(GetCommandLineW());
    const HANDLE instanceLock = CreateMutexW(nullptr, FALSE, L"Local\\RefarterDesktopShell");
    if (!instanceLock) return 1;
    const auto closeLock = [](void* handle) { CloseHandle(handle); };
    std::unique_ptr<void, decltype(closeLock)> lifetime(instanceLock, closeLock);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Sign-in must not open an already-running app; manual launches restore it.
        if (!startup || quit) {
            if (const HWND existing = FindWindowW(L"RefarterDesktopShell", nullptr)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(existing, &pid);
                if (!quit) AllowSetForegroundWindow(pid);
                PostMessageW(existing, RegisterWindowMessageW(quit ? refarter::quitMessageName : refarter::restoreMessageName), 0, 0);
            }
        }
        return 0;
    }
    if (quit) return 0;
    // Before anything that could crash; the name carries the version whose symbols read the dump.
    refarter::installCrashDumps(refarter::localAppDataFile(L"crashes"), L"refarter " REFARTER_VERSION_FULL_STRING);
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
        MessageBoxW(nullptr, L"the interface could not start.", L"refarter", MB_ICONERROR);
        return 1;
    }
    int result;
    {
        refarter::App app;
        result = app.run(instance, showCommand, startup);
    }
    Gdiplus::GdiplusShutdown(token);
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
