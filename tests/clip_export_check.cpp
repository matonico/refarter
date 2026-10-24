// Manual only: clip_export_check <source> <scratch-folder>
// Never exports beside the user's original. Checks cancellation, file locking,
// name collisions, the byte cap and timestamps in a fully decoded H.264 copy.
#include "player/clip_export.h"
#include "player/export_plan.h"
#include "player/media.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}
#include <windows.h>
#include <psapi.h>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
using namespace refarter::player;
namespace {
int failures = 0;
double cpuSeconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    return ((static_cast<std::uint64_t>(kernel.dwHighDateTime) << 32) + kernel.dwLowDateTime +
        (static_cast<std::uint64_t>(user.dwHighDateTime) << 32) + user.dwLowDateTime) / 1e7;
}
void check(bool pass, const char* message) {
    std::cout << (pass ? "ok   " : "FAIL ") << message << '\n';
    if (!pass) ++failures;
}
std::vector<double> timestamps(const std::filesystem::path& path, int& width, int& height, double& duration, int& tracks) {
    MediaFile file;
    check(file.open(path, {nullptr, false, true}), "software decoder opens the clip");
    duration = file.duration(); tracks = file.audioTracks();
    std::vector<double> times;
    auto first = file.seek(0);
    if (first) { width = first.frame->width; height = first.frame->height; times.push_back(first.time); }
    while (auto picture = file.next()) times.push_back(picture.time);
    return times;
}
bool noPartial(const std::filesystem::path& folder) {
    for (const auto& file : std::filesystem::directory_iterator(folder)) if (file.path().extension() == L".part") return false;
    return true;
}
}
int wmain(int argc, wchar_t** argv) {
    std::cout << std::unitbuf;
    std::wcout << std::unitbuf;
    if (argc != 3) { std::cerr << "usage: clip_export_check <source> <scratch-folder>\n"; return 2; }
    const std::filesystem::path folder(argv[2]);
    std::filesystem::create_directories(folder);
    const auto source = folder / std::filesystem::path(argv[1]).filename();
    if (std::filesystem::exists(source)) { std::cerr << "scratch source already exists; use a fresh folder\n"; return 2; }
    std::filesystem::copy_file(argv[1], source);
    const auto bytes = std::filesystem::file_size(source);
    const auto written = std::filesystem::last_write_time(source);
    std::atomic<float> progress = 0;
    std::atomic<bool> cancel = false;
    const auto tiny = folder / L"already-small.mp4";
    { std::ofstream file(tiny); file << "small"; }
    const auto fits = compressClip(tiny, progress, cancel);
    check(fits.alreadyFits && fits.path.empty() && noPartial(folder), "already-small input does not create a larger copy");
    const auto missing = compressClip(folder / L"missing.mp4", progress, cancel);
    check(!missing.error.empty() && missing.path.empty() && noPartial(folder), "missing file reports an error without leftovers");
    auto locked = CreateFileW(source.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto busy = compressClip(source, progress, cancel);
    check(locked != INVALID_HANDLE_VALUE && !busy.error.empty() && noPartial(folder), "busy source fails safely");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    cancel.store(true);
    auto cancelled = compressClip(source, progress, cancel);
    check(cancelled.cancelled && cancelled.path.empty() && noPartial(folder), "pre-cancel leaves no output or partial");
    cancel.store(false);
    std::atomic<bool> done = false;
    ExportResult result;
    std::thread thread([&] { result = compressClip(source, progress, cancel); done.store(true); });
    while (!done.load() && progress.load() <= 0.02f) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto write = CreateFileW(source.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(!done.load() && write == INVALID_HANDLE_VALUE, "active export prevents source modification");
    if (write != INVALID_HANDLE_VALUE) CloseHandle(write);
    cancel.store(true); thread.join();
    check(result.cancelled && result.path.empty() && noPartial(folder), "mid-export cancellation removes the partial");
    cancel.store(false);
    const auto occupied = exportPath(source);
    { std::ofstream marker(occupied); marker << "do not overwrite"; }
    const auto started = std::chrono::steady_clock::now();
    const double cpuStart = cpuSeconds();
    result = compressClip(source, progress, cancel);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    PROCESS_MEMORY_COUNTERS memory{sizeof(memory)};
    GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
    std::cout << "export cpu seconds " << cpuSeconds() - cpuStart << ", peak working bytes " << memory.PeakWorkingSetSize << '\n';
    std::wcout << L"export " << seconds << L" s: " << result.path << L" " << result.error << L'\n';
    check(!result.path.empty() && result.error.empty(), "hardware export succeeds");
    check(result.path == exportPath(source, 2), "collision produces a separate copy");
    check(std::filesystem::file_size(occupied) == 16, "existing export is untouched");
    check(noPartial(folder), "successful export leaves no partial");
    check(bytes == std::filesystem::file_size(source) && written == std::filesystem::last_write_time(source), "source metadata unchanged");
    if (result.path.empty()) return 1;
    const auto size = std::filesystem::file_size(result.path);
    check(size > 0 && size < exportLimit, "complete file is below 20,000,000 bytes");
    int sw = 0, sh = 0, ow = 0, oh = 0;
    double sourceDuration = 0, outputDuration = 0;
    int sourceTracks = 0, outputTracks = 0;
    const auto original = timestamps(source, sw, sh, sourceDuration, sourceTracks);
    const auto output = timestamps(result.path, ow, oh, outputDuration, outputTracks);
    check(std::abs(sourceDuration - outputDuration) <= 0.002, "export preserves the complete clip duration");
    check(outputTracks == (sourceTracks ? 1 : 0), "share copy has only the stored mix (or no audio)");
    check(!output.empty() && std::adjacent_find(output.begin(), output.end(), [](double a, double b) { return b <= a; }) == output.end(),
        "output pictures decode in strictly increasing timestamp order");
    bool sourceTimes = true;
    for (const double time : output) {
        auto found = std::lower_bound(original.begin(), original.end(), time - 0.00001);
        if (found == original.end() || std::abs(*found - time) > 0.00001) sourceTimes = false;
    }
    check(sourceTimes, "every output timestamp belongs to a source picture");
    check(output.size() <= original.size(), "compression never invents extra pictures");
    if (sw == ow && sh == oh) {
        bool same = original.size() == output.size();
        for (std::size_t i = 0; same && i < output.size(); ++i) same = std::abs(original[i] - output[i]) < 0.00001;
        check(same, "native-resolution export preserves every source timestamp");
    }
    std::cout << "bytes " << size << ", frames " << output.size() << "/" << original.size() << '\n';
    return failures ? 1 : 0;
}
