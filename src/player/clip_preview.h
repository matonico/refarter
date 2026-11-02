#pragma once

#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace refarter::player {

// The library's hover preview: a few keyframes spread over one clip, at tile size. Decoded only on demand, on a
// background-priority thread of its own that starts with the first request and sleeps between requests; nothing runs
// while no tile is hovered. Only the newest request is worked on, and only keyframes are decoded (one each, from its
// own seek), on the GPU that shows the window. The file is closed as soon as a request is done or dropped.
class ClipPreview {
public:
    struct Picture {
        std::uint64_t request = 0; // Which request it answers: older ones are discarded by the receiver.
        int index = 0;             // 0 … count-1, left to right across the tile.
        HBITMAP bitmap = nullptr;  // Premultiplied top-down 32-bit DIB of the asked size, owned by the receiver.
    };

    // `ready` runs on the preview thread once results wait (once per batch): post to the UI, then return.
    explicit ClipPreview(std::function<void()> ready) : ready_(std::move(ready)) {}
    ~ClipPreview();
    ClipPreview(const ClipPreview&) = delete;
    ClipPreview& operator=(const ClipPreview&) = delete;

    // `count` pictures of `clip` at `width` × `height`, replacing any earlier request; returns its number.
    std::uint64_t request(std::filesystem::path clip, int count, UINT width, UINT height, HMONITOR monitor);
    void cancel(); // No tile hovered: stop at the next picture.
    std::vector<Picture> take();

    // The time of picture `index` of `count` over `duration` seconds: the middle of its slice of the clip.
    static double timeOf(int index, int count, double duration) { return (index + 0.5) * duration / count; }

    // Coarse first, then the gaps: of 12 pictures 0 and 6, then 3 and 9, then the rest, so a quick sweep across the
    // tile already shows something everywhere.
    static std::vector<int> decodeOrder(int count) {
        std::vector<int> order;
        std::vector<bool> taken(static_cast<std::size_t>(std::max(0, count)));
        if (count <= 0) return order;
        for (int step = std::max(1, count / 2);; step = std::max(1, step / 2)) {
            for (int i = 0; i < count; i += step)
                if (!taken[i]) { taken[i] = true; order.push_back(i); }
            if (step == 1) return order;
        }
    }

    // The picture to show for pointer slice `wanted`: that one if decoded, else the nearest decoded one (the earlier
    // one on a tie); -1 when none has arrived. `decoded[i]` says whether picture i is there.
    static int nearest(const std::vector<bool>& decoded, int wanted) {
        for (int distance = 0; distance < static_cast<int>(decoded.size()); ++distance) {
            if (wanted - distance >= 0 && wanted - distance < static_cast<int>(decoded.size()) && decoded[wanted - distance]) return wanted - distance;
            if (wanted + distance < static_cast<int>(decoded.size()) && wanted + distance >= 0 && decoded[wanted + distance]) return wanted + distance;
        }
        return -1;
    }

private:
    struct Request {
        std::uint64_t number = 0;
        std::filesystem::path clip;
        int count = 0;
        UINT width = 0, height = 0;
        HMONITOR monitor = nullptr;
    };
    void run();
    bool current(std::uint64_t number); // Still the newest request, and not shutting down.

    std::function<void()> ready_;
    std::mutex mutex_;
    std::condition_variable work_;
    std::optional<Request> pending_;
    std::uint64_t newest_ = 0;
    std::vector<Picture> results_;
    bool exit_ = false;
    std::thread worker_;
};

} // namespace refarter::player
