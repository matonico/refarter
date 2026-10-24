#include "crash_dumps.h"

#include <windows.h>
#include <dbghelp.h>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cwchar>
#include <utility>
#include <vector>

namespace refarter {

namespace {
constexpr DWORD abortCode = 0x40000015; // STATUS_FATAL_APP_EXIT, which Windows reports for abort().

// Fixed at install, in plain buffers: a crashed process should not need to allocate, and the C runtime
// destroys global objects at exit while the writer may still run.
wchar_t folder[MAX_PATH]{}, name[64]{};
HANDLE requested = nullptr, written = nullptr; // written is manual-reset: every crashed thread sees it.
HANDLE writerHandle = nullptr; // Kept open: signalled once ExitProcess has ended the writer.
DWORD writerThread = 0;
EXCEPTION_POINTERS* crash = nullptr;
DWORD crashedThread = 0;
std::atomic<bool> claimed = false;
LPTOP_LEVEL_EXCEPTION_FILTER previous = nullptr;

// Leaves room for one more dump: the newest keptCrashDumps - 1 stay.
void removeOldDumps() {
    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW((std::wstring(folder) + L"\\*.dmp").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    std::vector<std::pair<ULONGLONG, std::wstring>> dumps;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const auto time = (static_cast<ULONGLONG>(found.ftLastWriteTime.dwHighDateTime) << 32) | found.ftLastWriteTime.dwLowDateTime;
        dumps.emplace_back(time, found.cFileName);
    } while (FindNextFileW(search, &found));
    FindClose(search);
    std::sort(dumps.begin(), dumps.end()); // Oldest first.
    for (std::size_t i = 0; i + keptCrashDumps <= dumps.size(); ++i) DeleteFileW((std::wstring(folder) + L"\\" + dumps[i].second).c_str());
}

// Every missing folder on the way to `folder`, without allocating.
void createFolders() {
    wchar_t path[MAX_PATH * 2]{};
    wcsncpy_s(path, folder, _TRUNCATE);
    for (wchar_t* c = path + 3; c < path + wcslen(path); ++c) // After the drive's "C:\".
        if (*c == L'\\') { *c = L'\0'; CreateDirectoryW(path, nullptr); *c = L'\\'; }
    CreateDirectoryW(path, nullptr);
}

// Tidies the folder once, then sleeps until a crash asks for a dump.
DWORD WINAPI writer(void*) {
    removeOldDumps();
    WaitForSingleObject(requested, INFINITE);
    // From System32 only, and only now: an app that never crashes never loads it.
    if (const HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        const auto write = reinterpret_cast<decltype(&MiniDumpWriteDump)>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t path[MAX_PATH * 2]{};
        swprintf_s(path, L"%ls\\%ls %04u-%02u-%02u %02u-%02u-%02u.dmp", folder, name, now.wYear, now.wMonth,
                   now.wDay, now.wHour, now.wMinute, now.wSecond);
        createFolders();
        const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION exception{crashedThread, crash, FALSE};
            const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
            const bool done = write && write(GetCurrentProcess(), GetCurrentProcessId(), file, type, &exception, nullptr, nullptr);
            CloseHandle(file);
            if (!done) DeleteFileW(path);
        }
    }
    SetEvent(written);
    return 0;
}

// The crashed thread hands its exception to the writer and waits: its own stack may be all but gone (an overflow).
void dump(EXCEPTION_POINTERS* pointers) {
    if (!requested || GetCurrentThreadId() == writerThread) return;
    if (!claimed.exchange(true)) { // The first crash is dumped; any other waits for that dump.
        crash = pointers;
        crashedThread = GetCurrentThreadId();
        SetEvent(requested);
    }
    // A writer stuck on a lock the crash holds must not keep the process alive, and one that ExitProcess has
    // already ended (a crash in a late destructor or DLL unload) will never answer.
    const HANDLE answers[] = {written, writerHandle};
    WaitForMultipleObjects(2, answers, FALSE, 30000);
}

LONG WINAPI unhandled(EXCEPTION_POINTERS* pointers) {
    dump(pointers);
    // The C runtime's own filter turns an uncaught C++ exception into std::terminate.
    return previous ? previous(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

LONG handOver(EXCEPTION_POINTERS* pointers) {
    dump(pointers);
    return EXCEPTION_EXECUTE_HANDLER;
}

// For ends that raise no exception: raising one records where the program stands.
void dumpHere() {
    __try {
        RaiseException(abortCode, 0, 0, nullptr);
    } __except (handOver(GetExceptionInformation())) {
    }
}

// abort, so also std::terminate and pure virtual calls; abort then goes on to Windows Error Reporting.
void onAbort(int) { dumpHere(); }

void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    dumpHere();
    __fastfail(FAST_FAIL_INVALID_ARG); // How the default handler ends the process.
}
} // namespace

bool installCrashDumps(const std::filesystem::path& where, const std::wstring& label) {
    // Room for the dump's own name after the folder.
    if (requested || where.empty() || where.native().size() + std::size(name) + 30 >= MAX_PATH) return false;
    wcsncpy_s(folder, where.c_str(), _TRUNCATE);
    wcsncpy_s(name, label.c_str(), _TRUNCATE);
    requested = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    written = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    writerHandle = requested && written
        ? CreateThread(nullptr, 256 * 1024, writer, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &writerThread) : nullptr;
    if (!writerHandle) {
        if (requested) CloseHandle(requested);
        if (written) CloseHandle(written);
        requested = written = nullptr;
        return false;
    }
    SetThreadDescription(writerHandle, L"refarter crash dumps");
    previous = SetUnhandledExceptionFilter(unhandled);
    std::signal(SIGABRT, onAbort);
    _set_invalid_parameter_handler(onInvalidParameter);
    return true;
}

} // namespace refarter
