// Crashes copies of itself in each way crash_dumps.h covers, also while globals are destroyed at exit, and
// checks that every one ends promptly with exactly one readable minidump, that a clean exit leaves none,
// and that old dumps are trimmed to the newest few.
#include "crash_dumps.h"
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
#pragma warning(push)
#pragma warning(disable : 4717) // Recursing on every path is the point.
int overflow(int depth) {
    volatile char frame[4096];
    frame[0] = static_cast<char>(depth);
    return overflow(depth + 1) + frame[0];
}
#pragma warning(pop)

// Crashes while the C runtime destroys globals at exit, with the writer thread still alive.
struct CrashOnExit {
    bool armed = false;
    ~CrashOnExit() { if (armed) *static_cast<volatile int*>(nullptr) = 1; }
} crashOnExit;

// The crashing copy: installs the handler, then ends as `how` says.
int crash(const std::wstring& how, const std::filesystem::path& folder) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX); // No Windows Error Reporting window in a test.
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
    if (!refarter::installCrashDumps(folder, L"refarter test")) return 2;
    if (how == L"access") *static_cast<volatile int*>(nullptr) = 1;
    else if (how == L"overflow") return overflow(0);
    else if (how == L"throw") throw std::runtime_error("uncaught");
    else if (how == L"thread") std::thread([] { throw std::runtime_error("uncaught on a thread"); }).join();
    else if (how == L"abort") std::abort();
    else if (how == L"parameter") { char* nowhere = nullptr; strcpy_s(nowhere, 1, "x"); }
    else if (how == L"destructor") crashOnExit.armed = true;
    return 0; // "none": a clean exit.
}

std::vector<std::filesystem::path> dumpsIn(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> found;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error))
        if (entry.path().extension() == L".dmp") found.push_back(entry.path());
    return found;
}

bool isMinidump(const std::filesystem::path& file) {
    char signature[4]{};
    std::ifstream(file, std::ios::binary).read(signature, 4);
    return std::memcmp(signature, "MDMP", 4) == 0 && std::filesystem::file_size(file) > 1024;
}

constexpr DWORD notRun = 0xFFFFFFFE, hung = 0xFFFFFFFF; // Codes no crash or clean exit produces.

// Runs a crashing copy and returns its exit code, notRun or hung.
DWORD run(const std::wstring& how, const std::filesystem::path& folder) {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" crash " + how + L" \"" + folder.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) return notRun;
    DWORD code = hung;
    if (WaitForSingleObject(process.hProcess, 60000) == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
    else TerminateProcess(process.hProcess, hung);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring(argv[1]) == L"crash") return crash(argv[2], argv[3]);
    int failures = 0;
    const auto check = [&](bool passes, const std::string& name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto root = std::filesystem::path(temp) / (L"refarter-crash-dumps-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(root);

    for (const auto* how : {L"access", L"overflow", L"throw", L"thread", L"abort", L"parameter", L"destructor"}) {
        const auto folder = root / how / L"crashes"; // The parent does not exist yet either.
        std::string name; // ASCII.
        for (const wchar_t* c = how; *c; ++c) name += static_cast<char>(*c);
        const DWORD code = run(how, folder);
        check(code != 0 && code != notRun && code != hung, name + ": the copy crashed promptly");
        const auto dumps = dumpsIn(folder);
        check(dumps.size() == 1, name + ": exactly one dump");
        if (dumps.size() == 1) {
            check(isMinidump(dumps[0]), name + ": a minidump");
            check(dumps[0].filename().wstring().starts_with(L"refarter test 20"), name + ": named with the label and date");
        }
    }

    // A folder whose dump names could not fit is refused before anything is installed.
    check(!refarter::installCrashDumps(root / std::wstring(250, L'x'), L"refarter test"), "too long a folder is refused");

    // A clean exit writes nothing and creates no folder.
    check(run(L"none", root / L"none") == 0, "clean exit: exit code 0");
    check(!std::filesystem::exists(root / L"none"), "clean exit: no folder");

    // Twelve old dumps: the next start keeps the newest nine, the crash adds one.
    const auto crowded = root / L"crowded";
    std::filesystem::create_directories(crowded);
    const auto now = std::filesystem::file_time_type::clock::now();
    for (int i = 0; i < 12; ++i) {
        const auto file = crowded / (L"old " + std::to_wstring(i) + L".dmp");
        std::ofstream(file) << "x";
        std::filesystem::last_write_time(file, now - std::chrono::hours(24 * (12 - i))); // "old 0" is the oldest.
    }
    std::ofstream(crowded / L"notes.txt") << "not a dump";
    const DWORD crowdedCode = run(L"access", crowded);
    check(crowdedCode != 0 && crowdedCode != notRun && crowdedCode != hung, "crowded: the copy crashed promptly");
    check(dumpsIn(crowded).size() == refarter::keptCrashDumps, "crowded: nine kept and the new one added");
    check(!std::filesystem::exists(crowded / L"old 2.dmp") && std::filesystem::exists(crowded / L"old 3.dmp"), "crowded: the oldest go first");
    check(std::filesystem::exists(crowded / L"notes.txt"), "crowded: other files stay");

    std::filesystem::remove_all(root);
    if (failures) std::cerr << failures << " check(s) failed\n";
    return failures ? 1 : 0;
}
