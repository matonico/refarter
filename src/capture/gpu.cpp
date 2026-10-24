#include "gpu.h"

#include <dxgi1_2.h>
#include <iterator>

namespace refarter::capture {

namespace {
winrt::com_ptr<IDXGIAdapter1> adapterForMonitor(IDXGIFactory1* factory, HMONITOR monitor) {
    winrt::com_ptr<IDXGIAdapter1> fallback;
    for (UINT a = 0;; ++a) {
        winrt::com_ptr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, adapter.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        for (UINT o = 0;; ++o) {
            winrt::com_ptr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, output.put()) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC outputDesc{};
            output->GetDesc(&outputDesc);
            if (outputDesc.Monitor == monitor) return adapter;
            if (!fallback) fallback = adapter;
        }
    }
    return fallback;
}
} // namespace

Gpu createGpuForMonitor(HMONITOR monitor) {
    winrt::com_ptr<IDXGIFactory1> factory;
    winrt::check_hresult(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()));
    auto adapter = adapterForMonitor(factory.get(), monitor);
    if (!adapter) winrt::throw_hresult(DXGI_ERROR_NOT_FOUND);

    Gpu gpu;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    // VIDEO_SUPPORT enables the fixed-function video processor used for
    // colour conversion and scaling; BGRA matches the capture format.
    winrt::check_hresult(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, gpu.device.put(), nullptr, gpu.context.put()));
    // Capture callbacks and the encoder's internal threads share this device.
    gpu.device.as<ID3D10Multithread>()->SetMultithreadProtected(TRUE);

    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    gpu.luid = desc.AdapterLuid;
    gpu.name = desc.Description;
    return gpu;
}

} // namespace refarter::capture
