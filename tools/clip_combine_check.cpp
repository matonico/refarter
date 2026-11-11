#include "player/clip_combine.h"
#include "storage_limit.h"
#include <windows.h>
#include <psapi.h>
#include <chrono>
#include <iostream>
// Manual file-only check. Writes beside the first input; never changes any input.
int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { std::wcerr << L"usage: clip_combine_check <first clip> <second clip> [more clips]\n"; return 2; }
    std::vector<std::filesystem::path> clips;
    for (int i=1;i<argc;++i) clips.emplace_back(argv[i]);
    std::atomic<float> progress{0}; std::atomic<bool> cancel{false};
    const auto start = std::chrono::steady_clock::now();
    // Leaves the same free-space reserve as a replay save, so a probe beside a recording cannot fill the drive.
    const auto result = refarter::player::combineClips(clips,progress,cancel,{0,refarter::saveReserve,true});
    PROCESS_MEMORY_COUNTERS memory{sizeof(memory)}; GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory));
    std::wcout << L"elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count()
               << L" peak_working_set_bytes=" << memory.PeakWorkingSetSize << L'\n';
    if (!result.error.empty()) { std::wcerr << result.error << L'\n'; return 1; }
    std::wcout << result.path.wstring() << L'\n'; return 0;
}
