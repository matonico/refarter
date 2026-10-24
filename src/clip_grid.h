#pragma once

#include "clip_library.h"
#include "clip_selection.h"

#include "theme.h"

#include <windows.h>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace refarter {

enum class ClipView { All, Favourites, Compressed };

// The library's clip tiles, newest first: thumbnail, name and date. A child
// window with its own scrolling and keyboard focus (arrows, Home/End, Page
// Up/Down; single click selects; double click, Enter or Space opens). Opening a tile
// and its menu (right click, the ⋯ button beside the name, Shift+F10) are reported
// to the parent, which decides where clips play. Thumbnails are requested only
// for tiles on screen. With folders on, the tab first shows a tile per game (one
// click, Enter or Space opens it; Backspace, Esc, the mouse's back button or the
// way back above the clips leave it).
class ClipGrid {
public:
    // WM_COMMAND notification codes sent to the parent.
    static constexpr WORD clipsChanged = 0x8001;
    static constexpr WORD clipOpened = 0x8002; // target(): the clip to open.
    static constexpr WORD clipMenu = 0x8003;   // target() and menuPoint(): show the clip's menu.
    static constexpr WORD noteChanged = 0x8004; // note() changed: repaint the strip under the grid.
    static constexpr WORD askChanged = 0x8005;  // The question's "don't ask again" box was used: read askBeforeBinning().

    const std::filesystem::path& target() const { return target_; }
    // The line shown under a clip's name ("yesterday · 21:42"), or empty for a clip not listed.
    std::wstring dateOf(const std::filesystem::path& path) const {
        const auto found = index_.find(path.wstring());
        return found != index_.end() && found->second < dates_.size() ? dates_[found->second] : std::wstring{};
    }
    POINT menuPoint() const { return menuPoint_; } // Screen coordinates.
    // After the parent replaces a clip's file: mark the new file as a favourite
    // again if the old one was (marks follow size and write time).
    void keepFavourite(const std::filesystem::path& path, const std::filesystem::path& replacement = {});
    // The multi-selection (click, Ctrl+click, Shift+click, Ctrl+A, a dragged rectangle; Esc clears). Bulk actions
    // work on the selected clips of the current tab only.
    std::size_t selectedCount() const { return selection_.size(); }
    bool selectedAreFavourites() const;
    bool clearSelection();
    bool cancelBand();          // Esc during a rectangle drag: restores the selection from before it.
    void recycleSelection();    // One confirmation, one Recycle Bin operation.
    void favouriteSelection(std::optional<bool> add = std::nullopt); // Adds the mark to all, or removes it when all have it (or as told).
    bool copySelection();       // Puts the files on the clipboard.
    void setOpenClip(const std::filesystem::path& path) { open_ = path; } // The clip open in the player: never binned.
    void setTrimSource(const std::filesystem::path& path) { pendingTrimSource_ = path; } // Protected through trim compression.
    void say(std::wstring text) { showNote(std::move(text)); } // A short note beside the folder size.
    // Every clip in the folder, newest first, as of the last scan (compressed copies and favourites included).
    const std::vector<ClipFile>& allClips() const { return allClips_; }
    // Bulk actions protect the export input, open player clip and original of a trim/compression pipeline.
    std::vector<std::filesystem::path> busyClips() const { return {exporting_, open_, pendingTrimSource_}; }
    // The storage limit's "bin oldest clips": no question and no dialog, whatever the window is doing. The library worker
    // reads the folder afresh and moves only what the Recycle Bin can take (see ClipLibrary::binOldest). Refused (false)
    // while another bin is running. The result arrives as a note on the strip.
    bool binOldest(std::uint64_t limit, std::uint64_t estimate);
    // Free bytes on the clip drive and whether an automatic bin is safe here, as of the last scan (read on the worker).
    std::uint64_t driveFree() const { return driveFree_; }
    bool binAllowed() const { return binAllowed_; }
    // A short status for the strip ("copied 2 clips"), cleared after a few seconds.
    const std::wstring& note() const { return note_; }
    // Edits a clip's name in place over its tile; the parent's menu, F2 and the pencil all start it.
    bool canRename(const std::filesystem::path& path) const { return !recycling_ && path != exporting_ && path != pendingTrimSource_; }
    void rename(const std::filesystem::path& path);
    // Renames a clip's file (the player's rename too): checks the name, keeps its star and its folder. Returns what
    // went wrong (empty on success or when nothing changed); `renamed` is the new file, or empty.
    std::wstring renameClip(const std::filesystem::path& path, const std::wstring& text, std::filesystem::path& renamed);
    // A new file made from `from` (a trim or a compressed copy) goes into the same folder once a scan lists it.
    void inheritSource(const std::filesystem::path& clip, const std::filesystem::path& from);
    // Folders: the tab's clips sorted by game. The overview shows a tile per folder; opening one shows its clips under
    // a way back. Only how the library shows them: no file moves.
    void setGrouped(bool on);
    // Whether binning asks first. Off: recycleSelection() bins at once and says any skipped clips in the note.
    void setAskBeforeBinning(bool on) { askBeforeBinning_ = on; }
    bool askBeforeBinning() const { return askBeforeBinning_; }
    bool leaveFolder(); // Back to the overview; false when no folder is open.
    // Offered every key press before the window's message loop handles it: Enter and
    // Tab commit, Esc cancels. True when the key was used.
    bool renameKey(WPARAM key);
    bool renaming() const { return edit_ != nullptr; } // A clip name is being typed over its tile.
    void compression(const std::filesystem::path& path, float progress, bool cancelling);

