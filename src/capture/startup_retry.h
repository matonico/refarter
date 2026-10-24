#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>

namespace refarter::capture {
inline bool retryableWindowCreation(HRESULT result) { return result == E_FAIL || result == E_INVALIDARG; }
// Retry only WGC item creation: E_FAIL and E_INVALIDARG can describe a window
// whose capture surface is not ready yet. They are not proof of a transient
// failure, so the budget is finite. Never retry permissions or encoder errors.
template<class Attempt, class Validate, class Wait, class Clock, class Report>
HRESULT openCaptureWindow(Attempt attempt, Validate validate, Wait wait, Clock clock, Report report) {
    constexpr std::uint64_t budgetMs = 3'000;
    const auto began = clock();
    for (unsigned number = 1; ; ++number) {
        if (const HRESULT identity = validate(); FAILED(identity)) return identity;
        const HRESULT result = attempt();
        report(result, number);
        if (SUCCEEDED(result)) return validate();
        if (!retryableWindowCreation(result)) return result;
        const auto elapsed = clock() - began;
        if (elapsed >= budgetMs || number >= 16) return result;
        const auto delay = static_cast<DWORD>(std::min<std::uint64_t>(200, budgetMs - elapsed));
        if (const HRESULT wake = wait(delay); FAILED(wake)) return wake;
    }
}
} // namespace refarter::capture
