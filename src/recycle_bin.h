#pragma once

#include <windows.h>
#include <cstddef>
#include <filesystem>
#include <vector>

namespace refarter {

// Moves the files to the Recycle Bin in one operation, or leaves them where they are: any delete the shell would make
// permanent (no Recycle Bin on the drive, a file over the bin's size limit, a path it cannot recycle) is aborted, and
// nothing is asked. Returns how many files are still in place; 0 means all of them are in the bin. Every bin and the
// player's Replace use it, so nothing Refarter does can delete a clip for good.
std::size_t recycleOnly(const std::vector<std::filesystem::path>& paths, HWND owner);

} // namespace refarter
