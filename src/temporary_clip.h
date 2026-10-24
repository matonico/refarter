#pragma once
#include <windows.h>
#include <filesystem>

namespace refarter {
// Identify an intermediate before exporting it. Cleanup must never remove a replacement or edited file.
struct TemporaryClipIdentity {
    BY_HANDLE_FILE_INFORMATION info{};
    bool valid = false;
};

inline TemporaryClipIdentity temporaryClipIdentity(const std::filesystem::path& path) {
    TemporaryClipIdentity identity;
    const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return identity;
    identity.valid = GetFileInformationByHandle(file, &identity.info) &&
        !(identity.info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    CloseHandle(file);
    return identity;
}

// Delete by the verified handle, not by re-opening its path after checking it. Only for our own intermediate,
// after a separate compressed output has been published; never for the user's original or an already-fit result.
inline bool discardTemporaryClip(const std::filesystem::path& path, const TemporaryClipIdentity& expected) {
    if (!expected.valid) return false;
    const HANDLE file = CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION actual{};
    const auto& old = expected.info;
    const bool same = GetFileInformationByHandle(file, &actual) &&
        !(actual.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        actual.dwVolumeSerialNumber == old.dwVolumeSerialNumber &&
        actual.nFileIndexHigh == old.nFileIndexHigh && actual.nFileIndexLow == old.nFileIndexLow &&
        actual.nFileSizeHigh == old.nFileSizeHigh && actual.nFileSizeLow == old.nFileSizeLow &&
        CompareFileTime(&actual.ftLastWriteTime, &old.ftLastWriteTime) == 0;
    FILE_DISPOSITION_INFO disposition{TRUE};
    const bool removed = same && SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
    CloseHandle(file);
    return removed;
}

inline bool discardExportIntermediate(const std::filesystem::path& path, const TemporaryClipIdentity& expected,
                                      const std::filesystem::path& output) {
    if (path == output || output.empty()) return false;
    // Keep the published output present and immutable until the intermediate has been removed.
    const HANDLE finished = CreateFileW(output.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (finished == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ready = GetFileInformationByHandle(finished, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        (info.nFileSizeHigh || info.nFileSizeLow);
    const bool removed = ready && discardTemporaryClip(path, expected);
    CloseHandle(finished);
    return removed;
}
} // namespace refarter
