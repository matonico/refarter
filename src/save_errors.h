#pragma once

#include <string>
#include <system_error>

// Wording for a failed clip write when the disk is the cause. Pure, so it can be tested without a full disk.
namespace refarter {

inline constexpr unsigned long long lowDiskBytes = 16ull * 1024 * 1024; // Below this free space a write failure is a full disk.

// Win32 ERROR_DISK_FULL (112), ERROR_HANDLE_DISK_FULL (39) or ERROR_DISK_QUOTA_EXCEEDED (1295), plain or as an HRESULT,
// or the storage error STG_E_MEDIUMFULL (0x80030070) that structured storage and some Media Foundation sinks report.
inline bool isDiskFullCode(long code) {
    const unsigned long raw = static_cast<unsigned long>(code);
    if (raw == 0x80030070ul) return true; // STG_E_MEDIUMFULL
    const unsigned long win32 = (raw & 0xFFFF0000ul) == 0x80070000ul ? (raw & 0xFFFFul) : raw;
    return win32 == 112 || win32 == 39 || win32 == 1295;
}

inline bool isDiskFull(const std::error_code& error) {
    return error == std::errc::no_space_on_device || (error.category() == std::system_category() && isDiskFullCode(error.value()));
}

// A write failed with `code` (an HRESULT or Win32 error; 0 when unknown). `freeBytes` is the free space on the target
// volume after the failure, or ~0 when it could not be read. Media Foundation does not always report a full disk as one.
inline bool failedForDiskSpace(long code, unsigned long long freeBytes) {
    return isDiskFullCode(code) || freeBytes < lowDiskBytes;
}

// What the clip writer reports; it cannot know whether the replay survives the failed save.
inline std::wstring diskFullMessage() { return L"disk full · free some space"; }

// Preflight failures happen before the clip writer: retain their actual OS error too.
inline std::wstring folderFailureText(const std::error_code& error, const std::wstring& fallback) {
    return isDiskFull(error) ? diskFullMessage() : fallback;
}

// The words shown for a failed save. A disk-full failure says the replay is kept (and that saving again can work) only when it is: not when the save
// was the one a stop, a quit, a shutdown or auto-capture's clear was finishing, because the stop then clears it.
inline std::wstring failedSaveText(const std::wstring& message, bool replayKept) {
    return replayKept && message == diskFullMessage() ? L"disk full · replay kept" : message;
}

} // namespace refarter
