#include "clip_library.h"
#include "clip_sources.h"
#include "favourites.h"

#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <wincodec.h>
#include <winrt/base.h>
#include <algorithm>
#include <cstdlib>
#include <set>
#include <utility>
#include <unordered_map>

namespace refarter {

namespace {
std::optional<std::uint64_t> clipDuration(const std::filesystem::path& path) {
    winrt::com_ptr<IShellItem2> item;
    ULONGLONG duration = 0;
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put()))) &&
        SUCCEEDED(item->GetUInt64(PKEY_Media_Duration, &duration)) && duration)
        return duration;
    return std::nullopt;
}

// What "bin oldest" needs to know about the folder's drive. Local calls only, and a network path is never probed.
BinFacts binFacts(const std::filesystem::path& folder) {
    BinFacts facts;
    if (isNetworkPathText(folder.native()) || !folder.has_root_name()) return facts;
    const auto root = (folder.root_name() / L"\\").wstring();
    if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) return facts;
    facts.fixedDrive = true;
    wchar_t mount[MAX_PATH + 1]{};
    if (!GetVolumePathNameW(folder.c_str(), mount, static_cast<DWORD>(std::size(mount)))) return facts;
    wchar_t volume[64]{};
    facts.realVolume = GetVolumeNameForVolumeMountPointW(mount, volume, static_cast<DWORD>(std::size(volume))) != 0;
    SHQUERYRBINFO info{sizeof(info)};
    facts.recycleBinReadable = SHQueryRecycleBinW(mount, &info) == S_OK;
    return facts;
}

// Free bytes for the clip folder's drive, from its nearest existing parent; unknown for a network path or a drive
// that is not there, so a share that went away cannot hold the worker up.
std::uint64_t driveFreeBytes(const std::filesystem::path& folder) {
    if (isNetworkPathText(folder.native()) || !folder.has_root_name()) return unknownBytes;
    const auto type = GetDriveTypeW((folder.root_name() / L"\\").c_str());
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_RAMDISK) return unknownBytes;
    auto path = folder;
    std::error_code error;
    while (!path.empty() && !std::filesystem::exists(path, error)) {
        auto parent = path.parent_path();
        if (parent == path) break;
        path = std::move(parent);
    }
    ULARGE_INTEGER available{};
    return !path.empty() && GetDiskFreeSpaceExW(path.c_str(), &available, nullptr, nullptr) ? available.QuadPart : unknownBytes;
}

