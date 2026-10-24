#pragma once
#include <windows.h>
#include <functional>

namespace refarter {
// Recheck completion and deadline between messages; never drain a busy queue indefinitely.
inline bool waitForMessages(const std::function<bool()>& done, DWORD timeoutMs,
                            bool (*ignore)(UINT)) {
    const ULONGLONG end = GetTickCount64() + timeoutMs;
    for (;;) {
        if (done()) return true;
        const auto now = GetTickCount64();
        if (now >= end) return false;
        MSG message{};
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                return done();
            }
            if (!ignore(message.message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        } else {
            // PeekMessage may itself dispatch sent messages, completing the operation.
            if (done()) return true;
            const auto afterPeek = GetTickCount64();
            if (afterPeek >= end) return false;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(end - afterPeek), QS_ALLINPUT);
        }
    }
}
} // namespace refarter
