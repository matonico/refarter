#include "clip_grid.h"
#include "message_box.h"
#include "theme.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <winrt/base.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <cmath>

namespace refarter {

namespace {
using namespace theme;
constexpr UINT libraryReady = WM_APP + 3;
constexpr wchar_t className[] = L"RefarterClipGrid";
constexpr UINT_PTR noteTimer = 1, bandTimer = 2;
bool keyDown(int key) { return (GetKeyState(key) & 0x8000) != 0; }

// A copy of a thumbnail `width` pixels wide as an opaque 32-bit bitmap, the form the shell's drag image takes
// (GDI leaves the alpha byte at zero, which would make it see-through). Null without a thumbnail.
HBITMAP dragBitmap(HBITMAP thumbnail, SIZE source, int width, SIZE& size) {
    if (!thumbnail || source.cx <= 0 || source.cy <= 0 || width <= 0) return nullptr;
    size = {width, std::max(1, MulDiv(width, source.cy, source.cx))};
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), size.cx, -size.cy, 1, 32, BI_RGB};
    void* bits = nullptr;
    const HBITMAP image = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!image) return nullptr;
    const HDC from = CreateCompatibleDC(nullptr), to = CreateCompatibleDC(nullptr);
    const auto oldFrom = SelectObject(from, thumbnail), oldTo = SelectObject(to, image);
    SetStretchBltMode(to, HALFTONE);
    SetBrushOrgEx(to, 0, 0, nullptr);
    StretchBlt(to, 0, 0, size.cx, size.cy, from, 0, 0, source.cx, source.cy, SRCCOPY);
    SelectObject(from, oldFrom);
    SelectObject(to, oldTo);
    DeleteDC(from);
    DeleteDC(to);
    GdiFlush();
    auto* pixel = static_cast<std::uint32_t*>(bits);
    for (LONG i = 0; i < size.cx * size.cy; ++i) pixel[i] |= 0xff000000u;
    return image;
}
} // namespace

ClipGrid::~ClipGrid() {
    library_.reset(); // Joins the worker before the bitmaps it hands over are freed.
    clearThumbnails();
    if (editBrush_) DeleteObject(editBrush_);
    if (ole_) OleUninitialize();
}

HWND ClipGrid::create(HWND parent, int id) {
    if (!registerWindowClass(className, proc, CS_DBLCLKS)) return nullptr;
    parent_ = parent;
    id_ = id;
    window_ = CreateWindowExW(0, className, L"clips", WS_CHILD | WS_TABSTOP | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), this);
    // Dark scroll bar to match the window; ignored where the theme is unavailable.
    if (window_) SetWindowTheme(window_, L"DarkMode_Explorer", nullptr);
    editBrush_ = CreateSolidBrush(background);
    library_ = std::make_unique<ClipLibrary>([window = window_] { PostMessageW(window, libraryReady, 0, 0); });
    return window_;
}

void ClipGrid::setStyle(UINT dpi, HFONT name, HFONT date, bool smallTiles) {
    dpi_ = dpi;
    nameFont_ = name;
    dateFont_ = date;
    smallTiles_ = smallTiles;
    updateLabels();
    measure();
}

// Texts and their widths for every listed clip; the fonts or the list changed.
void ClipGrid::updateLabels() {
    labels_.assign(items(), {});
    if (!window_) return;
    if (const HDC dc = GetDC(window_)) {
        const auto old = SelectObject(dc, dateFont_);
        const wchar_t tag[] = L"compressed";
        GetTextExtentPoint32W(dc, tag, static_cast<int>(std::size(tag)) - 1, &compressedTag_);
        SIZE count{}, name{};
        GetTextExtentPoint32W(dc, backCount_.c_str(), static_cast<int>(backCount_.size()), &count);
        SelectObject(dc, nameFont_);
        GetTextExtentPoint32W(dc, backLabel_.c_str(), static_cast<int>(backLabel_.size()), &name);
        backNameWidth_ = name.cx;
        backWidth_ = name.cx + px(10) + count.cx;
        SelectObject(dc, old);
        for (std::size_t i = 0; i < labels_.size(); ++i) updateLabel(dc, i);
        ReleaseDC(window_, dc);
    }
}

void ClipGrid::updateLabel(HDC dc, std::size_t index) {
    if (index >= static_cast<std::size_t>(items()) || index >= labels_.size()) return;
    const auto width = [&](HFONT font, const std::wstring& text) {
        SIZE extent{};
        const auto old = SelectObject(dc, font);
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent);
        SelectObject(dc, old);
        return static_cast<int>(extent.cx);
    };
    auto& label = labels_[index];
    if (overview()) {
        // A folder: its game, and its clip count where a clip shows its size.
        const auto& folder = folders_[index];
        label = {folder.name, folder.count == 1 ? L"1 clip" : std::to_wstring(folder.count) + L" clips"};
        label.nameWidth = width(nameFont_, label.name);
        label.sizeWidth = width(dateFont_, label.size);
        return;
    }
    label.name = clipDisplayName(clips_[index].path);
    label.nameWidth = width(nameFont_, label.name);
    label.size = formatClipBytes(clips_[index].bytes);
    label.sizeWidth = width(dateFont_, label.size);
    label.duration.clear();
    label.durationWidth = 0;
    if (const auto detail = details_.find(clips_[index].path.wstring()); detail != details_.end() && detail->second.duration) {
        label.duration = formatClipDuration(*detail->second.duration);
        label.durationWidth = width(dateFont_, label.duration);
    }
}

void ClipGrid::invalidateTile(int index) const {
    if (!window_ || index < 0 || index >= items() || tileWidth_ <= 0) return;
    RECT area = tileRect(index);
    InflateRect(&area, pad_, pad_); // The hover halo and the focus glow reach past the tile.
    InvalidateRect(window_, &area, FALSE);
}

void ClipGrid::invalidatePath(const std::filesystem::path& path) const {
    if (path.empty()) return;
    if (const auto found = index_.find(path.wstring()); found != index_.end()) invalidateTile(static_cast<int>(found->second));
}

ClipGrid::Parts ClipGrid::parts(int index, POINT point) const {
    Parts result;
    if (overview()) return result; // A folder tile is one button.
    const auto star = favouriteRect(index), dots = moreRect(index), trash = binRect(index);
    result.star = PtInRect(&star, point) != FALSE;
    result.more = PtInRect(&dots, point) != FALSE;
    result.bin = PtInRect(&trash, point) != FALSE;
    result.name = point.x < dots.left && point.y >= dots.top && point.y < dots.bottom;
    result.pencil = pencilAt(index, point, result.name);
    return result;
}

void ClipGrid::scan(const std::filesystem::path& folder) {
    folder_ = folder;
    lastScan_ = std::chrono::steady_clock::now();
    if (library_ && !folder.empty()) library_->scan(folder);
}

void ClipGrid::setView(ClipView view) {
    if (view_ == view) return;
    view_ = view;
    scroll_ = 0;
    updateList(); // The open folder stays open when this tab has clips in it.
}

void ClipGrid::setSearch(const std::wstring& text) {
    const auto first = text.find_first_not_of(L" \t");
    const auto trimmed = first == std::wstring::npos ? std::wstring{} : text.substr(first, text.find_last_not_of(L" \t") - first + 1);
    if (trimmed == search_) return;
    search_ = trimmed;
    pendingFocus_.clear(); // A clip renamed out of the search must not take the place when the search is cleared later.
    if (!search_.empty()) {
        scroll_ = 0;
        focus_ = 0; // The first match takes the keyboard place.
    } // Cleared: the focused clip keeps its place (updateList finds it again by path).
    if (scanned_) updateList();
}

void ClipGrid::setGrouped(bool on) {
    if (grouped_ == on) return;
    grouped_ = on;
    if (!scanned_) return; // At startup: the first scan lists the folders.
    openFolder_.clear();
    scroll_ = 0;
    focus_ = 0;
    updateList();
}

// A remembered game wins: it was the game when the clip was renamed, whatever the new name says.
std::wstring ClipGrid::sourceOf(const ClipFile& clip) const {
    return clip.source.empty() ? clipSourceFromName(clip.path) : clip.source;
}

void ClipGrid::enterFolder(int index) {
    if (!overview() || index < 0 || index >= static_cast<int>(folders_.size())) return;
    openFolder_ = folders_[index].key;
    openFolderName_ = folders_[index].name;
    clickedClip_.clear();
    scroll_ = 0;
    focus_ = 0;
    selection_.clear();
    updateList();
}

bool ClipGrid::leaveFolder() {
    if (!inFolder()) return false;
    const auto left = std::exchange(openFolder_, {});
    scroll_ = 0;
    backHover_ = pressedBack_ = false;
    clickedClip_.clear();
    updateList();
    // The keyboard place goes back to the folder just left.
    const auto found = std::find_if(folders_.begin(), folders_.end(), [&](const Folder& folder) { return folder.key == left; });
    if (found != folders_.end()) focusTile(static_cast<int>(found - folders_.begin()));
    return true;
}

int ClipGrid::contentTop() const { return pad_ + (inFolder() ? px(44) : 0); }