// Not a directory, symbolic link or mount point. Other reparse points, such as a OneDrive cloud placeholder, are
// ordinary files (dwReserved0 holds the tag only when the reparse attribute is set).
bool isPlainFile(const WIN32_FIND_DATAW& found) {
    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    return !(found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
           (found.dwReserved0 != IO_REPARSE_TAG_SYMLINK && found.dwReserved0 != IO_REPARSE_TAG_MOUNT_POINT);
}

// Bins all the clips in one operation. A clip that is gone, changed, or not a plain file is left out and
// counted; the rest are moved together through recycleOnly: to the Recycle Bin or not at all, with no
// confirmation and no error window, for a manual bin and the storage limit alike. Returns an empty text on complete success.
std::wstring recycleClips(const std::vector<ClipFile>& clips, HWND owner) {
    std::vector<std::filesystem::path> paths;
    std::size_t left = 0;
    for (const auto& clip : clips) {
        // Refuse a stale tile or a link/directory; never operate on a replacement file.
        WIN32_FIND_DATAW info{};
        const HANDLE search = FindFirstFileExW(clip.path.c_str(), FindExInfoBasic, &info, FindExSearchNameMatch, nullptr, 0);
        if (search == INVALID_HANDLE_VALUE) { ++left; continue; }
        FindClose(search);
        ClipFile current{clip.path, info.ftLastWriteTime,
            (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow};
        if (!sameClip(clip, current) || !isPlainFile(info)) { ++left; continue; }
        paths.push_back(clip.path);
    }
    const bool single = clips.size() == 1;
    std::wstring problem;
    if (left) {
        problem = single ? L"clip has changed or is no longer available · refresh the library."
                         : std::to_wstring(left) + L" of " + std::to_wstring(clips.size()) + L" clips changed or are gone and were left alone · refresh the library.";
    }
    if (paths.empty()) return problem;
    if (const auto failed = recycleOnly(paths, owner)) {
        return single ? L"clip could not be moved to the recycle bin · it may be in use."
                      : std::to_wstring(failed) + (failed == 1 ? L" clip" : L" clips") + L" could not be moved to the recycle bin · they may be in use." +
                        (left ? L" " + problem : std::wstring{});
    }
    return problem;
}

// "Bin oldest" on the worker, so no disk work reaches the window thread. Everything is read afresh: the folder (not
// the grid's older list) with the favourites applied. Nothing is binned unless the drive is one where the Recycle Bin
// is certain; favourites that cannot be read mean nothing is binned. Returns an empty text on success.
std::wstring binOldestClips(const ClipLibrary::BinRequest& request, HWND owner, const std::filesystem::path& favouritesFile, std::size_t& binned) {
    if (!canBinAutomatically(binFacts(request.folder))) return L"no recycle bin here";
    const auto favourites = readFavourites(favouritesFile);
    if (!favourites) return L"favourites could not be read";
    auto clips = ClipLibrary::list(request.folder);
    // A clip created since the request is the save the room is for (it may already be renamed into the folder): it
    // is neither counted as used nor a candidate, or its size would be added twice with the estimate.
    if (request.requested.dwHighDateTime || request.requested.dwLowDateTime)
        clips.erase(std::remove_if(clips.begin(), clips.end(), [&](const ClipFile& clip) {
            const FILETIME& born = clip.created.dwHighDateTime || clip.created.dwLowDateTime ? clip.created : clip.written;
            return CompareFileTime(&born, &request.requested) >= 0;
        }), clips.end());
    for (auto& clip : clips) clip.favourite = favourites->contains(favouriteKey(clip));
    const auto used = clipBytes(clips);
    const auto plan = planBin(std::move(clips), request.keep, request.folder, used, request.limit, request.estimate);
    if (!plan.reachable) return L"old clips cannot make enough room";
    if (plan.bin.empty()) return {};
    auto problem = recycleClips(plan.bin, owner);
    if (problem.empty()) binned = plan.bin.size();
    return problem;
}

// A thumbnail that fits `frame`, or null when Windows has none (thumbnail
// only: no file-type icons). Takes Windows' 768-pixel cached size (1280 for
// large tiles) and shrinks it with WIC's high-quality cubic filter, which is
// sharper than Windows' own small sizes and faster to fetch when Explorer
// already cached the large one. Only the small result is kept.
HBITMAP makeThumbnail(IWICImagingFactory* wic, const std::filesystem::path& path, SIZE frame, SIZE& size) {
    winrt::com_ptr<IShellItemImageFactory> factory;
    HBITMAP large = nullptr;
    const LONG sourceWidth = frame.cx * 2 <= 768 ? 768 : 1280;
    if (!wic || FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(factory.put()))) ||
        FAILED(factory->GetImage({sourceWidth, sourceWidth * 9 / 16}, SIIGBF_RESIZETOFIT | SIIGBF_THUMBNAILONLY, &large))) {
        return nullptr;
    }
    HBITMAP scaled = nullptr;
    winrt::com_ptr<IWICBitmap> source;
    winrt::com_ptr<IWICBitmapScaler> scaler;
    UINT width = 0, height = 0;
    if (SUCCEEDED(wic->CreateBitmapFromHBITMAP(large, nullptr, WICBitmapIgnoreAlpha, source.put())) &&
        SUCCEEDED(source->GetSize(&width, &height)) && width && height &&
        SUCCEEDED(wic->CreateBitmapScaler(scaler.put()))) {
        const double fit = std::min({1.0, static_cast<double>(frame.cx) / width, static_cast<double>(frame.cy) / height});
        size = {std::max(1L, static_cast<LONG>(width * fit + 0.5)), std::max(1L, static_cast<LONG>(height * fit + 0.5))};
        BITMAPINFO format{};
        format.bmiHeader = {sizeof(BITMAPINFOHEADER), size.cx, -size.cy, 1, 32, BI_RGB};
        void* bits = nullptr;
        scaled = CreateDIBSection(nullptr, &format, DIB_RGB_COLORS, &bits, nullptr, 0);
        const UINT stride = static_cast<UINT>(size.cx) * 4;
        if (scaled && (FAILED(scaler->Initialize(source.get(), size.cx, size.cy, WICBitmapInterpolationModeHighQualityCubic)) ||
                       FAILED(scaler->CopyPixels(nullptr, stride, stride * size.cy, static_cast<BYTE*>(bits))))) {
            DeleteObject(scaled);
            scaled = nullptr;
        } else if (scaled) {
            // WIC leaves the fourth byte of BGR pixels undefined; make it opaque so
            // nothing that reads 32-bit pixels treats the thumbnail as transparent.
            auto* pixel = static_cast<BYTE*>(bits);
            for (LONG i = 0; i < size.cx * size.cy; ++i) pixel[i * 4 + 3] = 0xFF;
        }
    }
    DeleteObject(large);
    return scaled;
}
} // namespace

