#include "temporary_clip.h"
#include <fstream>
#include <iostream>

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
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
