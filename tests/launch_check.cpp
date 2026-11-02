// Manual opt-in: does a build start? Launches the given Refarter.exe, waits for its window, lets it settle, closes it
// with --quit and checks its exit code and that no crash dump appeared. Needs a desktop session and no other Refarter
// running (the single-instance lock would make the result meaningless); only the copy it started is ever closed. It runs
// with the user's own profile (settings, clip folder, replay), so a dialog left open can make --quit time out.
// Usage: launch_check <path-to-Refarter.exe>
#include <windows.h>

#include <cstdio>
#include <set>
#include <string>

namespace {

constexpr wchar_t windowClass[] = L"RefarterDesktopShell";
constexpr wchar_t instanceLock[] = L"Local\\RefarterDesktopShell"; // The single-instance mutex of src/main.cpp.

struct Search {
    DWORD pid;
    bool found;
};

BOOL CALLBACK findOwned(HWND window, LPARAM param) {
    auto* search = reinterpret_cast<Search*>(param);
    wchar_t name[64]{};
    GetClassNameW(window, name, 64);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == search->pid && wcscmp(name, windowClass) == 0) { search->found = true; return FALSE; }
    return TRUE;
}

bool hasWindow(DWORD pid) {
    Search search{pid, false};
    EnumWindows(findOwned, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// The names of the .dmp files in the crashes folder.
std::set<std::wstring> dumpNames(const std::wstring& folder) {
    std::set<std::wstring> names;
    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW((folder + L"\\*.dmp").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return names;
    do {
        names.insert(found.cFileName);
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return names;
}

bool start(const std::wstring& exe, const wchar_t* arguments, PROCESS_INFORMATION& process) {
    std::wstring command = L"\"" + exe + L"\"" + (arguments ? std::wstring(L" ") + arguments : L"");
    STARTUPINFOW info{sizeof(info)};
    return CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &info, &process) != 0;
}

int fail(const wchar_t* message, PROCESS_INFORMATION* process = nullptr) {
    std::wprintf(L"FAIL: %ls\n", message);
    if (process && process->hProcess) {
        TerminateProcess(process->hProcess, 1);
        CloseHandle(process->hProcess);
        CloseHandle(process->hThread);
    }
    return 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { std::fwprintf(stderr, L"usage: launch_check <path-to-Refarter.exe>\n"); return 2; }
    const std::wstring exe = argv[1];
    // The window can be missing while the lock is held (a starting instance, or one in the tray), so test both.
    const HANDLE lock = OpenMutexW(SYNCHRONIZE, FALSE, instanceLock);
    if (lock) CloseHandle(lock);
    if (lock || FindWindowW(windowClass, nullptr)) {
        std::fwprintf(stderr, L"another Refarter is running; close it first (this check never touches it)\n");
        return 2;
    }
    std::wprintf(L"note: runs with your own Refarter profile (settings, replay); a dialog left open, such as the update question, can make --quit time out\n");

    wchar_t local[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    const std::wstring crashes = std::wstring(local) + L"\\Refarter\\crashes";
    const std::set<std::wstring> before = dumpNames(crashes);

    PROCESS_INFORMATION process{};
    if (!start(exe, nullptr, process)) return fail(L"could not start the program");
    const ULONGLONG begin = GetTickCount64();
    for (;;) {
        if (hasWindow(process.dwProcessId)) break;
        DWORD code = 0;
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            GetExitCodeProcess(process.hProcess, &code);
            CloseHandle(process.hProcess);
            CloseHandle(process.hThread);
            if (code == 0) {
                // A crash exits non-zero; a clean exit means the single-instance lock sent it to another copy.
                std::fwprintf(stderr, L"another Refarter took over (exit 0 before a window)\n");
                return 2;
            }
            std::wprintf(L"FAIL: exited with code 0x%08lX before its window appeared\n", code);
            return 1;
        }
        if (GetTickCount64() - begin > 15000) return fail(L"no window within 15 s", &process);
        Sleep(100);
    }
    std::wprintf(L"ok: window after %llu ms\n", GetTickCount64() - begin);

    if (WaitForSingleObject(process.hProcess, 3000) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(process.hProcess, &code);
        std::wprintf(L"FAIL: exited with code 0x%08lX within 3 s of showing its window\n", code);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        return 1;
    }
    std::wprintf(L"ok: still running after 3 s\n");

    PROCESS_INFORMATION quit{};
    if (!start(exe, L"--quit", quit)) return fail(L"could not run --quit", &process);
    WaitForSingleObject(quit.hProcess, 15000);
    CloseHandle(quit.hProcess);
    CloseHandle(quit.hThread);
    const ULONGLONG quitBegin = GetTickCount64();
    if (WaitForSingleObject(process.hProcess, 15000) != WAIT_OBJECT_0) return fail(L"did not exit within 15 s of --quit", &process);
    std::wprintf(L"ok: exited after --quit in about %llu ms\n", GetTickCount64() - quitBegin);
    // A fast-fail (0xC0000409) skips Refarter's own dump writer, so the exit code is the only signal for it.
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    if (exitCode != 0) {
        std::wprintf(L"FAIL: exited with code 0x%08lX after --quit\n", exitCode);
        return 1;
    }
    std::wprintf(L"ok: exit code 0\n");

    for (const std::wstring& name : dumpNames(crashes)) {
        if (before.count(name) == 0) {
            std::wprintf(L"FAIL: a new crash dump appeared in the crashes folder: %ls\n", name.c_str());
            return 1;
        }
    }
    std::wprintf(L"ok: no new crash dump\n");
    return 0;
}
