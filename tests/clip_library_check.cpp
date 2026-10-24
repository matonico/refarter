#include "clip_library.h"

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>

using namespace refarter;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

// Manual opt-in: exercise Shell metadata and recycle ONLY fixtures created in
// this run's unique scratch directory. Never bins the supplied source video.
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::cerr << "clip_library_check <sample-video> <scratch-folder>\n"; return 2; }
    try {
        const auto root = fs::absolute(argv[2]) / (L"library-check-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        if (!fs::create_directories(root)) throw std::runtime_error("scratch directory already exists");
        const auto sample = root / L"sample.mp4";
        fs::copy_file(argv[1], sample);
        std::ofstream(root / L"unfinished.mp4.partial") << "unfinished";
        std::ofstream(root / L"notes.txt") << "not a clip";
        fs::create_directory(root / L"subfolder.mp4");

        std::mutex mutex;
        std::condition_variable ready;
        bool waiting = false;
        ClipLibrary library([&] { std::lock_guard lock(mutex); waiting = true; ready.notify_one(); });
        auto take = [&] {
            std::unique_lock lock(mutex);
            if (!ready.wait_for(lock, 30s, [&] { return waiting; })) throw std::runtime_error("worker timed out");
            waiting = false;
            return library.take();
        };
        auto require = [](bool value, const char* label) {
            if (!value) throw std::runtime_error(label);
            std::cout << "pass: " << label << '\n';
        };
        library.scan(root);
        auto result = take();
        require(result.clips && result.clips->size() == 1, "scan excludes unfinished files, non-videos and directories");
        auto clip = result.clips->front();
        require(clipBytes(*result.clips) == fs::file_size(sample), "size equals clip-file bytes only");
        library.want({clip}, {232, 130});
        result = take();
        require(result.thumbnails.size() == 1 && sameClip(result.thumbnails[0].clip, clip), "metadata carries file identity");
        const auto duration = result.thumbnails[0].duration;
        require(duration.has_value() && *duration > 0, "Shell supplies a positive duration");
        std::wcout << L"duration: " << formatClipDuration(*duration) << L" (" << *duration << L" ticks)\n";
        for (auto& item : result.thumbnails) if (item.bitmap) DeleteObject(item.bitmap);

        // Failed deletion must not lose an unchanged file, even if it is in use.
        HANDLE held = CreateFileW(sample.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        require(held != INVALID_HANDLE_VALUE, "open fixture without delete sharing");
        library.recycle(clip);
        result = take();
        CloseHandle(held);
        require(result.recycleError && !result.recycleError->empty() && fs::exists(sample), "locked clip survives bin failure");

        std::ofstream(sample, std::ios::binary | std::ios::app) << "replacement";
        library.recycle(clip);
        result = take();
        require(result.recycleError && !result.recycleError->empty() && fs::exists(sample), "stale tile cannot bin a changed file");
        clip = result.clips->front();
        library.want({clip}, {232, 130});
        result = take();
        require(result.thumbnails.size() == 1 && sameClip(result.thumbnails[0].clip, clip), "replacement gets fresh metadata identity");
        for (auto& item : result.thumbnails) if (item.bitmap) DeleteObject(item.bitmap);

        // Recycle a tiny disposable fixture; leave the playable sample in scratch.
        const auto disposable = root / L"bin-test.mp4";
        std::ofstream(disposable) << "disposable library test fixture";
        library.scan(root);
        result = take();
        const auto found = std::find_if(result.clips->begin(), result.clips->end(), [&](const auto& c) { return c.path == disposable; });
        require(found != result.clips->end(), "disposable fixture listed");
        library.recycle(*found);
        result = take();
        require(result.recycleError && result.recycleError->empty() && !fs::exists(disposable), "disposable fixture recycled successfully");
        require(result.clips && result.clips->size() == 1 && fs::exists(sample), "successful bin rescans without touching other files");
        require(clipBytes(*result.clips) == fs::file_size(sample), "folder usage updates after binning");
        std::wcout << L"scratch retained: " << root << L'\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
