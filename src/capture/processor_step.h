#pragma once

#include <windows.h>
#include <winrt/base.h>

namespace refarter::capture {

// One processor frame: both view factories and the blit share the same failure boundary.
// Templates keep the factories inline; no allocation or indirect call is added per frame.
// winrt turns E_OUTOFMEMORY into std::bad_alloc, which passes through unclassified.
template <typename Input, typename Output, typename Blit, typename Failure>
decltype(auto) processVideoFrame(Input input, Output output, Blit blit, Failure failure) {
    try {
        auto in = input();
        auto out = output();
        return blit(in, out);
    } catch (const winrt::hresult_error& error) {
        failure(error.code());
        throw;
    }
}

// Retry a refused shader-dependent processor frame through the original scaler once.
// Device loss stays an error, and so does allocation: winrt throws E_OUTOFMEMORY as std::bad_alloc, which passes
// through. The caller disables shaders before retrying.
template <typename Frame, typename Healthy, typename Legacy>
bool scaledVideoFrame(bool downscaled, Frame frame, Healthy healthy, Legacy legacy) {
    try {
        return frame();
    } catch (const winrt::hresult_error& error) {
        if (downscaled && healthy()) return legacy(error.code());
        throw;
    }
}

// Whether a failed P010 step is a refusal of 10-bit: only when the device is healthy and `repeatWithNv12`, the same
// step with NV12 output, succeeds. Any exception from the repeat means no proof; it never replaces the original error.
template <typename DeviceRemoved, typename Repeat>
bool tenBitRefusal(bool p010, HRESULT error, DeviceRemoved deviceRemoved, Repeat repeatWithNv12) noexcept {
    if (!p010 || error == E_OUTOFMEMORY || deviceRemoved()) return false;
    try {
        repeatWithNv12();
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace refarter::capture
