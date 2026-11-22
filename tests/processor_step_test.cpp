#include "capture/processor_step.h"
#include <dxgi.h>
#include <iostream>
#include <vector>

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* text) { if (!ok) { ++failures; std::cerr << text << '\n'; } };
    // Inject each driver's failure at the same boundary used by FrameConverter. Later steps must not run,
    // the classifier sees the original HRESULT exactly once, and the caller still receives the error.
    for (HRESULT code : {E_INVALIDARG, E_NOTIMPL, DXGI_ERROR_DEVICE_REMOVED}) for (int failed = 0; failed != 4; ++failed) {
        std::vector<int> steps;
        int classified = 0;
        bool threw = false;
        const auto step = [&](int n) { steps.push_back(n); if (failed == n) winrt::throw_hresult(code); };
        try {
            const bool result = refarter::capture::processVideoFrame(
                [&] { step(1); return 11; }, [&] { step(2); return 22; },
                [&](int input, int output) { check(input == 11 && output == 22, "wrong views"); step(3); return true; },
                [&](HRESULT error) { ++classified; check(error == code, "lost driver HRESULT"); });
            check(result && failed == 0, "a failed frame was reported successful");
        } catch (const winrt::hresult_error& error) { threw = true; check(error.code() == code, "changed caller error"); }
        check(threw == (failed != 0) && classified == (failed ? 1 : 0), "failure classification count");
        check(steps.size() == static_cast<size_t>(failed ? failed : 3), "ran a later stage after failure");
    }
    // Real driver calls go through check_hresult, which throws E_OUTOFMEMORY as std::bad_alloc: it must reach the
    // recorder unclassified.
    bool allocated = false, classified = false;
    try {
        refarter::capture::processVideoFrame([]() -> int { winrt::check_hresult(E_OUTOFMEMORY); return 0; },
            [] { return 0; }, [](int, int) { return true; }, [&](HRESULT) { classified = true; });
    } catch (const std::bad_alloc&) { allocated = true; }
    check(allocated && !classified, "allocation failure was mistaken for a format refusal");
    for (const bool healthy : {false, true}) for (int failed : {2, 3}) {
        bool shaders = true;
        int attempts = 0, retries = 0, blits = 0;
        const auto frame = [&] {
            ++attempts;
            return refarter::capture::processVideoFrame([] { return 1; }, [&] {
                if (shaders && failed == 2) winrt::throw_hresult(E_INVALIDARG);
                return 2;
            }, [&](int, int) {
                if (shaders && failed == 3) winrt::throw_hresult(E_INVALIDARG);
                ++blits; return true;
            }, [](HRESULT) {});
        };
        bool threw = false;
        try {
            check(refarter::capture::scaledVideoFrame(shaders, frame, [&] { return healthy; }, [&](HRESULT) {
                ++retries; shaders = false; return frame();
            }), "legacy retry did not produce a frame");
        } catch (const winrt::hresult_error&) { threw = true; }
        check(threw == !healthy && retries == (healthy ? 1 : 0) && attempts == (healthy ? 2 : 1), "wrong fallback count");
        if (healthy) {
            check(refarter::capture::scaledVideoFrame(shaders, frame, [] { return true; }, [&](HRESULT) {
                ++retries; return false;
            }), "later legacy frame failed");
            check(retries == 1 && blits == 2, "later frame re-entered shaders");
        }
    }
    for (const bool downscaled : {false, true}) {
        bool retried = false, threw = false;
        try {
            // As real driver calls fail: check_hresult throws E_OUTOFMEMORY as std::bad_alloc.
            refarter::capture::scaledVideoFrame(downscaled, []() -> bool { winrt::check_hresult(E_OUTOFMEMORY); return true; },
                [] { return true; }, [&](HRESULT) { retried = true; return true; });
        } catch (const std::bad_alloc&) { threw = true; }
        check(threw && !retried, "processor OOM was retried");
    }
    bool nativeRetried = false, nativeThrew = false;
    try {
        refarter::capture::scaledVideoFrame(false, []() -> bool { winrt::throw_hresult(E_INVALIDARG); },
            [] { return true; }, [&](HRESULT) { nativeRetried = true; return true; });
    } catch (const winrt::hresult_error&) { nativeThrew = true; }
    check(nativeThrew && !nativeRetried, "a native-frame refusal entered the shader fallback");

    // The refusal decision: only a healthy-device P010 failure that the NV12 repeat survives.
    using refarter::capture::tenBitRefusal;
    int repeats = 0;
    const auto healthy = [] { return false; };
    const auto removed = [] { return true; };
    const auto nv12Works = [&] { ++repeats; };
    const auto nv12Fails = [&] { ++repeats; winrt::throw_hresult(E_INVALIDARG); };
    const auto nv12OutOfMemory = [&] { ++repeats; winrt::check_hresult(E_OUTOFMEMORY); };
    const auto nv12Throws = [&] { ++repeats; throw 1; };
    check(tenBitRefusal(true, E_INVALIDARG, healthy, nv12Works) && repeats == 1, "NV12 proof did not refuse 10-bit");
    check(!tenBitRefusal(false, E_INVALIDARG, healthy, nv12Works), "an NV12 failure refused 10-bit");
    check(!tenBitRefusal(true, E_INVALIDARG, removed, nv12Works), "a removed device refused 10-bit");
    check(!tenBitRefusal(true, E_OUTOFMEMORY, healthy, nv12Works), "exhausted memory refused 10-bit");
    check(repeats == 1, "repeated with NV12 when no refusal was possible");
    check(!tenBitRefusal(true, E_INVALIDARG, healthy, nv12Fails), "refused 10-bit although NV12 failed too");
    check(!tenBitRefusal(true, E_INVALIDARG, healthy, nv12OutOfMemory), "the NV12 repeat's bad_alloc escaped or refused");
    check(!tenBitRefusal(true, E_INVALIDARG, healthy, nv12Throws), "the NV12 repeat's exception escaped or refused");

    // The repeat runs inside the failure handler; its own error must not replace the original one.
    HRESULT reached = S_OK;
    try {
        refarter::capture::processVideoFrame([]() -> int { winrt::throw_hresult(E_INVALIDARG); },
            [] { return 0; }, [](int, int) { return true; },
            [&](HRESULT error) { tenBitRefusal(true, error, healthy, nv12OutOfMemory); });
    } catch (const winrt::hresult_error& error) { reached = error.code(); }
    check(reached == E_INVALIDARG, "the NV12 repeat replaced the original error");
    return failures ? 1 : 0;
}
