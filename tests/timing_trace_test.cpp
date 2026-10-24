#include "capture/timing_trace.h"
#include <iostream>
#include <thread>
#include <iterator>
#include <windows.h>

using namespace refarter::capture;

int main() {
    TimingTrace bounded(3);
    for (int i = 0; i < 5; ++i) bounded.record({TimingKind::Submitted, i, i});
    const auto wrapped = bounded.snapshot();
    if (wrapped.overwritten != 2 || wrapped.events.size() != 3 ||
        wrapped.events.front().captureTime != 2 || wrapped.events.back().captureTime != 4) {
        std::cerr << "bounded trace loses chronological insertion order or overwrite count\n";
        return 1;
    }
    TimingTrace concurrent(2'000);
    const auto producer = [&concurrent](TimingKind kind) {
        for (int i = 0; i < 1'000; ++i) concurrent.record({kind, i, i});
    };
    std::thread capture(producer, TimingKind::Submitted), encoder(producer, TimingKind::Encoded);
    bool boundedSnapshots = true;
    for (int i = 0; i < 50; ++i) {
        boundedSnapshots &= concurrent.snapshot().events.size() <= 2'000;
    }
    capture.join();
    encoder.join();
    const auto both = concurrent.snapshot();
    std::size_t captured = 0, encoded = 0;
    for (const auto& event : both.events) {
        captured += event.kind == TimingKind::Submitted;
        encoded += event.kind == TimingKind::Encoded;
    }
    if (!boundedSnapshots || both.overwritten || captured != 1'000 || encoded != 1'000) return 1;
    TimingTrace empty(0);
    empty.record({TimingKind::CaptureError});
    if (!empty.snapshot().events.empty() || empty.snapshot().overwritten != 1) return 1;
    // Passing a directory cannot open a CSV; diagnostic failure is reported, not thrown.
    if (bounded.write(std::filesystem::current_path(), 0, 0, 10, 60, 1920, 1080, 0, false)) return 1;
    wchar_t folder[MAX_PATH]{}, temporary[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, folder) || !GetTempFileNameW(folder, L"rft", 0, temporary)) return 1;
    const bool written = bounded.write(temporary, 49'021'191'055, 49'021'191'055,
                                       49'081'210'971, 60, 1920, 1080, 2, false, true, -1, 0, 3);
    std::ifstream file(temporary, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    std::error_code cleanupError;
    std::filesystem::remove(temporary, cleanupError);
    if (!written || cleanupError || text.find("clock_start=49021191055") == std::string::npos ||
        text.find("overwritten_events=2") == std::string::npos ||
        text.find("frame_budget=3") == std::string::npos ||
        text.find("submitted,2,2,-1,0,0,-1") == std::string::npos) return 1;
    return 0;
}
