// Manual opt-in: what one hover preview costs on this PC. Runs player::ClipPreview on a real clip, as the library does
// (12 keyframes at a tile's size), and prints the time to the first and the last picture, the process CPU time and the
// peak working set. Usage: clip_preview_check <clip> [repeats]
#include "player/clip_preview.h"

#include <psapi.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { std::fwprintf(stderr, L"usage: clip_preview_check <clip> [repeats]\n"); return 2; }
    const int repeats = argc > 2 ? std::max(1, _wtoi(argv[2])) : 3;
    std::mutex mutex;
    std::condition_variable arrived;
    int pending = 0;
    refarter::player::ClipPreview preview([&] { std::lock_guard lock(mutex); ++pending; arrived.notify_all(); });
    const auto cpu = [] {
        FILETIME created, exited, kernel, user;
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        const auto ticks = [](const FILETIME& t) { return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        return (ticks(kernel) + ticks(user)) / 10000.0; // ms
    };
    for (int run = 0; run < repeats; ++run) {
        const double cpuBefore = cpu();
        const auto start = std::chrono::steady_clock::now();
        const auto number = preview.request(argv[1], 12, 232, 130, MonitorFromPoint({}, MONITOR_DEFAULTTOPRIMARY));
        int pictures = 0;
        double first = -1;
        while (pictures < 12) {
            std::unique_lock lock(mutex);
            if (!arrived.wait_for(lock, std::chrono::seconds(10), [&] { return pending > 0; })) break;
            pending = 0;
            lock.unlock();
            for (const auto& picture : preview.take()) {
                if (picture.request == number) ++pictures;
                DeleteObject(picture.bitmap);
            }
            if (first < 0 && pictures > 0) first = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }
        const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        PROCESS_MEMORY_COUNTERS memory{sizeof(memory)};
        GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
        std::wprintf(L"run %d: %d pictures, first %.0f ms, all %.0f ms, cpu %.0f ms, peak working set %.1f MiB\n", run + 1, pictures, first, total,
                     cpu() - cpuBefore, memory.PeakWorkingSetSize / 1048576.0);
        if (pictures < 12) return 1;
    }
    return 0;
}
