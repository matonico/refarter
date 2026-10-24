#include "graphics_capture.h"
#include "startup_frame.h"
#include "startup_retry.h"
#include "diagnostic_log.h"

#include <dwmapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace refarter::capture {

namespace wgc = winrt::Windows::Graphics::Capture;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::SizeInt32;

namespace {
constexpr auto format = DirectXPixelFormat::B8G8R8A8UIntNormalized;
// Two buffers: one being written by DWM while we convert the other.
constexpr int poolBuffers = 2;

// Both the installed SDK and the running OS must expose this optional API.
template<class Session>
bool throttleSource(const Session& session, std::int64_t interval) {
    using winrt::Windows::Foundation::TimeSpan;
    if constexpr (requires { session.MinUpdateInterval(TimeSpan{}); }) {
        if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
            try {
                session.MinUpdateInterval(TimeSpan{interval});
                return true;
            } catch (const winrt::hresult_error&) {}
        }
    }
    return false; // FramePacer still enforces the encoder frame rate.
}
} // namespace

GraphicsCapture::GraphicsCapture(CaptureSource source, ID3D11Device* device, std::int64_t minInterval,
                                 FrameHandler onFrame, ClosedHandler onClosed, HANDLE startupCancel, HANDLE process)
    : window_(source.window), onFrame_(std::move(onFrame)), onClosed_(std::move(onClosed)) {
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    if (source.window) {
        const DWORD expectedPid = process ? GetProcessId(process) : 0;
        const auto validate = [&]() -> HRESULT {
            if (startupCancel && WaitForSingleObject(startupCancel, 0) != WAIT_TIMEOUT)
                return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            DWORD pid = 0;
            if (!expectedPid || WaitForSingleObject(process, 0) != WAIT_TIMEOUT ||
                !GetWindowThreadProcessId(source.window, &pid) || pid != expectedPid)
                return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
            return S_OK;
        };
        const auto wait = [&](DWORD delay) -> HRESULT {
            HANDLE handles[2]{process, startupCancel};
            const DWORD result = WaitForMultipleObjects(startupCancel ? 2 : 1, handles, FALSE, delay);
            if (result == WAIT_TIMEOUT) return S_OK;
            if (result == WAIT_FAILED) return HRESULT_FROM_WIN32(GetLastError());
            return HRESULT_FROM_WIN32(result == WAIT_OBJECT_0 ? ERROR_PROCESS_ABORTED : ERROR_CANCELLED);
        };
        winrt::check_hresult(openCaptureWindow([&] {
            item_ = nullptr;
            return interop->CreateForWindow(source.window, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item_));
        }, validate, wait, [] { return GetTickCount64(); }, [](HRESULT result, unsigned number) {
            diagnosticLog().write(DiagnosticEvent::CaptureWindowAttempt, result, number);
        }));
        diagnosticLog().write(DiagnosticEvent::CaptureWindowReady);
    } else {
        winrt::check_hresult(interop->CreateForMonitor(source.monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item_)));
    }

    winrt::com_ptr<IDXGIDevice> dxgi;
    winrt::check_hresult(device->QueryInterface(IID_PPV_ARGS(dxgi.put())));
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    device_ = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

    poolSize_ = regionFor_ = item_.Size();
    initialSurfaceSize_ = {poolSize_.Width, poolSize_.Height};
    region_ = initialRegion_ = recordedRegion(poolSize_);
    pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(device_, format, poolBuffers, poolSize_);
    session_ = pool_.CreateCaptureSession(item_);

    using winrt::Windows::Foundation::Metadata::ApiInformation;
    constexpr auto sessionClass = L"Windows.Graphics.Capture.GraphicsCaptureSession";
    // Include Windows-rendered game cursors too; not every game draws one into its own frame.
    if (ApiInformation::IsPropertyPresent(sessionClass, L"IsCursorCaptureEnabled")) session_.IsCursorCaptureEnabled(true);
    if (ApiInformation::IsPropertyPresent(sessionClass, L"IsBorderRequired")) {
        try { session_.IsBorderRequired(false); } catch (const winrt::hresult_error&) {}
    }
    // Windows 11 24H2+: DWM skips copies we would discard anyway, which matters
    // when a game renders far above the recording frame rate.
    sourceThrottled_ = throttleSource(session_, minInterval);
    // Revocation alone does not wait for a queued callback. The shared gate
    // outlives this object and covers acquisition, resize and closed callbacks.
    frameRevoker_ = pool_.FrameArrived(winrt::auto_revoke, [state = callbacks_](const auto& pool, const auto&) {
        std::lock_guard lock(state->mutex);
        if (state->owner) {
            try { state->owner->frameArrived(pool); }
            catch (const winrt::hresult_error& e) { state->owner->closed(e.code()); }
        }
    });
    closedRevoker_ = item_.Closed(winrt::auto_revoke, [state = callbacks_](const auto&, const auto&) {
        std::lock_guard lock(state->mutex);
        if (state->owner) state->owner->closed();
    });
    {
        std::lock_guard lock(callbacks_->mutex);
        callbacks_->owner = this;
    }
}

