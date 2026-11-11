// update_download.cpp against a scripted httpsGet (defined below in place of https_get.cpp): which outcome each answer
// gives, that a checked setup is locked, and that nothing runnable is left behind by a failure. No network is used.
#include "https_get.h"
#include "text.h"
#include "update_check.h"
#include "update_download.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

namespace refarter {
namespace {
struct Scripted { DWORD status = 0; bool complete = false; std::string body; };
std::map<std::wstring, Scripted> answers; // By the end of the path: "SHA256SUMS.txt" or the setup's name.
} // namespace

HttpsAnswer httpsGet(const std::wstring&, std::wstring_view, const std::wstring& path, const wchar_t*, DWORD,
                     const std::function<bool(const char*, DWORD, std::uint64_t)>& sink) {
    const auto file = path.substr(path.rfind(L'/') + 1);
    const auto found = answers.find(file);
    if (found == answers.end()) return {}; // Offline.
    const auto& scripted = found->second;
    HttpsAnswer answer{scripted.status, scripted.complete};
    if (scripted.status == 200 && scripted.complete && !scripted.body.empty() &&
        !sink(scripted.body.data(), static_cast<DWORD>(scripted.body.size()), scripted.body.size())) answer.complete = false;
    return answer;
}
} // namespace refarter

int main() {
    using namespace refarter;
    using Outcome = DownloadResult::Outcome;
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };

    const HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    constexpr UINT done = WM_APP + 1, progress = WM_APP + 2;
    const auto folder = std::filesystem::temp_directory_path() / (L"refarter-update-download-test-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);

    const std::wstring tag = L"v9.8.7";
    const auto setup = update::setupName(tag);
    const auto setupUtf8 = utf8(setup);
    const std::string body = "hello"; // SHA-256 below.
    const std::string sums = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824  " + setupUtf8 + "\r\n";

    // One download from the scripted answers, to its result; a worker that posts nothing fails after 10 s.
    const auto run = [&](const std::filesystem::path& where) {
        UpdateDownload download;
        download.start(window, done, progress, tag, L"Refarter/test", where);
        SetTimer(window, 1, 10'000, nullptr);
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == WM_TIMER) { std::cerr << "no result within 10 s\n"; ++failures; break; }
            if (message.message != done) continue;
            if (auto result = download.take()) { KillTimer(window, 1); return *result; }
        }
        KillTimer(window, 1);
        DownloadResult none;
        none.outcome = static_cast<Outcome>(-1); // Matches no expected outcome.
        return none;
    };
    const auto runnable = [&] { return std::filesystem::exists(folder / setup); };

    answers = {{L"SHA256SUMS.txt", {200, true, sums}}, {setup, {200, true, body}}};
    auto result = run(folder);
    check(result.outcome == Outcome::Ready && result.setup == folder / setup && result.lock, "a matching setup is ready and locked");
    {
        const HANDLE writer = CreateFileW(result.setup.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr, OPEN_EXISTING, 0, nullptr);
        check(writer == INVALID_HANDLE_VALUE, "the locked setup cannot be opened for writing");
        if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
        check(!DeleteFileW(result.setup.c_str()), "the locked setup cannot be deleted");
    }
    answers.erase(setup); // Offline for the setup itself: the whole file already there is used again.
    result = run(folder);
    check(result.outcome == Outcome::Ready, "a whole setup already downloaded is used again");

    answers = {{L"SHA256SUMS.txt", {200, true, sums}}, {setup, {200, true, "hellp"}}};
    result.lock.reset();
    std::filesystem::remove_all(folder, ignored);
    result = run(folder);
    check(result.outcome == Outcome::Mismatch && !result.lock, "corrupt bytes are a mismatch");
    check(!runnable(), "a mismatching download never gets the setup's name");

    answers = {{L"SHA256SUMS.txt", {404, true, {}}}, {setup, {200, true, body}}};
    check(run(folder).outcome == Outcome::NoChecksum, "a release without SHA256SUMS.txt has no checksum");
    answers = {{L"SHA256SUMS.txt", {200, true, "0000  another.exe\n"}}, {setup, {200, true, body}}};
    check(run(folder).outcome == Outcome::NoChecksum, "a list that does not name the setup has no checksum");
    for (const DWORD status : {DWORD{503}, DWORD{403}, DWORD{429}, DWORD{500}}) {
        answers = {{L"SHA256SUMS.txt", {status, true, {}}}, {setup, {200, true, body}}};
        check(run(folder).outcome == Outcome::Network, "a server or rate-limit answer for the list stays a retryable network failure");
    }
    answers = {{setup, {200, true, body}}}; // No answer at all for the list.
    check(run(folder).outcome == Outcome::Network, "offline is a network failure");
    answers = {{L"SHA256SUMS.txt", {200, true, sums}}, {setup, {503, true, {}}}};
    check(run(folder).outcome == Outcome::Network && !runnable(), "a failed setup request is a network failure and leaves nothing");
    answers = {{L"SHA256SUMS.txt", {200, true, sums}}, {setup, {200, false, body}}};
    check(run(folder).outcome == Outcome::Network && !runnable(), "a setup cut off part way is a network failure and leaves nothing");

    // A folder that cannot be made: its path is a file.
    const auto blocked = folder / L"not-a-folder";
    std::filesystem::create_directories(folder, ignored);
    std::ofstream(blocked) << "x";
    answers = {{L"SHA256SUMS.txt", {200, true, sums}}, {setup, {200, true, body}}};
    check(run(blocked / L"updates").outcome == Outcome::Disk, "an unusable folder is a disk failure");

    std::filesystem::remove_all(folder, ignored);
    DestroyWindow(window);
    if (failures == 0) std::cout << "update_download: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