RECT ClipGrid::backRect() const {
    const int top = pad_ - scroll_;
    return {pad_, top, pad_ + px(36) + backWidth_, top + px(32)};
}

void ClipGrid::inheritSource(const std::filesystem::path& clip, const std::filesystem::path& from) {
    const auto source = std::find_if(allClips_.begin(), allClips_.end(), [&](const ClipFile& c) { return c.path == from; });
    const auto game = source != allClips_.end() ? sourceOf(*source) : clipSourceFromName(from);
    if (!clip.empty() && !game.empty()) pendingSources_.push_back({clip, game});
}

void ClipGrid::toggleFavourite(int index) {
    if (savingFavourite_ || index < 0 || index >= static_cast<int>(clips_.size())) return;
    savingFavourite_ = true;
    library_->favourite(clips_[index], !clips_[index].favourite);
    invalidateTile(index);
}

RECT ClipGrid::favouriteRect(int index) const {
    const auto tile = tileRect(index);
    return {tile.left + px(6), tile.top + px(6), tile.left + px(36), tile.top + px(36)};
}

RECT ClipGrid::binRect(int index) const {
    const auto tile = tileRect(index);
    return {tile.right - px(36), tile.top + px(6), tile.right - px(6), tile.top + px(36)};
}

bool ClipGrid::selectedAreFavourites() const {
    const auto selected = actionClips();
    return !selected.empty() && std::all_of(selected.begin(), selected.end(), [](const ClipFile& clip) { return clip.favourite; });
}

// The selection, or the focused clip when nothing is selected (as Delete, Ctrl+C and the bin always did).
std::vector<ClipFile> ClipGrid::actionClips() const {
    auto selected = selection_.clips(clips_);
    if (selected.empty() && focus_ >= 0 && focus_ < static_cast<int>(clips_.size())) selected.push_back(clips_[focus_]);
    return selected;
}

