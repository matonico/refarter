#include "diagnostic_log.h"
#include <fstream>
#include <iostream>

int main() {
    using namespace refarter;
    int failures = 0;
    const auto check = [&](bool okay, const char* text) { if (!okay) { std::cerr << text << '\n'; ++failures; } };
    const auto root = std::filesystem::temp_directory_path() /
        (L"refarter-log-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    {
        DiagnosticLog log;
        check(log.configure(false, root, "test"), "disabled configuration succeeds");
        log.write(DiagnosticEvent::CaptureStarted);
        check(log.finish(2000) && !std::filesystem::exists(root), "disabled logging creates no folder or files");
    }
    {
        DiagnosticLog log(1024);
        check(log.configure(true, root, "test"), "logging starts");
        // 60 lines are well under the 128-entry queue, so nothing is dropped whatever the writer's
        // speed, and at roughly 90 bytes each they overflow three 1 KiB files whatever the timing:
        // the oldest rotates away and exactly the three files session.log, .1 and .2 remain.
        for (unsigned n = 0; n < 60; ++n) log.write(DiagnosticEvent::CaptureWindowAttempt, E_INVALIDARG, n + 1);
        check(log.finish(2000), "writer drains on shutdown");
        unsigned files = 0;
        bool sawHresult = false, sawVersion = false, sawPid = false;
        for (const auto& file : std::filesystem::directory_iterator(root)) {
            ++files;
            check(file.file_size() <= 1024, "each rotated file stays within its byte limit");
            std::ifstream stream(file.path(), std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(stream), {}};
            sawHresult |= text.find("hr=0x80070057") != std::string::npos;
            sawVersion |= text.find("version=test") != std::string::npos;
            sawPid |= text.find("pid=") != std::string::npos;
        }
        check(files == 3 && sawHresult && !sawVersion && !sawPid,
            "rotating event lines preserve failure details without repeating session metadata");
    }
    {
        DiagnosticLog log;
        const auto switched = root / L"switched";
        check(log.configure(true, switched, "test"), "second logger starts");
        check(log.configure(false, switched, "test"), "can disable without waiting for disk");
        for (unsigned n = 0; n < 100; ++n) log.write(DiagnosticEvent::SaveRequested);
        check(log.finish(2000) && !std::filesystem::exists(switched), "disabled writes are discarded");
    }
    {
        // An invalid destination must not throw into capture or hold shutdown.
        const auto blocked = root / L"file";
        std::ofstream(blocked) << "fixture";
        DiagnosticLog log;
        check(log.configure(true, blocked / L"logs", "test"), "I/O is deferred to the writer");
        log.write(DiagnosticEvent::CaptureStarted);
        check(log.finish(2000), "storage failure does not hang the writer");
    }
    std::filesystem::remove_all(root); // Unique test-owned temporary directory only.
    return failures ? 1 : 0;
}
