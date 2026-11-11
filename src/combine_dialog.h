#pragma once
#include <windows.h>
#include <filesystem>
#include <vector>
namespace refarter {
// Keeps the chosen order only on confirmation. The caller supplies the chronological starting order.
bool orderCombinedClips(HWND owner, std::vector<std::filesystem::path>& clips);
}