// Esc during a rectangle drag: stop it and go back to the selection from before the drag.
bool ClipGrid::cancelBand() {
    if (!band_) return false;
    endBand();
    ReleaseCapture();
    selection_ = bandBase_;
    pressed_ = -1;
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

bool ClipGrid::clearSelection() {
    if (selection_.empty()) return false;
    selection_.clear();
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

// Bins the selected clips of this tab. Clips being compressed or open in the player are left out (said in the
// question, or in the note when the question is switched off); the answer is asked once, and nothing outside the
// selection is touched. A ticked "don't ask again" counts only with a yes.
void ClipGrid::recycleSelection() {
    if (recycling_) return;
    auto plan = planRecycle(actionClips(), busyClips());
    if (plan.bin.empty()) {
        if (!plan.skipped.empty())
            messageBox(parent_, plan.skipped.size() == 1 ? L"finish or cancel compression before binning this clip."
                                                         : L"finish or cancel compression before binning these clips.",
                       L"refarter", MB_OK | MB_ICONINFORMATION);
        return;
    }
    // A modal prompt can dispatch scan results: the question works on copies of what was selected.
    if (askBeforeBinning_) {
        const auto question = recycleQuestion(plan.bin, plan.skipped.size());
        bool dontAsk = false;
        if (messageBox(parent_, question.c_str(), L"refarter", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2, L"don't ask again", &dontAsk) != IDYES) return;
        if (dontAsk) {
            askBeforeBinning_ = false;
            SendMessageW(parent_, WM_COMMAND, MAKEWPARAM(id_, askChanged), reinterpret_cast<LPARAM>(window_));
        }
    } else if (!plan.skipped.empty()) {
        showNote(skippedNote(plan.skipped.size())); // No dialog to say it.
    }
    if (recycling_) return;
    plan.bin.erase(std::remove_if(plan.bin.begin(), plan.bin.end(), [&](const ClipFile& clip) {
        const auto busy = busyClips();
        return std::find(busy.begin(), busy.end(), clip.path) != busy.end();
    }), plan.bin.end());
    if (plan.bin.empty()) return;
    recycling_ = true;
    clickedClip_.clear();
    library_->recycle(std::move(plan.bin), parent_); // The worker checks each file again before it moves it.
    InvalidateRect(window_, nullptr, FALSE);
}

bool ClipGrid::binOldest(std::uint64_t limit, std::uint64_t estimate) {
    if (recycling_ || !limit) return false;
    recycling_ = automaticRecycle_ = true;
    ClipLibrary::BinRequest request{folder_, limit, estimate, {}, {}};
    GetSystemTimeAsFileTime(&request.requested);
    for (const auto& busy : busyClips()) if (!busy.empty()) request.keep.push_back(busy);
    library_->binOldest(std::move(request), parent_); // Favourites are read again by the worker.
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

void ClipGrid::favouriteSelection(std::optional<bool> add) {
    if (savingFavourite_) return;
    const auto selected = actionClips();
    if (selected.empty()) return;
    savingFavourite_ = true;
    library_->favourite(selected, add.value_or(!selectedAreFavourites()));
    InvalidateRect(window_, nullptr, FALSE);
}

bool ClipGrid::copySelection() {
    std::vector<std::filesystem::path> paths;
    for (const auto& clip : actionClips()) {
        std::error_code error;
        if (std::filesystem::exists(clip.path, error)) paths.push_back(clip.path); // Not one that has vanished.
    }
    if (paths.empty()) return false;
    const bool copied = copyFilesToClipboard(window_, paths);
    showNote(copied ? copiedNote(paths.size()) : L"could not copy \u00b7 the clipboard is busy.");
    return copied;
}

void ClipGrid::showNote(std::wstring text) {
    note_ = std::move(text);
    SetTimer(window_, noteTimer, 3000, nullptr); // One shot: cleared below, so nothing runs at idle.
    SendMessageW(parent_, WM_COMMAND, MAKEWPARAM(id_, noteChanged), reinterpret_cast<LPARAM>(window_));
}

// The rectangle dragged from empty space selects every tile it touches (plus the earlier selection when
// Ctrl was held at the start). Near the top or bottom edge a timer scrolls while the button is down.
void ClipGrid::updateBand() {
    RECT client{};
    GetClientRect(window_, &client);
    const POINT now{bandPoint_.x, bandPoint_.y + scroll_};
    const RECT area{std::min(bandStart_.x, now.x), std::min(bandStart_.y, now.y), std::max(bandStart_.x, now.x), std::max(bandStart_.y, now.y)};
    selection_ = bandBase_;
    for (int i = 0; i < static_cast<int>(clips_.size()); ++i) {
        RECT tile = tileRect(i), hit{};
        OffsetRect(&tile, 0, scroll_); // Content coordinates.
        if (IntersectRect(&hit, &tile, &area)) selection_.include(clips_, i);
    }
    const bool outside = bandPoint_.y < 0 || bandPoint_.y > client.bottom;
    if (outside && !bandTimerOn_) { SetTimer(window_, bandTimer, 40, nullptr); bandTimerOn_ = true; } // Not re-armed per move.
    else if (!outside && bandTimerOn_) { KillTimer(window_, bandTimer); bandTimerOn_ = false; }
    InvalidateRect(window_, nullptr, FALSE);
}

// Dragging a clip out of the grid gives the drop target the files themselves, as Explorer would. Only copying is
// offered, so a drop never moves a clip out of the folder. The pressed clip is dragged with the rest of the
// selection. When it is not part of it, it joins the selection as a click would (Ctrl adds it, Shift selects a
// range) and is otherwise selected alone.
void ClipGrid::startDrag(int index) {
    dragReady_ = false;
    ReleaseCapture(); // The drag captures the mouse itself, and no button-up reaches the grid afterwards.
    if (index < 0 || index >= static_cast<int>(clips_.size())) return;
    if (!selection_.contains(clips_[index].path)) {
        const bool ctrl = keyDown(VK_CONTROL), shift = keyDown(VK_SHIFT);
        if (shift) selection_.range(clips_, index, focusBeforePress_, ctrl);
        else if (ctrl) selection_.toggle(clips_, index);
        else selection_.select(clips_, index);
    }
    clickedClip_.clear();
    InvalidateRect(window_, nullptr, FALSE);
    std::vector<std::filesystem::path> paths;
    for (const auto& clip : selection_.clips(clips_)) paths.push_back(clip.path); // A vanished file does not resolve and is left out.
    // Drag and drop needs OLE on this thread, not only COM; it stays set up once a drag has needed it.
    if (!ole_) ole_ = SUCCEEDED(OleInitialize(nullptr));
    winrt::com_ptr<IDataObject> data;
    data.attach(filesDataObject(paths));
    if (!ole_ || !data) { showNote(L"could not drag these clips"); return; }
    winrt::com_ptr<IDragSourceHelper2> helper;
    if (SUCCEEDED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(helper.put())))) {
        helper->SetFlags(DSH_ALLOWDROPDESCRIPTIONTEXT); // "copy to Desktop" under the pointer, as Explorer shows.
        SHDRAGIMAGE image{};
        if (const auto cached = thumbnails_.find(clips_[index].path.wstring()); cached != thumbnails_.end())
            image.hbmpDragImage = dragBitmap(cached->second.bitmap, cached->second.size, px(160), image.sizeDragImage);
        image.ptOffset = {image.sizeDragImage.cx / 2, image.sizeDragImage.cy / 2};
        image.crColorKey = CLR_NONE;
        // Without a thumbnail the shell draws its own image of the files.
        if (!image.hbmpDragImage || FAILED(helper->InitializeFromBitmap(&image, data.get()))) {
            if (image.hbmpDragImage) DeleteObject(image.hbmpDragImage);
            POINT cursor{};
            GetCursorPos(&cursor);
            helper->InitializeFromWindow(nullptr, &cursor, data.get());
        }
    }
    DWORD effect = DROPEFFECT_NONE;
    SHDoDragDrop(window_, data.get(), nullptr, DROPEFFECT_COPY, &effect);
}

void ClipGrid::endBand() {
    KillTimer(window_, bandTimer);
    bandTimerOn_ = false;
    band_ = false;
    InvalidateRect(window_, nullptr, FALSE);
}

void ClipGrid::compression(const std::filesystem::path& path, float progress, bool cancelling) {
    const int percent = std::clamp(static_cast<int>(progress * 100), 0, 99);
    if (exporting_ == path && exportPercent_ == percent && exportCancelling_ == cancelling) return;
    const auto previous = std::exchange(exporting_, path);
    exportPercent_ = percent;
    exportCancelling_ = cancelling;
    if (!window_) return;
    invalidatePath(previous);
    invalidatePath(path);
}

void ClipGrid::clearThumbnails() {
    for (auto& [path, thumbnail] : thumbnails_) DeleteObject(thumbnail.bitmap);
    thumbnails_.clear();
}

void ClipGrid::receive() {
    auto results = library_->take();
    // Anything but thumbnails can change every tile; thumbnails repaint only their own.
    bool everything = results.favouriteSaved || results.recycleError || (results.clips && results.folder == folder_);
    if (results.favouriteSaved) savingFavourite_ = false;
    const bool automaticBin = results.recycleError && automaticRecycle_;
    if (results.recycleError) {
        recycling_ = false;
        automaticRecycle_ = false;
    }
    if (results.clips && results.folder == folder_) {
        allClips_ = std::move(*results.clips);
        bytes_ = clipBytes(allClips_);
        driveFree_ = results.driveFree;
        binAllowed_ = results.binAllowed;
        favouritesError_ = std::move(results.favouritesError);
        scanned_ = true;
        updateList();
        // A clip replaced by its trimmed copy keeps its star (one save at a time).
        for (auto it = refavourite_.begin(); it != refavourite_.end() && !savingFavourite_;) {
            const auto clip = std::find_if(allClips_.begin(), allClips_.end(), [&](const auto& c) { return c.path == *it; });
            if (clip != allClips_.end() && clip->favourite) { ++it; continue; } // Still the old file.
            if (clip != allClips_.end()) {
                savingFavourite_ = true;
                library_->favourite(*clip, true);
            }
            it = refavourite_.erase(it);
        }
        // A renamed favourite is a new path; wait for the scan that lists it.
        for (auto it = renamedFavourites_.begin(); it != renamedFavourites_.end() && !savingFavourite_;) {
            const auto clip = std::find_if(allClips_.begin(), allClips_.end(), [&](const auto& c) { return c.path == *it; });
            if (clip == allClips_.end()) { ++it; continue; }
            if (!clip->favourite) {
                savingFavourite_ = true;
                library_->favourite(*clip, true);
            }
            it = renamedFavourites_.erase(it);
        }
        // A trim or compressed copy of a clip whose game only Refarter remembers: remember it for the new file too.
        std::vector<ClipLibrary::Source> remember;
        for (auto it = pendingSources_.begin(); it != pendingSources_.end();) {
            const auto clip = std::find_if(allClips_.begin(), allClips_.end(), [&](const auto& c) { return c.path == it->clip; });
            if (clip == allClips_.end() && --it->scans > 0) { ++it; continue; } // Not listed yet.
            if (clip != allClips_.end() && sourceOf(*clip) != it->source) remember.push_back({*clip, it->source});
            it = pendingSources_.erase(it);
        }
        if (!remember.empty()) library_->remember(std::move(remember));
    }
    std::vector<std::size_t> changed;
    for (auto& thumbnail : results.thumbnails) {
        const auto key = thumbnail.clip.path.wstring();
        const auto found = index_.find(key);
        if (found == index_.end() || !showsClip(static_cast<int>(found->second), thumbnail.clip)) {
            if (thumbnail.bitmap) DeleteObject(thumbnail.bitmap);
            continue;
        }
        details_[key] = {thumbnail.clip, thumbnail.duration};
        changed.push_back(found->second);
        if (!thumbnail.bitmap) { missing_.insert(key); continue; }
        if (auto old = thumbnails_.find(key); old != thumbnails_.end()) DeleteObject(old->second.bitmap);
        thumbnails_[key] = {thumbnail.bitmap, thumbnail.size, thumbnail.clip};
    }
    if (!changed.empty()) {
        if (const HDC dc = GetDC(window_)) {
            for (const auto i : changed) updateLabel(dc, i);
            ReleaseDC(window_, dc);
        }
    }
    if (everything) InvalidateRect(window_, nullptr, FALSE);
    else for (const auto i : changed) invalidateTile(static_cast<int>(i));
    // Apply this batch before a modal error dialog can dispatch newer scan results.
    if (automaticBin) {
        // A save may be running over the game: say it on the strip, never in a dialog.
        if (const auto note = binNote(*results.recycleError, results.binned); !note.empty()) showNote(note);
    } else if (results.recycleError && !results.recycleError->empty())
        messageBox(parent_, results.recycleError->c_str(), L"refarter", MB_OK | MB_ICONWARNING);
}

void ClipGrid::updateList() {
    const int oldFocus = focus_;
    const bool wasOverview = overview();
    const auto focused = focus_ < items() ? tileClip(focus_).path : std::filesystem::path{};
    const auto focusedFolder = wasOverview && focus_ < items() ? folders_[focus_].key : std::wstring{};
    std::vector<ClipFile> shown;
    for (const auto& clip : allClips_) {
        if (!search_.empty() && !matchesSearch(clipDisplayName(clip.path), sourceOf(clip), search_)) continue;
        const bool compressed = isCompressedCopy(clip.path);
        if (view_ == ClipView::Compressed ? compressed : view_ == ClipView::All || (!compressed && clip.favourite)) shown.push_back(clip);
    }
    viewCount_ = shown.size();
    folders_.clear();
    clips_.clear();
    if (grouped_) {
        // Folders in the order of their newest clip; the open one lists its clips.
        std::unordered_map<std::wstring, std::size_t> at;
        for (const auto& clip : shown) {
            const auto game = sourceOf(clip);
            auto key = game.empty() ? std::wstring(L"*") : lower(game);
            const auto [place, added] = at.emplace(key, folders_.size());
            if (added) folders_.push_back({key, game.empty() ? L"other" : game, {clip}});
            else if (folders_[place->second].covers.size() < 4) folders_[place->second].covers.push_back(clip);
            ++folders_[place->second].count;
            if (key == openFolder_) clips_.push_back(clip);
        }
        // Not in this tab, or emptied: back to the overview. A search that matches nothing here keeps the folder open,
        // so correcting or clearing it returns to the same place.
        if (clips_.empty() && search_.empty()) openFolder_.clear();
    } else clips_ = std::move(shown);
    backLabel_.clear();
    backCount_.clear();
    if (inFolder()) {
        backLabel_ = openFolderName_;
        backCount_ = L"no matches";
    }
    for (const auto& folder : folders_) {
        if (folder.key != openFolder_) continue;
        backLabel_ = folder.name;
        backCount_ = folder.count == 1 ? L"1 clip" : std::to_wstring(folder.count) + L" clips";
    }
    if (wasOverview != overview()) scroll_ = 0;
    library_->want({}, {}); // Cancel obsolete thumbnail requests after filtering.
    index_.clear();
    dates_.clear();
    SYSTEMTIME now{};
    GetLocalTime(&now);
    focus_ = items() == 0 ? 0 : std::clamp(oldFocus, 0, items() - 1);
    if (overview()) {
        // Folder tiles: the newest clip's date. Thumbnails are found by their covers' paths.
        for (std::size_t i = 0; i < folders_.size(); ++i) {
            for (const auto& cover : folders_[i].covers) index_[cover.path.wstring()] = i;
            if (folders_[i].key == focusedFolder) focus_ = static_cast<int>(i);
            dates_.push_back(formatClipDate(folders_[i].covers.front().written, now));
        }
    }
    for (std::size_t i = 0; i < clips_.size(); ++i) {
        index_[clips_[i].path.wstring()] = i;
        if (clips_[i].path == focused || (!pendingFocus_.empty() && clips_[i].path == pendingFocus_)) {
            focus_ = static_cast<int>(i); // Keep the keyboard place, also across a rename.
            if (clips_[i].path == pendingFocus_) {
                pendingFocus_.clear();
                selection_.select(clips_, static_cast<int>(i)); // The renamed clip stays selected.
            }
        }
        auto date = formatClipDate(clips_[i].written, now);
        if (isCompressedCopy(clips_[i].path)) {
            // "<date> · was 540.3 mb"; the original is named without the prefix and any " (n)".
            const auto source = compressedSourceStem(clips_[i].path);
            const auto original = std::find_if(allClips_.begin(), allClips_.end(), [&](const ClipFile& clip) {
                return !isCompressedCopy(clip.path) && _wcsicmp(clip.path.stem().c_str(), source.c_str()) == 0;
            });
            if (original != allClips_.end()) date += L" · was " + formatClipBytes(original->bytes);
        }
        dates_.push_back(std::move(date));
    }
    selection_.prune(clips_); // Gone, renamed or hidden by the tab: no longer selected.
    if (band_) bandBase_.prune(clips_);
    // Forget thumbnails and durations of clips that are gone or changed; keep those of clips the tab, a folder or the
    // search only hides, so clearing a search does not load them all again (requestThumbnails bounds the bitmaps).
    std::unordered_map<std::wstring, const ClipFile*> listed;
    for (const auto& clip : allClips_) listed.emplace(clip.path.wstring(), &clip);
    const auto stillListed = [&](const std::wstring& key, const ClipFile& clip) {
        const auto found = listed.find(key);
        return found != listed.end() && sameClip(*found->second, clip);
    };
    for (auto it = thumbnails_.begin(); it != thumbnails_.end();) {
        if (stillListed(it->first, it->second.clip)) ++it;
        else { DeleteObject(it->second.bitmap); it = thumbnails_.erase(it); }
    }
    for (auto it = details_.begin(); it != details_.end();) {
        if (stillListed(it->first, it->second.clip)) ++it;
        else { missing_.erase(it->first); it = details_.erase(it); }
    }
    hover_ = pressed_ = -1;
    starHover_ = pressedFavourite_ = false;
    moreHover_ = pressedMore_ = binHover_ = pressedBin_ = false;
    nameHover_ = pencilHover_ = pressedPencil_ = false;
    requestedFirst_ = requestedLast_ = -1;
    updateLabels();
    measure();
    if (GetFocus() == window_ && items() > 0) focusTile(focus_);
    SendMessageW(parent_, WM_COMMAND, MAKEWPARAM(id_, clipsChanged), reinterpret_cast<LPARAM>(window_));
}

void ClipGrid::measure() {
    if (!window_) return;
    RECT client{};
    GetClientRect(window_, &client);
    gap_ = px(smallTiles_ ? 12 : 16);
    pad_ = px(8); // Room for the focus glow around edge tiles.
    // Compact density shows more, narrower tiles; captions and buttons keep their size.
    const int target = px(smallTiles_ ? 176 : 232);
    const int width = std::max(1, static_cast<int>(client.right) - pad_ * 2);
    columns_ = std::max(1, (width + gap_) / (target + gap_));
    tileWidth_ = (width - gap_ * (columns_ - 1)) / columns_;
    thumbHeight_ = tileWidth_ * 9 / 16;
    tileHeight_ = thumbHeight_ + px(50);
    const int rows = (items() + columns_ - 1) / columns_;
    content_ = rows ? contentTop() + pad_ + rows * tileHeight_ + (rows - 1) * gap_ : 0;
    requestedFirst_ = requestedLast_ = -1;
    scrollTo(scroll_);
}

void ClipGrid::scrollTo(int y) {
    RECT client{};
    GetClientRect(window_, &client);
    scroll_ = std::clamp(y, 0, std::max(0, content_ - static_cast<int>(client.bottom)));
    SCROLLINFO info{sizeof(info), SIF_ALL | SIF_DISABLENOSCROLL, 0, std::max(0, content_ - 1),
                    static_cast<UINT>(client.bottom), scroll_};
    SetScrollInfo(window_, SB_VERT, &info, TRUE);
    placeEdit();
    InvalidateRect(window_, nullptr, FALSE);
}

RECT ClipGrid::tileRect(int index) const {
    const int x = pad_ + index % columns_ * (tileWidth_ + gap_);
    const int y = contentTop() + index / columns_ * (tileHeight_ + gap_) - scroll_;
    return {x, y, x + tileWidth_, y + tileHeight_};
}

int ClipGrid::hitTest(POINT point) const {
    if (tileWidth_ <= 0) return -1; // Not measured yet.
    const int column = (point.x - pad_) / (tileWidth_ + gap_);
    const int row = (point.y + scroll_ - contentTop()) / (tileHeight_ + gap_);
    if (point.x < pad_ || point.y + scroll_ < contentTop() || column >= columns_) return -1;
    const int index = row * columns_ + column;
    if (index >= items()) return -1;
    const RECT tile = tileRect(index);
    return PtInRect(&tile, point) ? index : -1;
}

void ClipGrid::focusTile(int index) {
    if (items() == 0) return;
    focus_ = std::clamp(index, 0, items() - 1);
    RECT client{};
    GetClientRect(window_, &client);
    const RECT tile = tileRect(focus_);
    const int above = focus_ < columns_ ? contentTop() : pad_; // The first row brings the way back into view too.
    if (tile.top - above < 0) scrollTo(scroll_ + tile.top - above);
    else if (tile.bottom + pad_ > client.bottom) scrollTo(scroll_ + tile.bottom + pad_ - client.bottom);
    InvalidateRect(window_, nullptr, FALSE);
}

void ClipGrid::open(int index) {
    if (overview()) return enterFolder(index);
    if (index < 0 || index >= static_cast<int>(clips_.size())) return;
    target_ = clips_[index].path;
    SendMessageW(parent_, WM_COMMAND, MAKEWPARAM(id_, clipOpened), reinterpret_cast<LPARAM>(window_));
}

RECT ClipGrid::moreRect(int index) const {
    const auto tile = tileRect(index);
    const int top = tile.top + thumbHeight_ + px(7);
    return {tile.right - px(30), top, tile.right, top + px(22)};
}

RECT ClipGrid::pencilRect(int index, bool* truncated) const {
    const auto tile = tileRect(index);
    const auto more = moreRect(index);
    const int width = index < static_cast<int>(labels_.size()) ? labels_[index].nameWidth : 0;
    const int after = tile.left + px(2) + width + px(4);
    const int left = std::min<int>(after, more.left - px(24));
    if (truncated) *truncated = left < after;
    return {left, more.top, left + px(24), more.bottom};
}

// Clicks on the pencil work only while it shows: while the name row is hovered or the tile has the keyboard.
bool ClipGrid::pencilShown(int index) const {
    return pencilShownFor(index, index == hover_ && nameHover_);
}

bool ClipGrid::pencilShownFor(int index, bool nameHovered) const {
    if (edit_ || index < 0 || index >= static_cast<int>(clips_.size()) || !canRename(clips_[index].path)) return false;
    return nameHovered || (GetFocus() == window_ && index == focus_);
}

bool ClipGrid::pencilAt(int index, POINT point, bool nameHovered) const {
    if (!pencilShownFor(index, nameHovered)) return false;
    const auto box = pencilRect(index);
    return PtInRect(&box, point) != FALSE;
}

// The box that holds the name while it is edited: the whole name line, ⋯ included.
RECT ClipGrid::nameFrame(int index) const {
    const auto tile = tileRect(index);
    const auto more = moreRect(index);
    return {tile.left, more.top, tile.right, more.bottom};
}

void ClipGrid::rename(const std::filesystem::path& path) {
    const auto found = index_.find(path.wstring());
    if (found != index_.end()) beginRename(static_cast<int>(found->second));
}

void ClipGrid::beginRename(int index) {
    if (edit_ || index < 0 || index >= static_cast<int>(clips_.size()) || !canRename(clips_[index].path)) return;
    focusTile(index);
    renaming_ = clips_[index].path;
    edit_ = CreateWindowExW(0, L"EDIT", clipDisplayName(renaming_).c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 0, 0, window_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!edit_) { renaming_.clear(); return; }
    SendMessageW(edit_, EM_SETLIMITTEXT, 200, 0);
    placeEdit();
    SetFocus(edit_);
    SendMessageW(edit_, EM_SETSEL, 0, -1);
    InvalidateRect(window_, nullptr, FALSE);
}

// Keeps the box over its tile after scrolling, resizing or a rescan; a clip that is gone cancels it.
void ClipGrid::placeEdit() {
    if (!edit_) return;
    const auto found = index_.find(renaming_.wstring());
    if (found == index_.end()) { finishRename(false); return; }
    const auto frame = nameFrame(static_cast<int>(found->second));
    int height = px(16);
    if (const HDC dc = GetDC(window_)) {
        const auto old = SelectObject(dc, nameFont_);
        TEXTMETRICW metrics{};
        if (GetTextMetricsW(dc, &metrics)) height = metrics.tmHeight;
        SelectObject(dc, old);
        ReleaseDC(window_, dc);
    }
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(nameFont_), FALSE);
    SetWindowPos(edit_, nullptr, frame.left + px(7), frame.top + (frame.bottom - frame.top - height) / 2,
        frame.right - frame.left - px(14), height, SWP_NOZORDER | SWP_NOACTIVATE);
}

