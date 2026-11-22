#pragma once
#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <vector>
#include "player/export_plan.h"

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

// Opens `path` for deleting or renaming, only when it is still the file `expected` describes (same file, size and
// write time). INVALID_HANDLE_VALUE otherwise.
inline HANDLE openTemporaryClip(const std::filesystem::path& path, const TemporaryClipIdentity& expected) {
    if (!expected.valid) return INVALID_HANDLE_VALUE;
    const HANDLE file = CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return file;
    BY_HANDLE_FILE_INFORMATION actual{};
    const auto& old = expected.info;
    const bool same = GetFileInformationByHandle(file, &actual) &&
        !(actual.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        actual.dwVolumeSerialNumber == old.dwVolumeSerialNumber &&
        actual.nFileIndexHigh == old.nFileIndexHigh && actual.nFileIndexLow == old.nFileIndexLow &&
        actual.nFileSizeHigh == old.nFileSizeHigh && actual.nFileSizeLow == old.nFileSizeLow &&
        CompareFileTime(&actual.ftLastWriteTime, &old.ftLastWriteTime) == 0;
    if (same) return file;
    CloseHandle(file);
    SetLastError(ERROR_FILE_NOT_FOUND); // Not the file we made: never retried as a sharing violation.
    return INVALID_HANDLE_VALUE;
}

// Delete by the verified handle, not by re-opening its path after checking it. Only for our own intermediate,
// after a separate compressed output has been published; never for the user's original or an already-fit result.
inline bool discardTemporaryClip(const std::filesystem::path& path, const TemporaryClipIdentity& expected) {
    const HANDLE file = openTemporaryClip(path, expected);
    if (file == INVALID_HANDLE_VALUE) return false;
    FILE_DISPOSITION_INFO disposition{TRUE};
    const bool removed = SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
    CloseHandle(file);
    return removed;
}

// A trim that already fits takes the name a compressed copy would get ("20mb_<name>", then " (2)"...), so it is
// tagged as compressed. Renamed by the verified handle and never over an existing file. The library or a virus
// scanner may still be reading the new trim, so a sharing violation is retried for about a second (worker thread
// only). Empty when it could not be renamed.
inline std::filesystem::path tagTemporaryClip(const std::filesystem::path& path, const TemporaryClipIdentity& expected,
                                              int attempts = 20) {
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (attempt) Sleep(50);
        file = openTemporaryClip(path, expected);
        if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION) break;
    }
    if (file == INVALID_HANDLE_VALUE) return {};
    std::filesystem::path tagged;
    for (int copy = 1; copy <= 10'000 && tagged.empty(); ++copy) {
        const auto candidate = player::exportPath(path, copy);
        const auto name = candidate.wstring();
        std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        rename->ReplaceIfExists = FALSE;
        rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
        std::copy(name.begin(), name.end(), rename->FileName);
        if (SetFileInformationByHandle(file, FileRenameInfo, rename, static_cast<DWORD>(buffer.size()))) tagged = candidate;
        else if (const auto failure = GetLastError(); failure != ERROR_ALREADY_EXISTS && failure != ERROR_FILE_EXISTS) break;
    }
    CloseHandle(file);
    return tagged;
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
