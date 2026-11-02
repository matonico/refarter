#include "source_names.h"
#include <iostream>

using namespace refarter;

int main() {
    if (friendlySourceName(L"  Apex Legends  ", L"r5apex_dx12.exe") != L"apex legends" ||
        friendlySourceName(L" \t", L"Example.exe") != L"example.exe") return 1;
    const auto labels = sourceLabels({{L"Apex Legends", L"r5apex_dx12.exe"},
        {L"editor", L"first.exe"}, {L"EDITOR", L"second.exe"},
        {L"refarter", L"refarter.exe"}, {L"refarter", L"refarter.exe"}});
    const std::vector<std::wstring> expected{L"apex legends", L"editor · first.exe", L"editor · second.exe",
        L"refarter · refarter.exe · window 1", L"refarter · refarter.exe · window 2"};
    if (labels != expected) { std::cerr << "friendly labels do not uniquely identify duplicate windows\n"; return 1; }
    if (sourceLabels({{L"",L"game.exe"},{L"",L"game.exe"}}) !=
        std::vector<std::wstring>{L"game.exe · window 1",L"game.exe · window 2"}) return 1;

    // A whole-monitor save takes the listed game's name only while it is in front on the recorded monitor.
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    const ForegroundApp deadlock{true, true, L"  Deadlock ", L"project8.exe"};
    check(monitorSaveName(L"Monitor 1", deadlock) == L"deadlock", "a listed game in front names the clip like its window capture");
    ForegroundApp untitled = deadlock;
    untitled.title.clear();
    check(monitorSaveName(L"Monitor 1", untitled) == L"project8.exe", "an untitled listed game is named after its executable");
    ForegroundApp unlisted = deadlock;
    unlisted.listed = false;
    check(monitorSaveName(L"Monitor 1", unlisted) == L"monitor 1", "an unlisted app in front keeps the monitor name");
    ForegroundApp elsewhere = deadlock;
    elsewhere.onMonitor = false;
    check(monitorSaveName(L"Monitor 1", elsewhere) == L"monitor 1", "a listed game on another monitor keeps the monitor name");
    check(monitorSaveName(L"Monitor 1", std::nullopt) == L"monitor 1", "no usable window in front keeps the monitor name");
    return failures ? 1 : 0;
}
