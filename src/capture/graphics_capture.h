#pragma once

#include "video_settings.h"

#include <windows.h>
#include <d3d11.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <cstdint>
#include <functional>
#include <mutex>
#include <memory>

namespace refarter::capture {

// What the user explicitly chose to record: one window, or one whole monitor.
// Exactly one is set. A window source never switches to the monitor.
struct CaptureSource {
    HWND window = nullptr;
    HMONITOR monitor = nullptr;
};

struct CapturedFrame {
    ID3D11Texture2D* texture; // Valid only during the callback.
    RECT region;              // The area to record within the texture.
    std::int64_t time;        // QPC-based system-relative time, 100 ns units.
};

// Windows Graphics Capture of the chosen source. Frames stay on the GPU and
// arrive on a free-threaded pool, so no UI thread or dispatcher is involved.
class GraphicsCapture {
public:
    using FrameHandler = std::function<void(const CapturedFrame&)>;
    // Runs when capture ends. `cause` is S_OK for the source closing, else the error that ended frame handling
    // (a device loss arrives here too: the owner checks the device).
    using ClosedHandler = std::function<void(HRESULT cause)>;

    // minInterval (100 ns) throttles delivery at the source where Windows supports it.
    GraphicsCapture(CaptureSource source, ID3D11Device* device, std::int64_t minInterval,
                    FrameHandler onFrame, ClosedHandler onClosed,
                    HANDLE startupCancel = nullptr, HANDLE process = nullptr);
    ~GraphicsCapture();
    GraphicsCapture(const GraphicsCapture&) = delete;
    GraphicsCapture& operator=(const GraphicsCapture&) = delete;

    void start();
    enum class StartupResult { Ready, Timeout, Closed, Cancelled };
    // Start only this source and block the caller until a restored usable frame.
    // Frame callbacks stay paused until start() releases them after encoder setup.
    StartupResult awaitRestoredFrame(DWORD timeoutMs, HANDLE process = nullptr, HANDLE cancel = nullptr);
    // Initial recorded size, updated by awaitRestoredFrame before encoder setup.
    Size size() const { return {initialRegion_.right - initialRegion_.left, initialRegion_.bottom - initialRegion_.top}; }
    bool sourceThrottled() const { return sourceThrottled_; }
    Size initialSurfaceSize() const { return initialSurfaceSize_; }

private:
    void frameArrived(const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& pool);
    RECT recordedRegion(winrt::Windows::Graphics::SizeInt32 content) const;
    void closed(HRESULT cause = S_OK); // The source is gone: ends a startup wait and reports it. Called under the callback gate.

    HWND window_;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker frameRevoker_;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem::Closed_revoker closedRevoker_;
    winrt::Windows::Graphics::SizeInt32 poolSize_{};
    Size initialSurfaceSize_{};
    winrt::handle startupReady_;
    bool started_ = false, startupPaused_ = false, waitingForFrame_ = false, startupClosed_ = false;
    winrt::Windows::Graphics::SizeInt32 regionFor_{};
    RECT region_{};
    RECT initialRegion_{};
    FrameHandler onFrame_;
    ClosedHandler onClosed_;
    std::mutex mutex_;
    bool stopped_ = false;
    bool sourceThrottled_ = false;
    struct CallbackState {
        std::mutex mutex;
        GraphicsCapture* owner = nullptr;
    };
    std::shared_ptr<CallbackState> callbacks_ = std::make_shared<CallbackState>();
};

} // namespace refarter::capture
