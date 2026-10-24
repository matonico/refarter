#include "leftover_files.h"
#include <windows.h>
#include <cstddef>
#include <cstring>
#include <memory>

namespace refarter {
namespace {
struct CloseFile {
    void operator()(void* handle) const { CloseHandle(handle); }
};
using File = std::unique_ptr<void, CloseFile>;

// A symbolic link or a mount point (junction) is never followed or moved. Other reparse points, such as a OneDrive
// cloud placeholder, are ordinary files. A tag that cannot be read counts as a link.
bool isLink(HANDLE handle, const BY_HANDLE_FILE_INFORMATION& data) {
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag))) return true;
    return tag.ReparseTag == IO_REPARSE_TAG_SYMLINK || tag.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT;
}

bool recoverOne(const std::filesystem::path& file) {
    if (!isLeftoverName(file.filename().wstring())) return false;
    // No write/delete sharing: a live writer or path replacement cannot race the checks and rename.
    const HANDLE raw = CreateFileW(file.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    File source(raw);
    BY_HANDLE_FILE_INFORMATION data{};
    if (!GetFileInformationByHandle(source.get(), &data) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || isLink(source.get(), data)) return false;
    FILETIME current{};
    GetSystemTimeAsFileTime(&current);
    const auto ticks = [](FILETIME time) {
        return (static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    const auto now = ticks(current), written = ticks(data.ftLastWriteTime);
    if (written > now || now - written < static_cast<unsigned long long>(leftoverMinAge.count()) * 10'000'000) return false;

    const auto folder = file.parent_path() / leftoverRecoveryFolder;
    if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    // Hold the destination directory against replacement and reject junctions/symlinks.
    const HANDLE dir = CreateFileW(folder.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (dir == INVALID_HANDLE_VALUE) return false;
    File directory(dir);
    if (!GetFileInformationByHandle(directory.get(), &data) || !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        isLink(directory.get(), data)) return false;

    std::error_code error;
    const auto destination = std::filesystem::absolute(folder / file.filename(), error).wstring();
    if (error) return false;
    const auto nameBytes = destination.size() * sizeof(wchar_t);
    const auto size = offsetof(FILE_RENAME_INFO, FileName) + nameBytes + sizeof(wchar_t);
    // operator new supplies the alignment needed by FILE_RENAME_INFO.
    std::unique_ptr<void, void(*)(void*)> storage(::operator new(size), [](void* p) { ::operator delete(p); });
    std::memset(storage.get(), 0, size);
    auto* rename = static_cast<FILE_RENAME_INFO*>(storage.get());
    rename->ReplaceIfExists = FALSE;
    rename->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(rename->FileName, destination.c_str(), nameBytes);
    // FileRenameInfo never copies across volumes, overwrites a collision or deletes a file.
    return SetFileInformationByHandle(source.get(), FileRenameInfo, rename, static_cast<DWORD>(size)) != FALSE;
}
} // namespace

int recoverLeftovers(const std::vector<std::filesystem::path>& files) {
    int moved = 0;
    for (const auto& file : files) {
        try { if (recoverOne(file)) ++moved; }
        catch (const std::exception&) {} // Preserve earlier successes; this file stays in place.
    }
    return moved;
}
} // namespace refarter
