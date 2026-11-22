#pragma once

#include <windows.h>
#include <d3d11.h>
#include <winrt/base.h>
#include <string>

namespace refarter::capture {

struct Gpu {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    LUID luid{};
    std::wstring name;
};

inline bool sameLuid(LUID a, LUID b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

// Creates a multithread-protected D3D11 device on the adapter that drives the
// monitor. Captured frames already live there, so nothing crosses
// adapters on hybrid-graphics machines.
Gpu createGpuForMonitor(HMONITOR monitor);
inline Gpu createGpuForWindow(HWND window) { return createGpuForMonitor(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)); }

} // namespace refarter::capture