ClipLibrary::~ClipLibrary() {
    {
        std::lock_guard lock(mutex_);
        exit_ = true;
    }
    work_.notify_one();
    if (worker_.joinable()) worker_.join();
    for (auto& thumbnail : results_.thumbnails) if (thumbnail.bitmap) DeleteObject(thumbnail.bitmap);
}

void ClipLibrary::wake() {
    if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
    work_.notify_one();
}

void ClipLibrary::scan(std::filesystem::path folder) {
    std::lock_guard lock(mutex_);
    scan_ = std::move(folder);
    wake();
}

void ClipLibrary::favourite(std::vector<ClipFile> clips, bool enabled) {
    std::lock_guard lock(mutex_);
    favourite_ = FavouriteChange{std::move(clips), enabled};
    wake();
}

void ClipLibrary::remember(std::vector<Source> sources) {
    std::lock_guard lock(mutex_);
    sources_.insert(sources_.end(), std::make_move_iterator(sources.begin()), std::make_move_iterator(sources.end()));
    wake();
}

void ClipLibrary::recycle(std::vector<ClipFile> clips, HWND owner) {
    std::lock_guard lock(mutex_);
    recycle_ = std::move(clips);
    recycleOwner_ = owner;
    wake();
}

void ClipLibrary::binOldest(BinRequest request, HWND owner) {
    std::lock_guard lock(mutex_);
    bin_ = std::move(request);
    recycleOwner_ = owner;
    wake();
}

void ClipLibrary::want(std::vector<ClipFile> paths, SIZE size) {
    std::lock_guard lock(mutex_);
    wanted_ = std::move(paths);
    size_ = size;
    if (!wanted_.empty()) wake();
}

bool ClipLibrary::hasResults() const {
    return results_.clips || results_.favouriteSaved || results_.recycleError || !results_.thumbnails.empty();
}

ClipLibrary::Results ClipLibrary::take() {
    std::lock_guard lock(mutex_);
    return std::exchange(results_, {});
}