bool ClipGrid::renameKey(WPARAM key) {
    if (!edit_ || GetFocus() != edit_) return false;
    if (key == VK_RETURN || key == VK_TAB) finishRename(true);
    else if (key == VK_ESCAPE) finishRename(false);
    else return false;
    return true;
}

void ClipGrid::finishRename(bool commit) {
    if (!edit_) return;
    wchar_t text[256]{};
    GetWindowTextW(edit_, text, static_cast<int>(std::size(text)));
    const HWND edit = std::exchange(edit_, nullptr); // Losing focus below must not commit again.
    const auto path = std::move(renaming_);
    renaming_.clear();
    if (GetFocus() == edit) SetFocus(window_);
    DestroyWindow(edit);
    InvalidateRect(window_, nullptr, FALSE);
    if (commit) commitRename(path, text);
}

void ClipGrid::commitRename(const std::filesystem::path& path, const std::wstring& text) {
    std::filesystem::path renamed;
    const auto problem = renameClip(path, text, renamed);
    if (!problem.empty()) messageBox(parent_, problem.c_str(), L"refarter", MB_OK | MB_ICONWARNING);
}

std::wstring ClipGrid::renameClip(const std::filesystem::path& path, const std::wstring& text, std::filesystem::path& renamed) {
    renamed.clear();
    const auto name = trimClipName(text);
    if (name.empty() || name == clipDisplayName(path)) return {}; // Nothing to change.
    switch (checkClipName(name)) {
    case ClipNameProblem::InvalidCharacter: return L"a clip name can't contain \\ / : * ? \" < > |.";
    case ClipNameProblem::Reserved: return L"windows reserves that name · try another.";
    default: break;
    }
    const auto found = std::find_if(allClips_.begin(), allClips_.end(), [&](const auto& clip) { return clip.path == path; });
    if (found == allClips_.end()) return L"this clip is not in the library yet · try again in a moment.";
    if (!canRename(path)) return path == exporting_ || path == pendingTrimSource_ ? L"finish or cancel compression before renaming this clip." : L"wait for the bin to finish, then try again.";
    const auto clip = *found; // A modal prompt could replace the list.
    const auto target = renamedClipPath(path, name);
    // No replace flag: an existing clip is never overwritten.
    if (!MoveFileExW(path.c_str(), target.c_str(), 0)) {
        const auto error = GetLastError();
        return error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS ? L"a clip with that name already exists."
                                                                           : renameFailedText;
    }
    // The clip stays in its folder: a rename keeps the size and write time its remembered game is filed under.
    if (const auto game = sourceOf(clip); !game.empty() && game != clipSourceFromName(target)) library_->remember({{clip, game}});
    pendingFocus_ = target;
    clickedClip_.clear();
    if (clip.favourite) renamedFavourites_.push_back(target);
    scan(folder_);
    renamed = target;
    return {};
}

