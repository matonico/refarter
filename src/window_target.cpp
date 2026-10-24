#include "window_target.h"
#include "source_names.h"

#include <dwmapi.h>
#include <filesystem>
#include <memory>

namespace refarter {

std::vector<HMONITOR> displayMonitors() {
    std::vector<HMONITOR> displays;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
        reinterpret_cast<std::vector<HMONITOR>*>(param)->push_back(monitor);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&displays));
    return displays;
}

std::wstring monitorDevice(HMONITOR monitor) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    return GetMonitorInfoW(monitor, &info) ? info.szDevice : L"";
}

std::optional<RecordingTarget> windowTarget(HWND window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    DWORD cloaked = 0;
    DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (!window || !IsWindowVisible(window) || cloaked || window == GetShellWindow() ||
        pid == GetCurrentProcessId() || GetWindow(window, GW_OWNER)) return std::nullopt;
    wchar_t title[256]{};
    if (!GetWindowTextW(window, title, 256)) return std::nullopt;
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) return std::nullopt;
    std::shared_ptr<void> handle(process, [](void* value) { CloseHandle(value); });
    wchar_t executable[32768]{};
    DWORD size = static_cast<DWORD>(std::size(executable));
    if (!QueryFullProcessImageNameW(process, 0, executable, &size)) return std::nullopt;
    const auto processName = std::filesystem::path(executable).filename().wstring();
    // Also exclude another installed/workspace instance of the app.
    if (_wcsicmp(processName.c_str(), L"Refarter.exe") == 0) return std::nullopt;
    RecordingTarget target{{window, nullptr}, pid, std::move(handle), processName, title};
    target.displayName = friendlySourceName(target.title, target.name);
    target.path = executable;
    return target;
}

} // namespace refarter
