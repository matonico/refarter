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
    return 0;
}