void ClipGrid::showMenu(int index, POINT screen) {
    if (index < 0 || index >= static_cast<int>(clips_.size())) return;
    focusTile(index);
    // A menu on a clip outside the selection acts on that clip alone; inside it, on the whole selection.
    if (!selection_.contains(clips_[index].path)) selection_.select(clips_, index);
    InvalidateRect(window_, nullptr, FALSE);
    target_ = clips_[index].path;
    menuPoint_ = screen;
    SendMessageW(parent_, WM_COMMAND, MAKEWPARAM(id_, clipMenu), reinterpret_cast<LPARAM>(window_));
}

void ClipGrid::keepFavourite(const std::filesystem::path& path, const std::filesystem::path& replacement) {
    for (const auto& clip : allClips_) {
        if (clip.path != path || !clip.favourite) continue;
        // A scan already in flight might not list a new output yet: wait for one that does.
        if (!replacement.empty() && replacement != path) renamedFavourites_.push_back(replacement);
        else refavourite_.push_back(path);
    }
}

bool ClipGrid::showsClip(int index, const ClipFile& clip) const {
    if (index < 0 || index >= items()) return false;
    if (!overview()) return sameClip(clip, clips_[index]);
    const auto& covers = folders_[index].covers;
    return std::any_of(covers.begin(), covers.end(), [&](const ClipFile& cover) { return sameClip(clip, cover); });
}

// Inside the thumbnail's 1-pixel border; a folder's quarters leave a small gap between them.
SIZE ClipGrid::thumbnailFrame() const {
    const SIZE frame{tileWidth_ - 2, thumbHeight_ - 2};
    return overview() ? SIZE{(frame.cx - px(2)) / 2, (frame.cy - px(2)) / 2} : frame;
}

void ClipGrid::requestThumbnails(int first, int last) {
    requestedFirst_ = first;
    requestedLast_ = last;
    std::vector<ClipFile> wanted;
    const SIZE frame = thumbnailFrame();
    for (int i = first; i <= last; ++i) {
        for (const auto& clip : tileClips(i)) {
            const auto key = clip.path.wstring();
            if (missing_.count(key)) continue;
            // Thumbnails fit the frame exactly and are drawn 1:1; after a resize, a DPI change or between
            // folders and clips the old one is stretched only until its replacement arrives.
            if (auto cached = thumbnails_.find(key); cached != thumbnails_.end() &&
                (std::abs(cached->second.size.cx - frame.cx) <= 1 || std::abs(cached->second.size.cy - frame.cy) <= 1)) continue;
            wanted.push_back(clip);
        }
    }
    library_->want(std::move(wanted), frame);
    // Keep memory bounded: drop thumbnails far from the screen once the cache grows (a folder tile shows four).
    const int visible = last - first + 1;
    if (static_cast<int>(thumbnails_.size()) > std::max(48, visible * (overview() ? 12 : 3))) {
        for (auto it = thumbnails_.begin(); it != thumbnails_.end();) {
            const auto known = index_.find(it->first);
            const int i = known != index_.end() ? static_cast<int>(known->second) : -1;
            if (known != index_.end() && i >= first - visible && i <= last + visible) ++it;
            else { DeleteObject(it->second.bitmap); it = thumbnails_.erase(it); }
        }
    }
}

