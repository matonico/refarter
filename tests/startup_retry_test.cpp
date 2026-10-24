#include "capture/startup_retry.h"
#include <iostream>
#include <vector>

int main() {
    using refarter::capture::openCaptureWindow;
    int failures = 0;
    const auto check = [&](bool okay, const char* text) { if (!okay) { std::cerr << text << '\n'; ++failures; } };
    unsigned attempts = 0, reports = 0, waits = 0;
    std::uint64_t now = 0;
    HRESULT identity = S_OK, wake = S_OK;
    std::vector<HRESULT> results;
    const auto run = [&] {
        attempts = reports = waits = 0;
        now = 0;
        return openCaptureWindow([&] { return results[std::min<std::size_t>(attempts++, results.size() - 1)]; },
            [&] { return identity; }, [&](DWORD delay) { ++waits; now += delay; return wake; },
            [&] { return now; }, [&](HRESULT, unsigned n) { reports = n; });
    };
    results = {S_OK};
    check(run() == S_OK && attempts == 1 && waits == 0, "ready source starts immediately");
    results = {E_FAIL, E_INVALIDARG, S_OK};
    check(run() == S_OK && attempts == 3 && waits == 2 && reports == 3 && now == 400,
        "temporary item creation failures recover on the same request");
    results = {E_ACCESSDENIED};
    check(run() == E_ACCESSDENIED && attempts == 1 && !waits, "permission denial never retries");
    results = {E_OUTOFMEMORY};
    check(run() == E_OUTOFMEMORY && attempts == 1 && !waits, "resource failure never retries");
    results = {E_FAIL};
    check(run() == E_FAIL && attempts == 16 && waits == 15 && now == 3000,
        "persistent failure stops at three seconds");
    wake = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    check(run() == wake && attempts == 1 && waits == 1, "stop cancels during backoff");
    wake = HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
    check(run() == wake && attempts == 1, "process exit cancels during backoff");
    wake = S_OK;
    identity = HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    check(run() == identity && attempts == 0, "changed target cannot be captured");
    identity = S_OK;
    attempts = 0;
    unsigned checked = 0;
    const auto changedBetweenAttempts = openCaptureWindow([&] { ++attempts; return results.front(); }, [&] {
        return ++checked == 1 ? identity : HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    }, [&](DWORD delay) { now += delay; return wake; }, [&] { return now; }, [](HRESULT, unsigned) {});
    check(changedBetweenAttempts == HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE) && attempts == 1,
        "a changed target is rejected before the next attempt");
    unsigned validations = 0;
    HRESULT okay = S_OK, invalid = HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE), failed = E_FAIL;
    const auto changedDuringOpen = openCaptureWindow([&] { return okay; }, [&] {
        return ++validations == 1 ? okay : invalid;
    }, [&](DWORD) { return okay; }, [&] { return now; }, [](HRESULT, unsigned) {});
    check(changedDuringOpen == HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE), "identity is rechecked after successful creation");
    // A stalled clock must not allow an endless retry loop either.
    attempts = 0;
    const auto frozenClock = openCaptureWindow([&] { ++attempts; return failed; }, [&] { return okay; },
        [&](DWORD) { return okay; }, [&] { return now; }, [](HRESULT, unsigned) {});
    check(frozenClock == E_FAIL && attempts == 16, "attempt limit independently bounds retries");
    return failures ? 1 : 0;
}