    ClipGrid() = default;
    ~ClipGrid();
    ClipGrid(const ClipGrid&) = delete;
    ClipGrid& operator=(const ClipGrid&) = delete;

    HWND create(HWND parent, int id);
    void setStyle(UINT dpi, HFONT name, HFONT date, bool smallTiles);
    // Lists the folder again; the current tiles stay until the new list arrives.
    void scan(const std::filesystem::path& folder);
    bool scanned() const { return scanned_; }
    // Whether the last scan request is older than a given age (or there was none): a limit's size is rescanned on activation only then.
    bool scanOlderThan(std::chrono::seconds age) const { return std::chrono::steady_clock::now() - lastScan_ >= age; }
    std::size_t count() const { return viewCount_; } // Clips in the tab, in every folder.
    std::uint64_t bytes() const { return bytes_; }
    // Which clips show: all clips (compressed copies too, with their tag), favourites, or the compressed copies alone.
    void setView(ClipView view);
    // The search typed above the library: only clips whose name or game holds every word show, in every tab and
    // folder. Empty shows everything. count() is then the number of matches.
    void setSearch(const std::wstring& text);
    bool searching() const { return !search_.empty(); }
    // Tiles on show: none while an open folder has no match, although count() is not zero.
    bool hasTiles() const { return items() > 0; }
    // Focus is moving to the grid by a key (the search field's Enter or Down): show the focus border on the keyboard place.
    void showKeyboardFocus() { keyboardNav_ = true; focusTile(focus_); }
    const std::wstring& error() const { return favouritesError_; }

private:
    struct Thumbnail {
        HBITMAP bitmap;
        SIZE size;
        ClipFile clip;
    };
    struct Details { ClipFile clip; std::optional<std::uint64_t> duration; };
    struct Folder {
        std::wstring key, name; // key: the game in lower case, or "*" for clips without one ("other").
        std::vector<ClipFile> covers; // Its newest clips, up to four: shown 2 × 2 on the tile; the first gives the date.
        std::size_t count = 0;
    };
    struct PendingSource { std::filesystem::path clip; std::wstring source; int scans = 3; };

