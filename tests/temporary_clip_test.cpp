#include "temporary_clip.h"
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

int main() {
    using namespace refarter;
    int failures = 0;
    const auto check = [&](bool value, const char* message) { if (!value) { ++failures; std::cerr << message << '\n'; } };
    wchar_t temporary[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temporary);
    const auto root = std::filesystem::path(temporary) / (L"refarter-cleanup-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    const auto input = root / L"trim.mp4", output = root / L"20mb_trim.mp4";
    const auto write = [](const std::filesystem::path& file, const char* data) { std::ofstream stream(file, std::ios::binary); stream << data; };
    write(input, "trim");
    auto identity = temporaryClipIdentity(input);
    check(!discardExportIntermediate(input, identity, output) && std::filesystem::exists(input), "missing result keeps intermediate");
    write(output, "");
    check(!discardExportIntermediate(input, identity, output), "empty result keeps intermediate");
    write(output, "compressed");
    check(!discardExportIntermediate(input, identity, input), "already-fit file cannot delete itself");
    const HANDLE locked = CreateFileW(input.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "reader opened");
    check(!discardExportIntermediate(input, identity, output) && std::filesystem::exists(input), "in-use intermediate is kept");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    write(input, "changed trim");
    check(!discardExportIntermediate(input, identity, output) && std::filesystem::exists(input), "changed intermediate is kept");
    identity = temporaryClipIdentity(input);
    const auto moved = root / L"kept-original.mp4";
    check(MoveFileExW(input.c_str(), moved.c_str(), 0), "move original aside");
    write(input, "changed trim");
    const HANDLE replacement = CreateFileW(input.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(replacement != INVALID_HANDLE_VALUE, "replacement opened");
    if (replacement != INVALID_HANDLE_VALUE) {
        SetFileTime(replacement, nullptr, nullptr, &identity.info.ftLastWriteTime);
        CloseHandle(replacement);
    }
    check(!discardExportIntermediate(input, identity, output) && std::filesystem::exists(input), "same-size replacement with same time is kept");
    identity = temporaryClipIdentity(input);
    check(discardExportIntermediate(input, identity, output), "unchanged owned intermediate removed after publication");
    check(!std::filesystem::exists(input) && std::filesystem::exists(output) && std::filesystem::exists(moved), "only the intended intermediate removed");
    check(!temporaryClipIdentity(root).valid, "directories are not intermediates");
    check(!discardExportIntermediate(moved, {}, output), "missing identity never deletes");

    // A trim that already fits takes the compressed name, never over an existing file.
    const auto small = root / L"small trim.mp4";
    write(small, "small");
    write(root / L"20mb_small trim.mp4", "taken");
    auto smallIdentity = temporaryClipIdentity(small);
    write(small, "small, changed");
    check(tagTemporaryClip(small, smallIdentity).empty() && std::filesystem::exists(small), "a changed trim is not renamed");
    check(tagTemporaryClip(small, {}).empty(), "missing identity never renames");
    smallIdentity = temporaryClipIdentity(small);
    const HANDLE reader = CreateFileW(small.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(tagTemporaryClip(small, smallIdentity, 2).empty() && std::filesystem::exists(small), "a trim held open throughout is not renamed");
    // A reader that lets go while the rename is retried (the library's scan, a virus scanner) does not stop it.
    std::thread release([reader] { Sleep(150); if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader); });
    const auto tagged = tagTemporaryClip(small, smallIdentity);
    release.join();
    check(tagged == root / L"20mb_small trim (2).mp4", "a taken compressed name moves to the next number");
    check(!std::filesystem::exists(small) && std::filesystem::exists(tagged), "the trim itself was renamed");
    std::ifstream kept(root / L"20mb_small trim.mp4");
    std::string text;
    kept >> text;
    check(text == "taken", "the existing compressed copy is untouched");
    kept.close();
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
