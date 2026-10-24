#include "player/player_view.h"
#include "temporary_clip.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <iostream>
struct TrimCancelCheck;
#define private private: friend struct ::TrimCancelCheck; private
#include "player/clip_player.h"
#undef private

struct TrimCancelCheck {
    static bool finish(const std::filesystem::path& path, refarter::TemporaryClipIdentity identity, bool expectedRemoved) {
        refarter::player::ClipPlayer player;
        player.trimmed_ = path;
        player.trimmedIdentity_ = identity;
        player.trimCancel_ = true;
        player.trimSession_ = 99; // Completion must not navigate the current player.
        bool warned = false, saved = false;
        player.callbacks_.trimFailed = [&](const std::wstring&) { warned = true; };
        player.callbacks_.trimSaved = [&](const auto&) { saved = true; };
        player.handle(WM_APP + 21, 0, 0);
        return std::filesystem::exists(path) != expectedRemoved && warned != expectedRemoved && !saved;
    }
};
int main() {
    const auto root = std::filesystem::temp_directory_path() / (L"refarter-trim-cancel-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    const auto partial = root / L"trim.part", output = root / L"trim.mp4", moved = root / L"kept.mp4";
    int failures = 0;
    const auto check = [&](bool ok, const char* message) { if (!ok) { ++failures; std::cerr << message << '\n'; } };
    const auto write = [&](const std::filesystem::path& p) { std::ofstream(p) << "completed trim"; };
    write(partial);
    auto identity = refarter::temporaryClipIdentity(partial);
    std::filesystem::rename(partial, output);
    check(TrimCancelCheck::finish(output, identity, true), "owned published output is removed without follow-up");
    write(output);
    identity = refarter::temporaryClipIdentity(output);
    std::filesystem::rename(output, moved);
    write(output);
    check(TrimCancelCheck::finish(output, identity, false), "replacement is kept and cleanup warning reported");
    check(TrimCancelCheck::finish(output, {}, false), "unknown ownership cannot delete output");
    identity = refarter::temporaryClipIdentity(output);
    const HANDLE locked = CreateFileW(output.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "lock fixture opens");
    check(TrimCancelCheck::finish(output, identity, false), "locked output is kept and cleanup warning reported");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
