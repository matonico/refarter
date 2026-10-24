#pragma once

#include "clip_list.h"
#include "recycle_bin.h"
#include "storage_limit.h"

#include <windows.h>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace refarter {

// Lists the clip folder and makes thumbnails on one background-priority
// thread, created on first use and asleep whenever there is nothing to do.
// Windows Shell supplies thumbnails and durations; it may generate thumbnails
// when they are not cached. Refarter does not decode video on the UI thread.
class ClipLibrary {
public:
    struct Thumbnail {
        ClipFile clip;
        HBITMAP bitmap = nullptr; // Owned by the receiver; null when Windows has none.
        SIZE size{};
        std::optional<std::uint64_t> duration;
    };
    struct Results {
        std::filesystem::path folder;
        std::wstring favouritesError;
        std::optional<bool> favouriteSaved;
        std::optional<std::wstring> recycleError; // Empty on success; set on completion.
        std::size_t binned = 0; // With recycleError after bin oldest: clips moved (0 when none were needed).
        std::optional<std::vector<ClipFile>> clips; // Set when a scan finished.
        std::uint64_t driveFree = unknownBytes; // With `clips`: free bytes on the clip drive (unknown for a network drive).
        bool binAllowed = false;                // With `clips`: "bin oldest" may run on its own here (canBinAutomatically).
        std::vector<Thumbnail> thumbnails;
    };

    // `ready` runs on the worker when results arrive while none are waiting (once per batch): post to the UI, then return.
    explicit ClipLibrary(std::function<void()> ready) : ready_(std::move(ready)) {}
    ~ClipLibrary();
    ClipLibrary(const ClipLibrary&) = delete;
    ClipLibrary& operator=(const ClipLibrary&) = delete;

    void scan(std::filesystem::path folder);
    // Marks (or unmarks) every clip in one write of the favourites file.
    void favourite(ClipFile clip, bool enabled) { favourite(std::vector<ClipFile>{std::move(clip)}, enabled); }
    void favourite(std::vector<ClipFile> clips, bool enabled);
    // Remembers each clip's game (see clip_sources.h) in one write, then lists the folder with it applied.
    struct Source { ClipFile clip; std::wstring source; };
    void remember(std::vector<Source> sources);
    // One Recycle Bin operation for all the clips. Each is checked first and left alone if it changed or is gone;
    // the result says how many were left out or could not be moved.
    void recycle(ClipFile clip, HWND owner = nullptr) { recycle(std::vector<ClipFile>{std::move(clip)}, owner); }
    void recycle(std::vector<ClipFile> clips, HWND owner = nullptr); // `owner` parents any shell prompt.
    // The storage limit's "bin oldest": the worker reads the folder afresh, applies the favourites, plans with planBin
    // and moves the files with a Recycle Bin operation that aborts rather than delete for good or ask. Nothing
    // is binned unless canBinAutomatically holds. The result arrives like a recycle (empty text on success).
    struct BinRequest {
        std::filesystem::path folder;
        std::uint64_t limit = 0, estimate = 0;
        FILETIME requested{}; // When the save asked: clips created since are the save itself, not part of the folder's size.
        std::vector<std::filesystem::path> keep; // Clips in use (compressing, open).
    };
    void binOldest(BinRequest request, HWND owner);
    // Replaces queued thumbnail work: only what is on screen now is worth making.
    void want(std::vector<ClipFile> clips, SIZE size);
    Results take();

    // Lists clip files directly in `folder` (not subfolders), newest first.
    static std::vector<ClipFile> list(const std::filesystem::path& folder);

private:
    void wake();
    bool hasResults() const; // Caller holds the mutex.
    void run();

    std::function<void()> ready_;
    std::mutex mutex_;
    std::condition_variable work_;
    // favourite_, recycle_ and bin_ hold one request each (a newer one would replace an unstarted older one). That is
    // safe only because ClipGrid never sends a second until the first has answered: `recycling_` guards recycle_ and
    // bin_, `savingFavourite_` guards favourite_.
    std::optional<std::filesystem::path> scan_;
    struct FavouriteChange { std::vector<ClipFile> clips; bool enabled; };
    std::optional<FavouriteChange> favourite_;
    std::vector<Source> sources_; // Games to remember, in the order asked.
    std::optional<std::vector<ClipFile>> recycle_;
    std::optional<BinRequest> bin_;
    HWND recycleOwner_ = nullptr;
    std::vector<ClipFile> wanted_;
    SIZE size_{};
    Results results_;
    bool exit_ = false;
    std::thread worker_;
};

} // namespace refarter