    // The folder overview is showing: its tiles are folders_, otherwise clips_.
    bool overview() const { return grouped_ && openFolder_.empty(); }
    bool inFolder() const { return grouped_ && !openFolder_.empty(); }
    int items() const { return static_cast<int>(overview() ? folders_.size() : clips_.size()); }
    const ClipFile& tileClip(int index) const { return overview() ? folders_[index].covers.front() : clips_[index]; }
    // The clips whose thumbnails a tile shows: one, or a folder's covers.
    std::vector<ClipFile> tileClips(int index) const { return overview() ? folders_[index].covers : std::vector<ClipFile>{clips_[index]}; }
    bool showsClip(int index, const ClipFile& clip) const; // `clip`, unchanged, is on tile `index`.
    SIZE thumbnailFrame() const; // The size a thumbnail is made for: the frame, or a quarter of it for folders.
    std::wstring sourceOf(const ClipFile& clip) const;
    int contentTop() const; // Where the first row starts: below the way back inside a folder.
    RECT backRect() const;  // The way back to the overview, inside a folder.
    void enterFolder(int index);

    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    int px(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    void receive();
    void updateList();
    void toggleFavourite(int index);
    void showNote(std::wstring text);
    void updateBand();
    void startDrag(int index); // Hands the clips to Explorer, Discord and the like as files to copy.
    std::vector<ClipFile> actionClips() const;
    void endBand();
    RECT favouriteRect(int index) const;
    RECT binRect(int index) const;
    RECT moreRect(int index) const;
    // The pencil sits just after the name, or beside the ⋯ when the name is cut short.
    RECT pencilRect(int index, bool* truncated = nullptr) const;
    bool pencilShown(int index) const;
    bool pencilShownFor(int index, bool nameHovered) const;
    bool pencilAt(int index, POINT point, bool nameHovered) const;
    // Which of a tile's controls a point falls on; the one place that hit-tests them.
    struct Parts { bool star = false, more = false, bin = false, name = false, pencil = false; };
    Parts parts(int index, POINT point) const;
    void invalidateTile(int index) const;
    void invalidatePath(const std::filesystem::path& path) const;
    // Captions and their widths, measured when data arrives rather than on every paint.
    struct Label { std::wstring name, size, duration; int nameWidth = 0, sizeWidth = 0, durationWidth = 0; };
    void updateLabels();
    void updateLabel(HDC dc, std::size_t index);
    RECT nameFrame(int index) const;
    void beginRename(int index);
    void finishRename(bool commit);
    void commitRename(const std::filesystem::path& path, const std::wstring& text);
    void placeEdit();
    void showMenu(int index, POINT screen);
    void measure();
    void scrollTo(int y);
    void focusTile(int index);
    void open(int index);
    int hitTest(POINT point) const;
    RECT tileRect(int index) const;
    void requestThumbnails(int first, int last);
    void paint(HDC dc);
    void clearThumbnails();

    HWND window_ = nullptr;
    theme::BackBuffer buffer_; // Paint buffer, kept between paints.
    HWND parent_ = nullptr;
    int id_ = 0;
    UINT dpi_ = 96;
    HFONT nameFont_ = nullptr;
    HFONT dateFont_ = nullptr;
    bool smallTiles_ = false;
    std::unique_ptr<ClipLibrary> library_;
    bool scanned_ = false;
    std::chrono::steady_clock::time_point lastScan_{};
    std::uint64_t bytes_ = 0;
    std::vector<ClipFile> clips_;   // The clip tiles: the tab's clips, or one folder's.
    std::vector<ClipFile> allClips_;
    std::size_t viewCount_ = 0;     // Clips in the tab.
    bool grouped_ = false;
    std::wstring openFolder_;       // Key of the open folder; empty for the overview.
    std::wstring openFolderName_;   // Its name, for the way back while a search hides all its clips.
    std::vector<Folder> folders_;   // The overview's tiles, newest first.
    std::wstring backLabel_;        // "apex legends" over the open folder's clips, and its count.
    std::wstring backCount_;
    int backWidth_ = 0, backNameWidth_ = 0; // Text widths of both together and of the name, measured with the labels.
    bool backHover_ = false, pressedBack_ = false;
    std::vector<PendingSource> pendingSources_;
    std::uint64_t driveFree_ = unknownBytes;
    bool binAllowed_ = false;
    std::filesystem::path folder_;
    std::filesystem::path clickedClip_; // First click must select the same file before opening.
    std::wstring favouritesError_;
    ClipView view_ = ClipView::All;
    std::wstring search_; // Trimmed; empty when nothing is searched for.
    bool savingFavourite_ = false, pressedFavourite_ = false, starHover_ = false;
    bool moreHover_ = false, pressedMore_ = false;
    bool nameHover_ = false, pencilHover_ = false, pressedPencil_ = false;
    HWND edit_ = nullptr; // The rename box, while one is open.
    HBRUSH editBrush_ = nullptr;
    std::filesystem::path renaming_;
    std::filesystem::path pendingFocus_;                   // A renamed clip keeps the keyboard place.
    std::vector<std::filesystem::path> renamedFavourites_; // Renamed favourites to mark again once rescanned.
    bool binHover_ = false, pressedBin_ = false, recycling_ = false;
    bool askBeforeBinning_ = true;
    bool automaticRecycle_ = false; // The running bin was started by the storage limit: its result is a note.
    std::filesystem::path target_;
    std::filesystem::path exporting_;
    int exportPercent_ = -1;
    bool exportCancelling_ = false;
    POINT menuPoint_{};
    std::vector<std::filesystem::path> refavourite_; // Replaced clips to mark again once rescanned.
    std::vector<std::wstring> dates_;
    std::vector<Label> labels_;            // Parallel to clips_.
    SIZE compressedTag_{};                 // Extent of the "compressed" pill text.
    RECT dirty_{};                         // The area being painted; tiles outside it are skipped.
    std::unordered_map<std::wstring, std::size_t> index_;
    std::unordered_map<std::wstring, Thumbnail> thumbnails_;
    std::unordered_set<std::wstring> missing_; // Windows has no thumbnail for these.
    std::unordered_map<std::wstring, Details> details_;
    // Geometry in pixels.
    int columns_ = 1, tileWidth_ = 0, thumbHeight_ = 0, tileHeight_ = 0, gap_ = 0, pad_ = 0, content_ = 0, scroll_ = 0;
    int hover_ = -1, focus_ = 0, pressed_ = -1;
    int requestedFirst_ = -1, requestedLast_ = -1;
    ClipSelection selection_;
    std::filesystem::path open_;
    std::filesystem::path pendingTrimSource_;
    std::wstring note_;
    int focusBeforePress_ = 0;
    // Dragging a rectangle from empty space: start in content coordinates, the pointer in the window.
    bool band_ = false, bandMoved_ = false, bandTimerOn_ = false;
    POINT bandStart_{}, bandPoint_{};
    ClipSelection bandBase_;
    // Pressing a tile's picture or name and moving past the system's drag distance drags its files out.
    bool dragReady_ = false, ole_ = false;
    POINT dragStart_{};
    bool tracking_ = false;
    bool keyboardNav_ = false; // The last input was a key: only then the focused tile shows its border.
};

} // namespace refarter