void ClipGrid::paint(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    fill(dc, client, library);
    // An open folder with no match still draws its way back.
    if ((items() == 0 && !inFolder()) || tileWidth_ <= 0) return;
    const int rowHeight = tileHeight_ + gap_;
    const int firstRow = std::max(0, (scroll_ - contentTop()) / rowHeight);
    const int lastRow = (scroll_ + client.bottom - contentTop()) / rowHeight;
    const int first = firstRow * columns_;
    const int last = std::min(items() - 1, (lastRow + 1) * columns_ - 1);
    if (first > last && items() > 0) return;
    // Painting only happens while visible, so requests follow what is on screen.
    // One row ahead makes scrolling down show finished thumbnails.
    const int ahead = std::min(items() - 1, last + columns_);
    if (items() > 0 && (first != requestedFirst_ || ahead != requestedLast_)) requestThumbnails(first, ahead);

    const float scale = static_cast<float>(dpi_) / 96.f;
    const bool focused = GetFocus() == window_;
    SetBkMode(dc, TRANSPARENT);
    SetStretchBltMode(dc, COLORONCOLOR);
    HDC source = CreateCompatibleDC(dc);
    Gdiplus::Graphics g(dc); // One for the whole paint.
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    // Rounded corners for thumbnails, built once at the origin and moved onto each tile.
    const HRGN corners = CreateRoundRectRgn(1, 1, tileWidth_ + 1, thumbHeight_ + 1, px(14), px(14));
    // A rounded badge on the thumbnail with one centred line of small text.
    const auto pill = [&](RECT box, int radius, COLORREF edge, const wchar_t* value, COLORREF colour) {
        roundRect(g, box, radius, background, edge);
        const auto old = SelectObject(dc, dateFont_);
        SetTextColor(dc, colour);
        DrawTextW(dc, value, -1, &box, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, old);
    };
    if (inFolder()) {
        // The way back to the folders: a chevron, the folder's game and its clip count.
        const auto back = backRect();
        if (backHover_ || pressedBack_) roundRect(g, back, px(8), raised, raised);
        Gdiplus::Pen pen(color(backHover_ ? ink : muted), 1.6f * scale);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        const float cx = static_cast<float>(back.left + px(16)), cy = (back.top + back.bottom) / 2.f;
        const Gdiplus::PointF chevron[] = {{cx + 3 * scale, cy - 6 * scale}, {cx - 3 * scale, cy}, {cx + 3 * scale, cy + 6 * scale}};
        g.DrawLines(&pen, chevron, 3);
        const int left = back.left + px(28);
        drawText(dc, nameFont_, backLabel_, {left, back.top, left + backNameWidth_ + 1, back.bottom}, ink);
        drawText(dc, dateFont_, backCount_, {left + backNameWidth_ + px(10), back.top, back.right, back.bottom}, muted);
        if (items() == 0) {
            const int top = contentTop() - scroll_ + px(24);
            drawText(dc, nameFont_, L"no clips in this folder match.", {pad_, top, client.right - pad_, top + px(24)}, muted, DT_CENTER);
        }
    }
    for (int i = first; i <= last; ++i) {
        const RECT tile = tileRect(i);
        RECT halo = tile;
        InflateRect(&halo, px(6), px(6));
        RECT reach = tile, overlap{};
        InflateRect(&reach, pad_, pad_);
        if (!IntersectRect(&overlap, &reach, &dirty_)) continue; // Not part of this repaint.
        const Label& label = labels_[i];
        // Selected: a light accent wash with one accent border on the tile's own rounded shape (inset, so it
        // never spills out). Keyboard focus alone adds a border of the same shape; hover stays grey.
        const bool selected = !overview() && selection_.contains(clips_[i].path);
        const bool keyFocus = focused && keyboardNav_ && i == focus_;
        if (selected) roundRect(g, halo, px(10), accentSurface, accentSurface);
        else if (i == hover_ || i == pressed_) roundRect(g, halo, px(10), raised, raised);
        if (selected || keyFocus) {
            const float width = (selected ? (keyFocus ? 2.f : 1.5f) : 1.f) * scale, half = width / 2.f;
            Gdiplus::GraphicsPath path;
            roundPath(path, halo.left + half, halo.top + half, static_cast<float>(halo.right - halo.left) - width,
                      static_cast<float>(halo.bottom - halo.top) - width, static_cast<float>(px(10) * 2));
            Gdiplus::Pen pen(color(accent), width);
            g.DrawPath(&pen, &path);
        }
        const RECT thumb{tile.left, tile.top, tile.right, tile.top + thumbHeight_};
        roundRect(g, thumb, px(8), background, border);
        // A clip fills the frame; a folder shows its four newest clips 2 × 2 (cells without a clip stay empty).
        const SIZE cell = thumbnailFrame();
        const auto shown = tileClips(i);
        SelectClipRgn(dc, corners);
        OffsetClipRgn(dc, thumb.left, thumb.top);
        for (std::size_t k = 0; k < shown.size(); ++k) {
            const int left = thumb.left + 1 + static_cast<int>(k % 2) * (cell.cx + px(2));
            const int top = thumb.top + 1 + static_cast<int>(k / 2) * (cell.cy + px(2));
            const auto key = shown[k].path.wstring();
            if (auto cached = thumbnails_.find(key); cached != thumbnails_.end()) {
                // Fit inside the 16:9 cell, keeping the clip's own aspect ratio.
                const auto [bw, bh] = cached->second.size;
                const double fit = std::min(static_cast<double>(cell.cx) / bw, static_cast<double>(cell.cy) / bh);
                const int w = static_cast<int>(bw * fit), h = static_cast<int>(bh * fit);
                const int x = left + (cell.cx - w) / 2, y = top + (cell.cy - h) / 2;
                const auto previous = SelectObject(source, cached->second.bitmap);
                // Thumbnails arrive at cell size and are copied. A stale size after a
                // resize is stretched with the cheap mode until its replacement arrives.
                if (w == bw && h == bh) BitBlt(dc, x, y, w, h, source, 0, 0, SRCCOPY);
                else StretchBlt(dc, x, y, w, h, source, 0, 0, bw, bh, SRCCOPY);
                SelectObject(source, previous);
            } else {
                // Placeholder until the thumbnail arrives, or for files Windows cannot preview.
                const float cx = left + cell.cx / 2.f, cy = top + cell.cy / 2.f, r = (overview() ? 6.f : 9.f) * scale;
                const Gdiplus::PointF play[] = {{cx - r * 0.7f, cy - r}, {cx + r, cy}, {cx - r * 0.7f, cy + r}};
                Gdiplus::SolidBrush brush(color(missing_.count(key) ? border : muted));
                g.FillPolygon(&brush, play, 3);
            }
        }
        SelectClipRgn(dc, nullptr);
        if (overview()) {
            // A folder: a folder mark where a clip has its star, the clip count where it has its size, then its game
            // and the date of its newest clip.
            const RECT count{thumb.right - px(23) - label.sizeWidth, thumb.bottom - px(29), thumb.right - px(7), thumb.bottom - px(7)};
            pill(count, px(5), background, label.size.c_str(), ink);
            const auto mark = favouriteRect(i);
            roundRect(g, mark, px(7), background, border);
            Gdiplus::Pen pen(color(i == hover_ || (focused && i == focus_) ? accent : muted), 1.4f * scale);
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            const float cx = (mark.left + mark.right) / 2.f, cy = (mark.top + mark.bottom) / 2.f;
            const auto at = [&](float x, float y) { return Gdiplus::PointF{cx + x * scale, cy + y * scale}; };
            const Gdiplus::PointF shape[] = {at(-7.5f, -5.5f), at(-2.5f, -5.5f), at(-0.5f, -3.5f), at(7.5f, -3.5f), at(7.5f, 5.5f), at(-7.5f, 5.5f)};
            g.DrawPolygon(&pen, shape, 6);
            const int captionTop = thumb.bottom + px(8);
            drawText(dc, nameFont_, label.name, {tile.left + px(2), captionTop, tile.right - px(4), captionTop + px(20)}, ink);
            drawText(dc, dateFont_, dates_[i], {tile.left + px(2), captionTop + px(21), tile.right - px(4), captionTop + px(39)}, muted);
            continue;
        }
        if (!label.duration.empty()) {
            const RECT badge{thumb.left + px(7), thumb.bottom - px(29), thumb.left + px(23) + label.durationWidth, thumb.bottom - px(7)};
            pill(badge, px(5), background, label.duration.c_str(), ink);
        }
        {
            // The file size mirrors the duration badge at the right; it yields to the duration on narrow
            // tiles and to the compression badge.
            const RECT badge{thumb.right - px(23) - label.sizeWidth, thumb.bottom - px(29), thumb.right - px(7), thumb.bottom - px(7)};
            const int durationRight = label.duration.empty() ? thumb.left : thumb.left + px(23) + label.durationWidth;
            const bool clear = badge.left >= durationRight + px(6) &&
                !(clips_[i].path == exporting_ && badge.top < thumb.top + thumbHeight_ / 2 + px(18));
            if (clear) pill(badge, px(5), background, label.size.c_str(), ink);
        }
        if (clips_[i].path == exporting_) {
            const std::wstring text = exportCancelling_ ? L"cancelling…" : L"compressing · " + std::to_wstring(exportPercent_) + L"%";
            const RECT badge{thumb.left + px(7), thumb.top + thumbHeight_ / 2 - px(18), thumb.right - px(7), thumb.top + thumbHeight_ / 2 + px(18)};
            pill(badge, px(6), border, text.c_str(), ink);
            RECT rail{badge.left + px(8), badge.bottom - px(5), badge.right - px(8), badge.bottom - px(3)};
            fill(dc, rail, border);
            rail.right = rail.left + (rail.right - rail.left) * exportPercent_ / 100;
            fill(dc, rail, accent);
        }
        // Bin and star share one look: grey at rest; the button under the mouse gets a
        // lighter background and an orange icon. A favourite's star stays filled orange.
        const bool active = i == hover_ || (focused && i == focus_);
        if (!active && isCompressedCopy(clips_[i].path)) {
            // A compressed copy is marked at the top right, where the bin takes over on hover or focus.
            const RECT tag{thumb.right - px(7) - px(16) - compressedTag_.cx, thumb.top + px(7), thumb.right - px(7),
                           thumb.top + px(7) + px(4) + compressedTag_.cy};
            pill(tag, px(5), background, L"compressed", accent);
        }
        if (active) {
            const auto bin = binRect(i);
            const bool lit = binHover_ && i == hover_ && !recycling_;
            roundRect(g, bin, px(7), lit ? raised : background, border);
            Gdiplus::Pen pen(color(lit ? accent : muted), 1.4f * scale);
            const float cx = (bin.left + bin.right) / 2.f, cy = (bin.top + bin.bottom) / 2.f;
            g.DrawLine(&pen, cx - 8 * scale, cy - 5 * scale, cx + 8 * scale, cy - 5 * scale);
            g.DrawRectangle(&pen, cx - 3 * scale, cy - 8 * scale, 6 * scale, 3 * scale);
            const Gdiplus::PointF body[] = {{cx - 6 * scale, cy - 3 * scale}, {cx - 5 * scale, cy + 8 * scale},
                {cx + 5 * scale, cy + 8 * scale}, {cx + 6 * scale, cy - 3 * scale}};
            g.DrawLines(&pen, body, 4);
            g.DrawLine(&pen, cx - 2 * scale, cy - 1 * scale, cx - 2 * scale, cy + 5 * scale);
            g.DrawLine(&pen, cx + 2 * scale, cy - 1 * scale, cx + 2 * scale, cy + 5 * scale);
        }
        // Favourites always show their star; other clips only while hovered or focused.
        if (active || clips_[i].favourite) {
            const auto favourite = favouriteRect(i);
            const bool lit = starHover_ && i == hover_ && !savingFavourite_;
            roundRect(g, favourite, px(7), lit ? raised : background, border);
            const auto tint = savingFavourite_ ? muted : clips_[i].favourite || lit ? accent : muted;
            Gdiplus::Pen pen(color(tint), 1.4f * scale);
            Gdiplus::SolidBrush brush(color(tint));
            Gdiplus::PointF points[10];
            const float cx = (favourite.left + favourite.right) / 2.f, cy = (favourite.top + favourite.bottom) / 2.f;
            for (int vertex = 0; vertex < 10; ++vertex) {
                const float angle = -1.5707963f + vertex * 0.62831853f;
                const float radius = (vertex % 2 ? 4.4f : 9.5f) * scale;
                points[vertex] = {cx + std::cos(angle) * radius, cy + std::sin(angle) * radius};
            }
            if (clips_[i].favourite) g.FillPolygon(&brush, points, 10);
            g.DrawPolygon(&pen, points, 10);
        }
        const auto more = moreRect(i);
        const bool renamingThis = edit_ && renaming_ == clips_[i].path;
        const bool pencil = !renamingThis && pencilShown(i);
        auto text = [&](const std::wstring& value, HFONT font, COLORREF colour, int top, int height, int right) {
            RECT rect{tile.left + px(2), top, right, top + height};
            const auto previous = SelectObject(dc, font);
            SetTextColor(dc, colour);
            DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &rect,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | DT_LEFT);
            SelectObject(dc, previous);
        };
        const int captionTop = thumb.bottom + px(8);
        bool truncated = false;
        const RECT pencilBox = pencil ? pencilRect(i, &truncated) : RECT{};
        // The pencil takes room from a long name; a short one leaves it where it is.
        if (!renamingThis)
            text(label.name, nameFont_, ink, captionTop, px(20), pencil && truncated ? pencilBox.left - px(2) : more.left - px(4));
        text(dates_[i], dateFont_, muted, captionTop + px(21), px(18), more.left - px(4));
        if (renamingThis) {
            // The edit box sits in this frame, over the name line.
            roundRect(g, nameFrame(i), px(6), background, accent);
        } else {
            if (pencil) {
                // Like ⋯: no border, a raised background and an ink icon under the mouse.
                const bool lit = i == hover_ && pencilHover_;
                if (lit || (pressedPencil_ && i == pressed_)) roundRect(g, pencilBox, px(6), raised, raised);
                Gdiplus::Pen pen(color(lit ? ink : muted), 1.4f * scale);
                pen.SetLineJoin(Gdiplus::LineJoinRound);
                const float cx = (pencilBox.left + pencilBox.right) / 2.f, cy = (pencilBox.top + pencilBox.bottom) / 2.f;
                auto at = [&](float x, float y) { return Gdiplus::PointF{cx + x * scale, cy + y * scale}; };
                // A pencil lying diagonally, tip at the bottom left.
                const Gdiplus::PointF body[] = {at(-5.5f, 5.5f), at(-1.8f, 4.3f), at(6.7f, -4.2f), at(4.2f, -6.7f), at(-4.3f, 1.8f)};
                g.DrawPolygon(&pen, body, 5);
                g.DrawLine(&pen, at(-4.3f, 1.8f), at(-1.8f, 4.3f));
                g.DrawLine(&pen, at(2.1f, -4.6f), at(4.6f, -2.1f));
            }
            // ⋯ beside the name opens the same menu as a right click.
            const bool lit = i == hover_ && moreHover_;
            if (lit || (pressedMore_ && i == pressed_)) roundRect(g, more, px(6), raised, raised);
            Gdiplus::SolidBrush dot(color(lit ? ink : muted));
            const float cx = (more.left + more.right) / 2.f, cy = (more.top + more.bottom) / 2.f, r = 1.6f * scale;
            for (int k = -1; k <= 1; ++k) g.FillEllipse(&dot, cx + k * 5.f * scale - r, cy - r, r * 2, r * 2);
        }
    }
    DeleteObject(corners);
    DeleteDC(source);
    if (band_ && bandMoved_) {
        // The dragged rectangle, in window coordinates.
        const int left = std::min<int>(bandStart_.x, bandPoint_.x), right = std::max<int>(bandStart_.x, bandPoint_.x);
        const int top = std::min<int>(bandStart_.y - scroll_, bandPoint_.y), bottom = std::max<int>(bandStart_.y - scroll_, bandPoint_.y);
        const Gdiplus::RectF rect(static_cast<float>(left), static_cast<float>(top), static_cast<float>(right - left), static_cast<float>(bottom - top));
        Gdiplus::SolidBrush wash(Gdiplus::Color(48, accent & 0xFF, (accent >> 8) & 0xFF, (accent >> 16) & 0xFF));
        Gdiplus::Pen edge(color(accent), 1.f);
        g.FillRectangle(&wash, rect);
        g.DrawRectangle(&edge, rect);
    }
}

