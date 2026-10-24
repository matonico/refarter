#pragma once

#include <windows.h>
#include <memory>

namespace refarter::capture {

// The process a game-only audio track is bound to: the game process the recorder validated when recording started,
// identified by a handle held for as long as any copy of this object lives (copies share it). Window handles are
// recycled by Windows, so the window is never asked again; only this process counts, and only while it still runs.
// Copies stay valid after the recorder has stopped its producers, as the saver's copy does.
class BoundProcess {
public:
    BoundProcess() = default;

    // Copies the recorder's validated process identity. Failure or an exited process must
    // never cause a new window-owner lookup: that window may now belong to another game.
    static BoundProcess ofProcess(HANDLE process) {
        if (!process) return {};
        HANDLE held = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &held,
                SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0)) return {};
        return adopt(GetProcessId(held), held);
    }

    // Binds the window's owner now (the recorder uses ofProcess; this is for tests and tools). Empty when the window
    // is gone, has no owner, cannot be opened, or changes owner
    // while it is being opened (the handle might then belong to a recycled ID).
    static BoundProcess ofWindow(HWND window) {
        DWORD pid = 0;
        if (!window || !IsWindow(window) || !GetWindowThreadProcessId(window, &pid) || !pid) return {};
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return {};
        DWORD again = 0;
        if (!IsWindow(window) || !GetWindowThreadProcessId(window, &again) || again != pid) {
            CloseHandle(process);
            return {};
        }
        return adopt(pid, process);
    }
    // Takes over `process` (closed with the last copy), which must be a SYNCHRONIZE handle to `pid`.
    static BoundProcess adopt(DWORD pid, HANDLE process) {
        BoundProcess bound;
        if (!pid || !process) {
            if (process) CloseHandle(process);
            return bound;
        }
        bound.pid_ = pid;
        bound.handle_ = std::shared_ptr<void>(process, [](void* h) { CloseHandle(static_cast<HANDLE>(h)); });
        return bound;
    }

    // The bound process ID while that process runs; 0 once it has exited, or when nothing is bound. Any thread.
    DWORD running() const {
        return handle_ && WaitForSingleObject(static_cast<HANDLE>(handle_.get()), 0) == WAIT_TIMEOUT ? pid_ : 0;
    }

private:
    DWORD pid_ = 0;
    std::shared_ptr<void> handle_;
};

} // namespace refarter::capture
