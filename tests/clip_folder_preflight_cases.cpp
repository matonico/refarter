// Compile the production preflight with three Win32 calls intercepted. This tests the
// error path before the clip writer without filling a real drive or changing production APIs.
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <iostream>

namespace {
enum class Failure { CreateFull, WriteFull, Permission } failure;
HANDLE probeCreate(const wchar_t* path, DWORD access, DWORD sharing, LPSECURITY_ATTRIBUTES security,
                   DWORD disposition, DWORD flags, HANDLE templateFile) {
    if (failure == Failure::WriteFull)
        return CreateFileW(path, access, sharing, security, disposition, flags, templateFile);
    SetLastError(failure == Failure::CreateFull ? ERROR_DISK_FULL : ERROR_ACCESS_DENIED);
    return INVALID_HANDLE_VALUE;
}
BOOL probeWrite(HANDLE, const void*, DWORD, DWORD* written, OVERLAPPED*) {
    *written = 0;
    SetLastError(ERROR_DISK_FULL);
    return FALSE;
}
BOOL probeClose(HANDLE file) {
    const BOOL closed = CloseHandle(file);
    SetLastError(ERROR_ACCESS_DENIED); // The write error must already have been captured.
    return closed;
}
}
#define CreateFileW probeCreate
#define WriteFile probeWrite
#define CloseHandle probeClose
#include "../src/capture/clip_folder.cpp"
#undef CreateFileW
#undef WriteFile
#undef CloseHandle

int checkClipFolderPreflight() {
    const auto folder = std::filesystem::temp_directory_path() / (L"refarter-preflight-test-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    int failures = 0;
    for (const auto kind : {Failure::CreateFull, Failure::WriteFull, Failure::Permission}) {
        failure = kind;
        const auto result = refarter::capture::prepareClipFolder(folder);
        const auto expected = kind == Failure::Permission ? L"clips cannot be written to this folder" : L"disk full · free some space";
        if (!result || *result != expected) {
            std::cerr << "preflight did not preserve the original failure\n";
            ++failures;
        }
    }
    if (!std::filesystem::is_empty(folder)) {
        std::cerr << "preflight leaked its write probe\n";
        ++failures;
    }
    std::filesystem::remove_all(folder);
    return failures;
}
