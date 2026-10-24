#pragma once

#include "temporary_clip.h"

#include "player/player_view.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace refarter::player {

// The library's built-in player: a child window that plays one clip with its
// own decoding/rendering thread, and trims it into a new file. Nothing runs and
// no GPU or audio device is held while no clip is open.
//
// Keyboard: space/k play, ←/→ 5 s (one frame while trimming), j/l 10 s,
// ,/. one frame, ↑/↓ volume, Home/End the start/end (the trim's handles while trimming), m mute,
// f fullscreen, t trim, F2 rename; while trimming enter saves a new clip (the "save" button) and c
// toggles "compress & copy". The letters ignore Ctrl and Alt. Esc and Alt+← leave fullscreen,
// trimming (cancelling a copy being written), then the player.
class ClipPlayer {
public:
    struct SavedTrim {
        std::filesystem::path clip, source;
        bool compressCopy = false, replace = false;
        std::uint64_t session = 0;
    };
    struct Callbacks {
        std::function<void()> back;                                   // Leave the player.
        std::function<void(bool)> fullscreen;                         // The window should enter or leave fullscreen.
        std::function<void(const std::filesystem::path&)> external;   // Open in the default player.
        std::function<void(const std::filesystem::path&)> folder;     // Reveal the clip in Explorer.
        // A result derived from `source` is available as `clip`; compressed replacements keep their own name.
        std::function<void(const std::filesystem::path& clip, const std::filesystem::path& source)> saved;
        std::function<void(const std::filesystem::path&, const std::filesystem::path&)> replacing; // Original and replacement.
        std::function<void()> trimEnded;                              // The background trim write finished (or failed).
        // A trim failed after the player was left (no toast to show it); `reason` has no full stop.
        std::function<void(const std::wstring& reason)> trimFailed;
        // A trim was saved as `path`; `session` identifies the player that started it, not the current player.
        std::function<void(const SavedTrim&)> trimSaved;
        std::function<void()> cancelTrimCompression;
        std::function<void(int)> volume;                              // 0–100, to remember.
        // Renames the clip's file to `name` (already trimmed); the player keeps playing it, open with delete sharing.
        // Returns what went wrong (empty on success) and sets `renamed` to the new file.
        std::function<std::wstring(const std::filesystem::path& clip, const std::wstring& name, std::filesystem::path& renamed)> rename;
        // Shows a menu of `items` (those in `checked` ticked) at a screen point; returns the choice or -1.
        std::function<int(const std::vector<std::wstring>& items, const std::vector<bool>& checked, POINT at)> choose;
    };

    // The clip a trim is still being written from (empty when none): it must not be binned meanwhile.
    std::filesystem::path trimSource() const { return trimThread_.joinable() ? trimSource_ : std::filesystem::path{}; }
    std::uint64_t session() const { return session_; }

    // A trim is being edited or written.
    bool trimBusy() const { return view_.editing || view_.saving || trimThread_.joinable(); }

    ClipPlayer();
    ~ClipPlayer();
    ClipPlayer(const ClipPlayer&) = delete;
    ClipPlayer& operator=(const ClipPlayer&) = delete;