std::vector<ClipFile> ClipLibrary::list(const std::filesystem::path& folder) {
    std::vector<ClipFile> clips;
    WIN32_FIND_DATAW found{};
    // Basic info and large fetches: one directory read gives names, sizes and times.
    const HANDLE search = FindFirstFileExW((folder / L"*").c_str(), FindExInfoBasic, &found,
                                           FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (search == INVALID_HANDLE_VALUE) return clips;
    do {
        if (found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) continue;
        std::filesystem::path path = folder / found.cFileName;
        if (!isClipFile(path)) continue;
        clips.push_back({std::move(path), found.ftLastWriteTime,
                         (static_cast<std::uint64_t>(found.nFileSizeHigh) << 32) | found.nFileSizeLow, false, found.ftCreationTime});
    } while (FindNextFileW(search, &found));
    FindClose(search);
    sortNewestFirst(clips);
    return clips;
}

void ClipLibrary::run() {
    // Below-normal CPU and I/O priority reduces contention with a game or a save.
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    winrt::com_ptr<IWICImagingFactory> wic;
    std::filesystem::path currentFolder;
    const auto favouritesFile = favouritesPath();
    const auto sourcesFile = clipSourcesPath();
    std::unordered_map<std::wstring, std::optional<std::uint64_t>> durations;
    for (;;) {
        std::optional<std::filesystem::path> folder;
        ClipFile wanted;
        std::optional<FavouriteChange> change;
        std::optional<std::vector<ClipFile>> recycle;
        std::optional<BinRequest> bin;
        std::vector<Source> remembered;
        HWND owner = nullptr;
        SIZE size{};
        bool notify = false;
        {
            std::unique_lock lock(mutex_);
            work_.wait(lock, [this] { return exit_ || scan_ || favourite_ || !sources_.empty() || recycle_ || bin_ || !wanted_.empty(); });
            if (exit_ && !favourite_ && sources_.empty() && !recycle_) break; // Finish accepted user operations before teardown.
            if (recycle_) { recycle = std::exchange(recycle_, std::nullopt); owner = recycleOwner_; }
            else if (bin_) {
                bin = std::exchange(bin_, std::nullopt);
                owner = recycleOwner_;
                change = std::exchange(favourite_, std::nullopt); // A star placed just before must protect its clip from this bin.
            }
            else if (exit_ && favourite_) change = std::exchange(favourite_, std::nullopt);
            else if (scan_) folder = std::exchange(scan_, std::nullopt);
            else if (favourite_) change = std::exchange(favourite_, std::nullopt);
            else if (sources_.empty()) {
                wanted = std::move(wanted_.front());
                wanted_.erase(wanted_.begin());
                size = size_;
            }
            // Games to remember go with whatever lists the folder next (on their own when nothing else does).
            if (wanted.path.empty()) remembered = std::exchange(sources_, {});
        }
        if (folder || change || recycle || bin || !remembered.empty()) {
            if (folder) currentFolder = *folder;
            auto favourites = readFavourites(favouritesFile);
            std::wstring failure;
            std::optional<bool> saved;
            if (!favourites) failure = L"favourites could not be read · marks are unchanged.";
            if (change) {
                saved = false;
                if (favourites) {
                    for (const auto& marked : change->clips) {
                        const auto identity = favouriteKey(marked);
                        if (change->enabled) favourites->insert(identity);
                        else favourites->erase(identity);
                    }
                    saved = writeFavourites(favouritesFile, *favourites);
                    if (!*saved) {
                        failure = L"favourite could not be saved · try again.";
                        favourites = readFavourites(favouritesFile);
                    }
                }
            }
            std::optional<std::wstring> recycleError;
            std::size_t binned = 0;
            if (recycle) recycleError = recycleClips(*recycle, owner);
            else if (bin) recycleError = binOldestClips(*bin, owner, favouritesFile, binned);
            auto clips = list(currentFolder);
            // Drive facts are read here, off the window thread, on every scan.
            const auto driveFree = driveFreeBytes(currentFolder);
            const bool binAllowed = canBinAutomatically(binFacts(currentFolder)); // Cheap local calls; a network path is never probed.
            // Keep attempted durations (including unavailable properties) only for
            // unchanged files, so resize/filter/rescan does not reopen each video.
            std::unordered_map<std::wstring, std::optional<std::uint64_t>> retained;
            for (const auto& clip : clips) {
                const auto key = favouriteKey(clip);
                if (const auto cached = durations.find(key); cached != durations.end()) retained.emplace(*cached);
            }
            durations.swap(retained);
            if (favourites) for (auto& clip : clips) clip.favourite = favourites->contains(favouriteKey(clip));
            // Remembered games: an unreadable file is left as it is and the folders fall back to the names.
            if (auto sources = readClipSources(sourcesFile)) {
                if (!remembered.empty()) {
                    for (const auto& [clip, source] : remembered) (*sources)[sourceKey(clip)] = source;
                    // Entries for this folder's clips that are gone are dropped as the file is written.
                    const auto here = sourceFolder(currentFolder);
                    std::set<SourceKey> listed;
                    for (const auto& clip : clips) listed.insert(sourceKey(clip));
                    std::erase_if(*sources, [&](const auto& entry) { return entry.first.folder == here && !listed.contains(entry.first); });
                    writeClipSources(sourcesFile, *sources);
                }
                for (auto& clip : clips)
                    if (const auto found = sources->find(sourceKey(clip)); found != sources->end()) clip.source = found->second;
            }
            std::lock_guard lock(mutex_);
            notify = !hasResults();
            results_.folder = currentFolder;
            results_.favouritesError = std::move(failure);
            if (saved) results_.favouriteSaved = saved;
            if (recycleError) { results_.recycleError = std::move(recycleError); results_.binned = binned; }
            results_.clips = std::move(clips);
            results_.driveFree = driveFree;
            results_.binAllowed = binAllowed;
        } else {
            Thumbnail thumbnail{wanted};
            const auto key = favouriteKey(wanted);
            auto cached = durations.find(key);
            if (cached == durations.end()) cached = durations.emplace(key, clipDuration(wanted.path)).first;
            thumbnail.duration = cached->second;
            // Created on first use and kept for the thread's lifetime.
            if (!wic) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()));
            thumbnail.bitmap = makeThumbnail(wic.get(), wanted.path, size, thumbnail.size);
            std::lock_guard lock(mutex_);
            notify = !hasResults();
            results_.thumbnails.push_back(thumbnail);
        }
        // One wake-up per batch: results already waiting have one posted, and take() empties them.
        if (notify && ready_) ready_();
    }
    wic = nullptr; // Released before COM shuts down on this thread.
    if (SUCCEEDED(com)) CoUninitialize();
}

} // namespace refarter