LRESULT CALLBACK ClipGrid::proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* grid = reinterpret_cast<ClipGrid*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        grid = static_cast<ClipGrid*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        grid->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(grid));
    }
    return grid ? grid->handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT ClipGrid::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case libraryReady: receive(); return 0;
    case WM_SIZE: measure(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window_, &ps);
        RECT client{};
        GetClientRect(window_, &client);
        dirty_ = ps.rcPaint;
        buffer_.paint(dc, client.right, client.bottom, [&](HDC target) { paint(target); });
        EndPaint(window_, &ps);
        return 0;
    }
    case WM_VSCROLL: {
        RECT client{};
        GetClientRect(window_, &client);
        const int line = tileHeight_ / 3;
        switch (LOWORD(wParam)) {
        case SB_LINEUP: scrollTo(scroll_ - line); break;
        case SB_LINEDOWN: scrollTo(scroll_ + line); break;
        case SB_PAGEUP: scrollTo(scroll_ - client.bottom); break;
        case SB_PAGEDOWN: scrollTo(scroll_ + client.bottom); break;
        case SB_TOP: scrollTo(0); break;
        case SB_BOTTOM: scrollTo(content_); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: {
            SCROLLINFO info{sizeof(info), SIF_TRACKPOS};
            GetScrollInfo(window_, SB_VERT, &info);
            scrollTo(info.nTrackPos);
            break;
        }
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        scrollTo(scroll_ - GET_WHEEL_DELTA_WPARAM(wParam) * px(72) / WHEEL_DELTA);
        return 0;
    case WM_MOUSEMOVE: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int hit = hitTest(point);
        const Parts now = hit >= 0 ? parts(hit, point) : Parts{};
        if (const RECT back = backRect(); (inFolder() && PtInRect(&back, point)) != backHover_) {
            backHover_ = !backHover_;
            RECT area = back;
            InflateRect(&area, px(2), px(2));
            InvalidateRect(window_, &area, FALSE);
        }
        if (band_ && (wParam & MK_LBUTTON)) {
            if (!bandMoved_ && std::abs(point.x - bandStart_.x) + std::abs(point.y + scroll_ - bandStart_.y) < px(4)) return 0;
            bandMoved_ = true;
            bandPoint_ = point;
            updateBand();
        }
        if (dragReady_ && (wParam & MK_LBUTTON) && pressed_ >= 0 &&
            (std::abs(point.x - dragStart_.x) > GetSystemMetricsForDpi(SM_CXDRAG, dpi_) ||
             std::abs(point.y - dragStart_.y) > GetSystemMetricsForDpi(SM_CYDRAG, dpi_))) {
            startDrag(pressed_);
            return 0;
        }
        if (!tracking_) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
            tracking_ = TrackMouseEvent(&track) != FALSE;
        }
        if (hit != hover_ || now.star != starHover_ || now.more != moreHover_ || now.bin != binHover_ ||
            now.name != nameHover_ || now.pencil != pencilHover_) {
            const int previous = hover_;
            hover_ = hit;
            nameHover_ = now.name;
            pencilHover_ = now.pencil;
            starHover_ = now.star;
            moreHover_ = now.more;
            binHover_ = now.bin;
            invalidateTile(previous);
            invalidateTile(hit);
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        tracking_ = false;
        const int previous = hover_, held = pressed_;
        hover_ = pressed_ = -1;
        moreHover_ = starHover_ = binHover_ = nameHover_ = pencilHover_ = false;
        if (std::exchange(backHover_, false)) InvalidateRect(window_, nullptr, FALSE);
        invalidateTile(previous);
        invalidateTile(held);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && (hover_ >= 0 || backHover_)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
    case WM_LBUTTONDOWN: {
        keyboardNav_ = false;
        SetFocus(window_);
        SetCapture(window_);
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (const RECT back = backRect(); inFolder() && PtInRect(&back, point)) {
            pressedBack_ = true; // Leaves on release over it.
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        const int previousPressed = pressed_, previousFocus = focus_;
        focusBeforePress_ = focus_;
        dragReady_ = false;
        pressed_ = hitTest(point);
        if (pressed_ < 0 && !overview()) {
            // Empty space: a click clears the selection; dragging draws a rectangle (Ctrl adds to it).
            band_ = true;
            bandMoved_ = false;
            bandStart_ = {point.x, point.y + scroll_};
            bandPoint_ = point;
            bandBase_ = keyDown(VK_CONTROL) ? selection_ : ClipSelection{};
        }
        pressedFavourite_ = pressedMore_ = pressedBin_ = pressedPencil_ = false;
        if (pressed_ >= 0) {
            const Parts down = parts(pressed_, point);
            pressedFavourite_ = down.star;
            pressedMore_ = down.more;
            pressedBin_ = down.bin;
            pressedPencil_ = down.pencil;
            focus_ = pressed_;
            // The picture and the name can be dragged out; the tile's buttons only click.
            dragReady_ = !overview() && !down.star && !down.more && !down.bin && !down.pencil;
            dragStart_ = point;
        }
        invalidateTile(previousPressed);
        invalidateTile(previousFocus);
        invalidateTile(pressed_);
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (std::exchange(pressedBack_, false)) {
            ReleaseCapture();
            const RECT back = backRect();
            if (inFolder() && PtInRect(&back, point)) leaveFolder();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        const int hit = hitTest(point);
        const bool click = hit >= 0 && hit == pressed_;
        const Parts up = hit >= 0 ? parts(hit, point) : Parts{};
        const bool favouriteClick = pressedFavourite_, moreClick = pressedMore_, binClick = pressedBin_, pencilClick = pressedPencil_;
        const int released = pressed_;
        pressed_ = -1;
        pressedMore_ = pressedPencil_ = dragReady_ = false;
        const bool wasBand = band_, dragged = bandMoved_;
        if (band_) endBand();
        ReleaseCapture();
        invalidateTile(released);
        if (overview()) {
            if (click) enterFolder(hit); // One click opens a folder: it has nothing to select.
            return 0;
        }
        const bool ctrl = keyDown(VK_CONTROL), shift = keyDown(VK_SHIFT);
        if (wasBand && !dragged && !ctrl && !shift) clearSelection();
        clickedClip_ = click && !up.star && !favouriteClick && !up.more && !moreClick && !up.bin && !binClick && !up.pencil && !pencilClick
            ? clips_[hit].path : std::filesystem::path{};
        if (!clickedClip_.empty()) {
            if (shift) selection_.range(clips_, hit, focusBeforePress_, ctrl);
            else if (ctrl) selection_.toggle(clips_, hit);
            else selection_.select(clips_, hit);
            InvalidateRect(window_, nullptr, FALSE);
        }
        if (click && up.star && favouriteClick) toggleFavourite(hit);
        if (click && up.bin && binClick) {
            // The bin acts on the whole selection when its tile is part of it, otherwise on its own tile.
            if (!selection_.contains(clips_[hit].path)) selection_.select(clips_, hit);
            recycleSelection();
        }
        if (click && up.pencil && pencilClick) beginRename(hit);
        if (click && up.more && moreClick) {
            const auto dots = moreRect(hit);
            POINT at{dots.left, dots.bottom + px(4)};
            ClientToScreen(window_, &at);
            showMenu(hit, at);
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (overview()) return 0; // The first click already opened the folder.
        const int hit = hitTest(point);
        // Stars and ⋯ stay single-click actions; the second click must be on the
        // same selected tile, not a neighbouring tile or one of its buttons.
        if (hit >= 0 && hit == focus_ && clips_[hit].path == clickedClip_ && !keyDown(VK_CONTROL) && !keyDown(VK_SHIFT)) {
            const Parts on = parts(hit, point);
            if (!on.star && !on.more && !on.bin && !on.pencil) { open(hit); return 0; } // Opened: no press, so no drag follows.
        }
        // Otherwise this is a press that came within the double-click time after a click: set it up as a
        // press (a possible drag or click) on the picture or the name, as WM_LBUTTONDOWN does.
        if (hit >= 0) {
            const Parts on = parts(hit, point);
            if (!on.star && !on.more && !on.bin && !on.pencil) return handle(WM_LBUTTONDOWN, wParam, lParam);
        }
        return 0;
    }
    case WM_XBUTTONUP: // The mouse's back button leaves a folder.
        if (GET_XBUTTON_WPARAM(wParam) == XBUTTON1 && leaveFolder()) return TRUE;
        break;
    case WM_RBUTTONDOWN: {
        keyboardNav_ = false;
        const int hit = hitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        SetFocus(window_);
        if (hit >= 0 && !overview()) {
            focusTile(hit);
            if (!selection_.contains(clips_[hit].path)) selection_.select(clips_, hit); // Inside the selection it stays.
        }
        return 0;
    }
    case WM_CONTEXTMENU: {
        // Right click opens at the pointer; Shift+F10 or the menu key under the focused tile's ⋯.
        POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        int index = focus_;
        if (lParam == -1) {
            if (clips_.empty()) return 0;
            focusTile(index);
            const auto dots = moreRect(index);
            at = {dots.left, dots.bottom + px(4)};
            ClientToScreen(window_, &at);
        } else {
            POINT client = at;
            ScreenToClient(window_, &client);
            index = hitTest(client);
            if (index < 0) return 0;
        }
        showMenu(index, at);
        return 0;
    }
    case BM_CLICK: open(focus_); return 0; // Enter, routed by the window's message loop.
    case WM_KEYDOWN: {
        if (!keyboardNav_) { keyboardNav_ = true; InvalidateRect(window_, nullptr, FALSE); } // Keys show the focus border.
        RECT client{};
        GetClientRect(window_, &client);
        const int pageRows = std::max(1, static_cast<int>(client.bottom) / (tileHeight_ + gap_));
        const bool ctrl = keyDown(VK_CONTROL), shift = keyDown(VK_SHIFT), repeat = (lParam & (1LL << 30)) != 0;
        // Moving: Shift extends the selection from the anchor, Ctrl moves only the keyboard place, a plain
        // arrow selects the clip it lands on.
        const auto move = [&](int target) {
            if (items() == 0) return;
            const int before = focus_;
            focusTile(target);
            if (overview()) return; // Folders are not selected.
            if (shift) selection_.range(clips_, focus_, before, ctrl);
            else if (!ctrl) selection_.select(clips_, focus_);
        };
        switch (wParam) {
        case 'F':
            if (!repeat) {
                if (!selection_.empty()) favouriteSelection();
                else toggleFavourite(focus_);
            }
            break;
        case 'A':
            if (!ctrl) return DefWindowProcW(window_, message, wParam, lParam);
            selection_.selectAll(clips_);
            InvalidateRect(window_, nullptr, FALSE);
            break;
        case 'C':
            if (!ctrl) return DefWindowProcW(window_, message, wParam, lParam);
            if (!repeat) copySelection();
            break;
        case VK_F2: if (!repeat) beginRename(focus_); break;
        case VK_BACK: leaveFolder(); break;
        case VK_DELETE: if (!repeat) recycleSelection(); break;
        case VK_LEFT: move(focus_ - 1); break;
        case VK_RIGHT: move(focus_ + 1); break;
        case VK_UP: move(focus_ - columns_); break;
        case VK_DOWN: move(focus_ + columns_); break;
        case VK_PRIOR: move(focus_ - columns_ * pageRows); break;
        case VK_NEXT: move(focus_ + columns_ * pageRows); break;
        case VK_HOME: move(0); break;
        case VK_END: move(items() - 1); break;
        case VK_SPACE:
            if (ctrl && !clips_.empty()) { selection_.toggle(clips_, focus_); InvalidateRect(window_, nullptr, FALSE); }
            else open(focus_);
            break;
        default: return DefWindowProcW(window_, message, wParam, lParam);
        }
        return 0;
    }
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lParam) == edit_ && HIWORD(wParam) == EN_KILLFOCUS) finishRename(true);
        return 0;
    case WM_CTLCOLOREDIT: {
        const auto edit = reinterpret_cast<HDC>(wParam);
        SetTextColor(edit, ink);
        SetBkColor(edit, background);
        return reinterpret_cast<LRESULT>(editBrush_);
    }
    case WM_SETFOCUS: case WM_KILLFOCUS:
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (wParam == noteTimer) {
            KillTimer(window_, noteTimer);
            note_.clear();
            SendMessageW(parent_, WM_COMMAND, MAKEWPARAM(id_, noteChanged), reinterpret_cast<LPARAM>(window_));
        } else if (wParam == bandTimer && band_) {
            // Scroll towards the pointer while it is above or below the grid, then redo the rectangle.
            POINT cursor{};
            GetCursorPos(&cursor);
            ScreenToClient(window_, &cursor);
            RECT client{};
            GetClientRect(window_, &client);
            if (cursor.y < 0 || cursor.y > client.bottom) scrollTo(scroll_ + (cursor.y < 0 ? -1 : 1) * px(24));
            bandPoint_ = {cursor.x, std::clamp<LONG>(cursor.y, -px(200), client.bottom + px(200))};
            bandMoved_ = true;
            updateBand();
        }
        return 0;
    case WM_CANCELMODE: case WM_CAPTURECHANGED:
        if (band_) endBand();
        pressedBack_ = dragReady_ = false;
        invalidateTile(std::exchange(pressed_, -1));
        pressedFavourite_ = pressedMore_ = pressedBin_ = pressedPencil_ = false;
        return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

} // namespace refarter