    HWND create(HWND parent, int id, Callbacks callbacks);
    // `date` is the line the library shows under the clip; empty: from the file's time.
    void open(const std::filesystem::path& path, int volume, const std::wstring& date = {}, bool muted = false);
    // Stops playback and releases the decoder, GPU surfaces and audio device.
    void close();
    void setDpi(UINT dpi);
    // Called once the window has entered or left fullscreen.
    void setFullscreen(bool on);
    // Whether the trim bar's "compress & copy" box starts ticked (the app setting), read each time trim mode opens.
    void setCompressCopyDefault(bool on) { compressDefault_ = on; }
    // A compression is running: the box is disabled (and unticked in effect). Updates a trim bar that is open.
    void setCompressionBusy(bool busy, const std::filesystem::path& clip = {}); // `clip`: the one being compressed.
    // The toast line: `hold` keeps it until replaced (progress), otherwise it fades after a few seconds.
    void showToast(const std::wstring& text, ToastKind kind, bool hold);
    const std::filesystem::path& clipPath() const { return path_; }
    const std::wstring& toastText() const { return view_.toast; } // Empty when no toast shows.
    // Offered every key press before the window's message loop handles it while the name is being edited:
    // Enter and Tab rename, Esc cancels. True when the key was used.
    bool renameKey(WPARAM key);
    // One step back, as Esc: renames a name being typed, then leaves fullscreen, then trim mode, then the player.
    // For the mouse's back button, a keyboard's browser back key and Alt+←.
    void back();
    // Ends the trim/export pipeline. Only its original player session may open the result or replace the source.
    // Empty result: release the busy state and keep the original trim view. Returns any replacement warning.
    // `takeOriginalName`: with `replace`, the result is moved onto the original's name (an unchanged trim that
    // already fits); `kept` receives where the result ended up.
    std::wstring finishTrimCompression(const std::filesystem::path& result, bool replace, std::uint64_t session,
                                       bool takeOriginalName = false, std::filesystem::path* kept = nullptr);
    void trimCompressionProgress(float progress, std::uint64_t session);

private:
    struct Engine;
    void setTitle(const std::filesystem::path& path); // The name over the picture, and its width for clicks.
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    void publish();  // Sends the view to the engine.
    // The open clip's facts (never null; shared, not copied) and what changes while it plays.
    std::shared_ptr<const ClipInfo> info() const;
    Playback playback() const;
    Layout layout() const;
    Layout layout(const ClipInfo& info) const;
    void command(int kind, double value = 0);
    void seek(double time, bool scrub);
    void activate(Part part);
    void drag(float x, float y);
    void startTrim();
    void endTrim();
    void leaveTrim(); // endTrim, also cancelling a copy that is still being written.
    void save(bool replace);
    std::filesystem::path replaceOriginal(bool keepCopyName = false); // The clip that now holds the trimmed copy.
    void setVolume(int volume, bool remember);
    void step(int frames);
    void toggleFullscreen();
    void showControls();
    void requestThumbnails();
    // Renaming: a small popup field over the title (the picture is drawn above any child window).
    static LRESULT CALLBACK renameProc(HWND, UINT, WPARAM, LPARAM);
    void beginRename();
    void placeRename();
    void finishRename(bool commit);
    void commitRename(const std::wstring& text);

    HWND window_ = nullptr;
    Callbacks callbacks_;
    std::filesystem::path path_;
    std::uint64_t session_ = 0; // Changes on close/open, including reopening the same file.
    std::uint64_t trimSession_ = 0;
    std::filesystem::path trimSource_; // Fixed when the trim starts, independent of subsequent playback.
    View view_;
    std::unique_ptr<Engine> engine_;
    bool wasPlaying_ = false;   // Playback resumes after a drag on the timeline.
    bool tracking_ = false;
    LPARAM lastMove_ = -1;      // Mouse moves that did not move do not reveal the controls.
    int thumbnailsFor_ = 0;      // Strip width the thumbnails were made for.
    std::thread trimThread_;
    std::atomic<float> trimProgress_ = 0;
    std::atomic<bool> trimCancel_ = false;
    std::wstring trimError_;
    std::filesystem::path trimmed_;
    TemporaryClipIdentity trimmedIdentity_;
    bool replace_ = false;      // The finished copy replaces the clip; the original goes to the recycle bin.
    bool compressDefault_ = false, compressBusy_ = false;
    std::filesystem::path compressClip_;
    bool followUp_ = false;     // The box was ticked when the running trim was saved.
    bool followUpBlocksTrim() const;
    bool followUpPending_ = false; // One trim/export pipeline; another trim cannot overwrite its source/range.
    std::filesystem::path followUpSource_; // The clip that pipeline belongs to; a trim of another clip may overwrite trimSource_.
    HWND renameBox_ = nullptr, renameEdit_ = nullptr;
    HFONT renameFont_ = nullptr;
    HBRUSH renameBrush_ = nullptr;
};

} // namespace refarter::player