GraphicsCapture::~GraphicsCapture() {
    {
        std::lock_guard lock(callbacks_->mutex);
        callbacks_->owner = nullptr;
    }
    {
        std::lock_guard lock(mutex_); // Waits for an in-flight frame callback.
        stopped_ = true;
    }
    frameRevoker_.revoke();
    closedRevoker_.revoke();
    try { session_.Close(); } catch (const winrt::hresult_error&) {}
    try { pool_.Close(); } catch (const winrt::hresult_error&) {}
}

void GraphicsCapture::start() {
    {
        std::lock_guard lock(mutex_);
        startupPaused_ = false;
        if (started_) return;
        started_ = true;
    }
    session_.StartCapture();
}

GraphicsCapture::StartupResult GraphicsCapture::awaitRestoredFrame(DWORD timeoutMs, HANDLE process, HANDLE cancel) {
    {
        std::lock_guard lock(mutex_);
        startupReady_.attach(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!startupReady_) winrt::throw_last_error();
        if (startupClosed_) return StartupResult::Closed;
        startupPaused_ = waitingForFrame_ = true;
        started_ = true;
    }
    session_.StartCapture();
    HANDLE events[3]{};
    DWORD count = 0, cancelIndex = MAXDWORD, processIndex = MAXDWORD;
    if (cancel) { cancelIndex = count; events[count++] = cancel; }
    if (process) { processIndex = count; events[count++] = process; }
    const DWORD readyIndex = count;
    events[count++] = startupReady_.get();
    const DWORD result = WaitForMultipleObjects(count, events, FALSE, timeoutMs);
    if (result == WAIT_TIMEOUT) return StartupResult::Timeout;
    if (result == WAIT_FAILED) winrt::throw_last_error();
    if (cancelIndex != MAXDWORD && result == WAIT_OBJECT_0 + cancelIndex) return StartupResult::Cancelled;
    if (processIndex != MAXDWORD && result == WAIT_OBJECT_0 + processIndex) return StartupResult::Closed;
    if (result != WAIT_OBJECT_0 + readyIndex) return StartupResult::Closed;
    std::lock_guard lock(mutex_);
    return startupClosed_ ? StartupResult::Closed : StartupResult::Ready;
}

void GraphicsCapture::closed(HRESULT cause) {
    {
        std::lock_guard lock(mutex_);
        startupClosed_ = true;
        if (startupReady_) SetEvent(startupReady_.get());
    }
    if (onClosed_) onClosed_(cause);
}

// Windowed games include a title bar and borders in the captured surface; keep
// only the client area. Monitors, and windows that cannot be measured, use the
// whole surface.
RECT GraphicsCapture::recordedRegion(SizeInt32 content) const {
    const RECT whole{0, 0, content.Width, content.Height};
    if (!window_) return whole;
    RECT frame{};
    RECT client{};
    POINT origin{};
    if (FAILED(DwmGetWindowAttribute(window_, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) ||
        !GetClientRect(window_, &client) || !ClientToScreen(window_, &origin)) {
        return whole;
    }
    const RECT inFrame{origin.x - frame.left, origin.y - frame.top,
                       origin.x - frame.left + client.right, origin.y - frame.top + client.bottom};
    RECT region{};
    if (!IntersectRect(&region, &inFrame, &whole) || region.right - region.left < 2 || region.bottom - region.top < 2) return whole;
    return region;
}

void GraphicsCapture::frameArrived(const wgc::Direct3D11CaptureFramePool& pool) {
    auto frame = pool.TryGetNextFrame();
    if (!frame) return;
    const auto content = frame.ContentSize();
    const bool visible = content.Width > 0 && content.Height > 0;
    {
        std::lock_guard lock(mutex_);
        if (stopped_) return;
        if (visible) {
            // Measure only when the size changes, not on every frame.
            if (content.Width != regionFor_.Width || content.Height != regionFor_.Height) {
                regionFor_ = content;
                region_ = recordedRegion(content);
            }
            winrt::com_ptr<ID3D11Texture2D> texture;
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(texture.put())));
            if (startupPaused_) {
                if (waitingForFrame_) {
                    RECT client{};
                    GetClientRect(window_, &client);
                    D3D11_TEXTURE2D_DESC description{};
                    texture->GetDesc(&description);
                    if (usableStartupFrame(IsIconic(window_) != FALSE, {client.right, client.bottom},
                            {content.Width, content.Height}, {static_cast<int>(description.Width), static_cast<int>(description.Height)})) {
                        region_ = initialRegion_ = recordedRegion(content);
                        waitingForFrame_ = false;
                        SetEvent(startupReady_.get());
                    }
                }
            } else onFrame_({texture.get(), region_, frame.SystemRelativeTime().count()});
        }
    }
    frame.Close(); // Return the buffer to DWM before any resize.
    if (visible && (content.Width != poolSize_.Width || content.Height != poolSize_.Height)) {
        poolSize_ = content;
        pool.Recreate(device_, format, poolBuffers, poolSize_);
    }
}

} // namespace refarter::capture
