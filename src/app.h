#pragma once

#include <chrono>
#include <windows.h>
#include <array>
#include <future>
#include <string>
#include <vector>
#include "recording_controller.h"
#include "capture/audio_capture.h"
#include "auto_capture.h"
#include "auto_session.h"
#include "tray_flyout.h"
#include "tray_icon.h"
#include "compression_job.h"
#include "trim_follow_up.h"
#include "clip_grid.h"
#include "player/clip_player.h"
#include "player/clip_export.h"
#include "config.h"
#include "hotkey.h"
#include "raw_hotkey.h"
#include "settings.h"
#include "storage_limit.h"
#include "update_client.h"
#include "theme.h"
#include "chime.h"
#include "power_resume.h"
#include "value_bubble.h"
#include <functional>

namespace refarter {

enum class Page { Library, Recording, Settings };

// Text styles (px at 96 dpi): small 12, body 14, heading 17 and title 28 (the last two semibold).
enum class Font { Small, Body, Heading, Title };

// Control ids and the groups of them that size arrays or select behaviour.
namespace ui {
enum Id {
    Library = 100, Recording, Settings, AllClips, Favourites,
    SelectProcess, StartReplay, ThirtySeconds, OneMinute, ThreeMinutes,
    FiveMinutes, LargeTiles, SmallTiles, CustomSeconds,
    SaveReplay, DesktopAudio, Microphone, Suppression, Gate, Resolution, FrameRate,
    Bitrate, ChooseFolder, OpenFolder, Clips,
    GateOpen, GateClose, GateAttack, GateHold, GateRelease, QualityPreset, VideoCodec, AssignHotkey,
    DesktopVolumeLabel, DesktopVolume, MicrophoneVolumeLabel, MicrophoneVolume, CloseToTray,
    ChimeVolumeLabel, ChimeVolume, StartWithWindows, AutoCaptureSource, Player, RecordDiscord,
    SidebarToggle, Compressed, DesktopReset, MicrophoneReset,
    DiagnosticLogging, CompressCopyAfterTrim, StorageLimitField, StorageBehaviour, GroupFolders, AskBeforeBinning, ClipSearch,
    AssignScreenshot, UpdateCheckSwitch, UpdateCheckNow, StartMinimised, LastId = StartMinimised
};
// The global shortcuts, as indexes into App's shortcut arrays.
inline constexpr int saveShortcut = 0, screenshotShortcut = 1, shortcutCount = 2;
// Ids are contiguous, so a control is found by `id - firstId`.
inline constexpr int firstId = Library, idCount = LastId - firstId + 1;
// Row buttons drawn as on/off switches (their text names the state for screen readers).
inline constexpr int toggleIds[] = {Suppression, Gate, RecordDiscord, CloseToTray, StartWithWindows, StartMinimised, DiagnosticLogging, CompressCopyAfterTrim, AskBeforeBinning, UpdateCheckSwitch};
// Number fields of the noise gate and the caption painted before each.
struct GateField { int id; const wchar_t* name; };
inline constexpr GateField gateFields[] = {
    {GateOpen, L"open"}, {GateClose, L"close"}, {GateAttack, L"attack"}, {GateHold, L"hold"}, {GateRelease, L"release"},
};
inline const GateField* gateFieldOf(int id) {
    const auto found = std::find_if(std::begin(gateFields), std::end(gateFields), [id](const auto& field) { return field.id == id; });
    return found == std::end(gateFields) ? nullptr : found;
}
} // namespace ui

// Page state and the preferences that live outside the recorder's settings.
struct ShellState {
    Page page = Page::Library;
    ClipView clipView = ClipView::All; // The library tab: all clips, favourites or compressed copies.
    int replaySeconds = 180;
    bool replayInputValid = true;
    bool smallTiles = false;
    bool groupFolders = false; // Library: a folder per game in every tab.
    bool askBeforeBinning = true; // Library: the recycle-bin question is asked.
    bool closeToTray = false;
    bool startWithWindows = false;
    bool startMinimised = true;
    bool sidebarCollapsed = false; // Icons only, for more page width.
    bool diagnosticLogging = false;
    bool compressCopyAfterTrim = false; // The trim bar's "compress & copy" box starts ticked.
    unsigned maxStorageGb = 0;          // Clip folder limit; 0 is none.
    StorageFull storageFull = StorageFull::Warn;
    bool storageFullChosen = false;     // The user picked a behaviour (saved), so the first-time question is over.
    UpdateChoice updateCheck = UpdateChoice::Ask; // The daily release check: ask once, on or off.
    long long lastUpdateCheck = 0;      // Unix seconds of the last check that got an answer.
};

class App {
public:
    ~App();
    int run(HINSTANCE instance, int showCommand, bool startup = false);

private:
    enum class ButtonBackdrop { Canvas, Card, Sidebar };
    struct Control {
        HWND window = nullptr;
        Page page = Page::Library;
        bool global = false; // Shown on every page.
        ButtonBackdrop backdrop = ButtonBackdrop::Canvas;
    };

    HWND window_ = nullptr;
    UINT dpi_ = 96;
    ShellState state_;
    std::array<Control, ui::idCount> controls_{}; // By id - ui::firstId; ids without a control have no window.
    std::array<HFONT, 4> fonts_{}; // By Font.
    HFONT fontOf(Font font) const { return fonts_[static_cast<size_t>(font)]; }
    theme::BackBuffer buttonBuffer_; // Owner-drawn buttons are composed here, then copied once.
    theme::BackBuffer backBuffer_; // The window's paint buffer, kept between paints.
    int hovered_ = 0;
    bool keyboardFocus_ = false; // Focus came from the keyboard, so the focused control shows its outline.
    void setKeyboardFocus(bool on);
    // Switch animation: displayed position (0 off .. 1 on) per toggle, eased towards the state.
    // The timer exists only while one of them runs.
    struct SwitchMotion { float from = 0.f, to = 0.f; ULONGLONG start = 0; bool running = false; };
    std::array<SwitchMotion, std::size(ui::toggleIds)> switches_{};
    float switchPosition(int id) const;
    void animateSwitch(int id, float from);
    void afterToggleChanged(int id);
    void stepSwitches();
    HBRUSH surfaceBrush_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    HICON largeIcon_ = nullptr;
    HICON smallIcon_ = nullptr;
    TrayIcon tray_;
    UINT restoreMessage_ = 0;
    UINT quitMessage_ = 0;
    bool startupLaunch_ = false, startupTray_ = false, startupOtherCopy_ = false;
    bool startupReadFailed_ = false;
    std::unique_ptr<RecordingController> recorder_;
    RecordingTarget target_;
    std::wstring sourceMonitor_; // Device name of the chosen (or remembered) monitor; empty for a game.
    capture::VideoSettings video_;
    capture::AudioSettings audio_;
    capture::RecorderState recording_ = capture::RecorderState::Off;
    // While buffering: an enabled audio source has no device open (unplugged, disabled).
    bool desktopMissing_ = false, microphoneMissing_ = false;
    bool discordUnavailable_ = false; // A session found that Windows cannot split Discord out.
    bool discordSharesOutput_ = false; // Discord plays on the chosen desktop output; checked when the audio page shows.
    void checkDiscordOutput();

    // Sleep, shutdown and leftovers (app_lifecycle.cpp). Nothing polls: Windows notifies, one timer fires once.
    SleepMemory sleep_;                // What was running when Windows suspended.
    bool asleep_ = false;              // Replay was paused for sleep and not yet restarted, kept or dropped.
    bool resumeDue_ = false;           // The settle delay after a resume has passed.
    bool sleepRestart_ = false;        // The replay now starting is the restart after sleep.
    bool pendingSuspend_ = false;      // Windows suspended while the recorder was busy; applied when it answers.
    bool pauseInFlight_ = false;       // The sleep pause was sent to the recorder and its own answer is still due.
    bool resumeSeenWhilePending_ = false; // ...and the machine already woke: the pending suspend is dropped, not applied.
    bool endingSession_ = false;       // Windows is shutting down: a save error is a status, never a dialog.
    std::wstring progressNote_;        // The last progress toast written for a trim follow-up.
    int waiting_ = 0;                  // Inside waitFor (a bounded shutdown or sleep wait that pumps messages).
    bool shutdownBlocked_ = false;     // A shutdown block reason is shown until WM_ENDSESSION's wait ends.
    bool quietDialogs() const { return closing_ || endingSession_ || asleep_ || waiting_ > 0; } // Report in the status line, never open a dialog.
    void report(const std::wstring& text, UINT icon); // A dialog, or the status line while dialogs must stay closed.
    // Error messages from the recorder (a failed save on a full disk, a failed start) that arrived while Refarter's own
    // window was not in front: never a dialog over a game. Each is kept with whether its sound already played; shown on activation.
    std::vector<std::pair<std::wstring, bool>> queuedErrors_;
    bool showingErrors_ = false; // Modal loops may dispatch activation and recording events again.
    void showQueuedErrors();
    bool shellOpen(const wchar_t* target, const wchar_t* failure = nullptr, UINT icon = MB_ICONERROR); // Opens a file, folder or link as Explorer would; `failure`, if given, is the dialog on error.
    // The startup scan for unfinished files runs on a thread of its own (same-volume
    // recovery moves). ~App waits for it briefly; one stuck on a slow volume is let go: it holds only copies and the shared task.
    struct LeftoverTask;
    std::thread leftoverThread_;
    std::shared_ptr<LeftoverTask> leftoverTask_;
    std::future<void> leftoverFinished_;
    void powerBroadcast(WPARAM wParam);
    void suspendForSleep();
    void pauseForSleep();              // Ends capture and keeps an unsaved replay; the recorder answers with a Stopped event.
    void applyPendingSuspend();
    void resumeFromSleep();
    void finishResume();               // Decides once the delay has passed and the recorder is idle.
    bool sourceStillThere() const;
    void revalidateMonitor();          // After waking: a stale monitor handle is found again by its device name, or dropped.
    bool waitFor(const std::function<bool()>& done, DWORD timeoutMs); // Pumps messages until done or the time runs out.
    bool queryEndSession(LPARAM flags);
    void endSession(bool ending, LPARAM flags);
    void startLeftoverCleanup();
    void finishLeftoverCleanup();      // Quit: waits briefly for the cleanup thread, then lets a stuck one go.
    void leftoversHandled(int found, int moved);
    void releaseLeftoverTask();         // The window is going: the cleanup thread posts nothing from here on.
    void showLeftoverNote();           // Shows the cleanup result once the recorder is idle.
    std::wstring leftoverNote_;        // The cleanup result still waiting for the status line.
    // Live level meters (app_meters.cpp). The recorder feeds them while buffering; with replay off, a
    // meter-only capture does, only while the Recording page is visible. The timer exists only while a meter shows.
    capture::LevelMeter desktopMeter_, microphoneMeter_;
    std::array<std::unique_ptr<capture::AudioCapture>, 2> meterCapture_; // Desktop, microphone.
    std::vector<std::future<void>> meterClosers_; // Meter-only captures being stopped off the UI thread; joined in ~App.
    std::array<capture::MeterBar, 2> meterBars_;
    std::array<int, 4> meterKeys_{-1, -1, -1, -1}; // What each display last showed: sidebar desktop/mic, slider desktop/mic.
    std::array<theme::BackBuffer, 4> meterBuffers_; // One kept compose bitmap per display.
    ULONGLONG meterTick_ = 0;
    bool meterTimer_ = false, meterToolAdded_ = false;
    void syncMeters(bool reopen = false);
    void stepMeters();
    void closeMeterCapture(int source);
    void closeMeterCaptures();
    void joinMeterClosers();
    bool meterDim(int source) const;
    int meterFill(int source, int length, int thickness) const;
    RECT meterRect(int display) const;
    void drawMeterBar(HDC dc, RECT bar, bool vertical, int source) const;
    void paintMeterNow(int display, HWND target, RECT bar, bool vertical, int source);
    RECT meterIconRect(int display) const;
    void drawMeterIcon(HDC dc, int display) const;
    int litSegments(int source) const;
    void drawSegmentMeter(HDC dc, RECT box, int source) const;
    void paintSliderMeters(HDC dc, const RECT& dirty) const;
    const wchar_t* meterToolText(UINT_PTR id) const;
    void paintSidebarMeters(HDC dc, const RECT& dirty) const;
    void updateMeterTooltip();
    bool isMeterTool(UINT_PTR id) const;
    std::filesystem::path clipFolder_;
    Chimes chimes_;
    ClipGrid clipGrid_;
    // The built-in player replaces the grid on the library page while a clip is open.
    player::ClipPlayer player_;
    bool playerOpen_ = false;
    int playerVolume_ = 100;
    struct Fullscreen { bool on = false; WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)}; LONG_PTR style = 0; };
    Fullscreen fullscreen_;
    player::ClipPlayer::Callbacks playerCallbacks(); // What the player asks of the app: leave, fullscreen, open elsewhere, menus.
    void openClip(const std::filesystem::path& path);
    bool openExternally(const std::filesystem::path& path);
    void showInFolder(const std::filesystem::path& path);
    void closePlayer();
    void showClipMenu();
    void startCompression(const std::filesystem::path& path, bool discardIntermediate = false);
    void receiveCompression();
    void trimSaved(const player::ClipPlayer::SavedTrim& trim);
    std::uint64_t trimPlayerSession_ = 0; // The player session in which the trim started.
    TrimFollowUp trimFollowUp_; // The trimmed clip whose compression ends in a clipboard copy.
    CompressionJob compression_; // Polled by the 250 ms exportTimer while it runs.
    bool recorderClosed_ = false;
    void setFullscreen(bool on);
    bool busy_ = false, saving_ = false, closing_ = false;
    bool shooting_ = false; // A screenshot was asked for and has not answered yet.
    Config config_;
    std::vector<SettingEntry> stored_; // What refarter.ini says now, to write only changes.
    // The global shortcuts, indexed by saveShortcut and screenshotShortcut; each registers under its index + 1.
    std::array<Hotkey, ui::shortcutCount> hotkeys_{defaultHotkey, defaultScreenshotHotkey};
    std::array<bool, ui::shortcutCount> hotkeyRegistered_{};
    bool assigning_ = false;
    int assigningShortcut_ = ui::saveShortcut; // Which shortcut is being assigned, while assigning_.
    UINT heldModifiers_ = 0; // Modifiers down while assigning, shown on the shortcut row.
    UINT releaseKey_ = 0; // Key-up of the key that ended assignment, swallowed like the key-down.
    std::array<std::wstring, ui::shortcutCount> hotkeyNotes_; // Last assignment problem of each; empty when there is none.
    bool bitrateValid_ = true;
    std::array<bool, std::size(ui::gateFields)> gateFieldValid_ = [] { std::array<bool, std::size(ui::gateFields)> valid; valid.fill(true); return valid; }();
    std::vector<capture::CodecSupport> codecs_; // Empty until the GPU check has run.
    bool probing_ = false;
    std::wstring status_ = L"choose a game, then enable replay";
    bool statusRoutine_ = true; // status_ is an ordinary prompt, not worth a note on the source row.
    void setStatus(std::wstring text, bool routine = false) { status_ = std::move(text); statusRoutine_ = routine; }
    std::mutex eventsMutex_;
    std::vector<RecordingEvent> events_;

    // Auto-capture: the explicit game list and its two switches are settings kept here; the hooks, card,
    // suppression and match-exit rules live in AutoSession, which asks the App through these.
    AutoCaptureList autoCapture_;
    bool autoCaptureReadable_ = true; // False: the list file is unreadable and is never overwritten.
    bool autoCaptureOn_ = true, autoCapturePrompt_ = true;
    AutoSession auto_;
    AutoSession::Host autoHost();
    AutoSession::Context autoContext() const;
    bool startAutoReplay(RecordingTarget target); // Selects `target` and starts; the previous source returns on failure.
    void declineGame(const std::wstring& path);
    void setAutoCaptured(const std::wstring& path, const std::wstring& label, bool on);
    void chooseAutoCapture();
    bool canUseAutoCapture() const;
    void useAutoCapture();
    bool startReplay();
    void stopReplay();
    // One description of the session for the footer, the tray tip and the tray flyout.
    struct SessionLine {
        enum class State { Closing, Saving, Busy, Live, Retained, Off } state = State::Off;
        bool automatic = false;  // The live replay was started by auto-capture.
        std::wstring status;     // The footer wording: "replay on", "recording off"…
        std::wstring source;     // Lower case; empty without a source.
        std::wstring summary() const; // Tray tip and flyout wording.
    };
    SessionLine sessionLine() const;
    std::wstring footerStatus() const;
    RECT footerStatusBounds() const; // Device pixels; a click there opens the Recording page.
    bool footerHot_ = false;         // The pointer is over it.
    // The release check (app_updates.cpp). Opt-in; one worker thread only while a check is pending or running.
    void scheduleUpdateCheck(std::chrono::seconds delay); // The start-up rule: on, due, and not yet checked this session.
    void startUpdateCheck(bool manual, std::chrono::seconds delay);
    void updateCheckDone();                    // The worker answered: record it, show the footer note and the row note.
    void askUpdateQuestion();                  // The one-time opt-in question, only with Refarter's own window in front.
    void openUpdatePage();
    std::wstring updateRowNote() const;
    std::wstring footerUpdateText() const;     // "update available · v1.2.0", or empty.
    RECT footerUpdateBounds() const;           // Device pixels; empty when there is no update to show.
    int footerReserve() const;                 // Logical pixels the right end of the footer takes: the version text's room plus the update text.
    std::wstring footerVersion() const;        // "refarter v0.5.1 ...", the right end of the footer.
    int smallTextWidth(const std::wstring& text) const; // Logical pixels, in the small font.
    int smallTextExtent(const std::wstring& text) const; // Device pixels, measured once per text and font (the pointer's footer test runs on every mouse move).
    mutable std::vector<std::pair<std::wstring, int>> textExtents_; // Cleared with the fonts (createFonts); a changed status text or update tag is a new entry.
    UpdateClient updates_;
    bool updateManual_ = false;                // The check in flight was asked for by the button.
    bool updateTried_ = false;                 // A check was started this session (at most one automatic check per start).
    std::wstring updateTag_, updateUrl_;       // The newer release found, if any.
    std::wstring updateNote_;                  // The result of the last check, shown on the Settings row.
    bool updateHot_ = false;                   // The pointer is over the footer's update text.
    bool replayKeptNow() const; // The replay survives a failed save: nothing is stopping, quitting or clearing it (a sleep pause keeps it).
    std::wstring trayTip() const;
    void updateTrayTip();
    TrayFlyout trayFlyout_;
    void trayCommand(TrayCommand command);

    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK buttonProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK editProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK volumeProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    void createControls();
    void createFonts();
    void loadIcons();
    bool addTrayIcon();
    void removeTrayIcon();
    void restoreWindow();
    void quit();
    void refreshStartup();
    bool toggleStartWithWindows(); // False: nothing was changed and the click ends here.
    void updateReplayInput();
    void updateBitrateInput();
    void updateSearch();          // The library's search field changed: filter the clips.
    bool searchKey(WPARAM key);   // A key in the search field (Esc, Enter, Down); true when used.
    void updateGateInput();
    bool gateInputValid() const;
    // layout() rebuilds the page rows and moves every control; refreshPages() does that only when a row or
    // control would move, and otherwise just repaints the window.
    void layout();
    void placeControls();
    void refreshPages();
    bool rebuildSections(); // Refreshes pages_ and pageHeights_; true when any row, control or card geometry changed.
    int sidebarWidth() const;  // Logical pixels; narrower while collapsed.
    int tabWidth(int id) const; // A library tab: border, padding, icon and name, in logical pixels (measured by createFonts).
    std::array<int, 3> tabWidths_{};
    int contentLeft() const;   // Where page content starts, after the sidebar and its gap.
    int footerTop() const;     // Logical pixels: where the footer starts; the sidebar and the pages end there.
    HWND tooltips_ = nullptr;  // Names icon-only buttons: library view and folder, and the collapsed sidebar.
    void updateSidebarToggleText(); // The collapse handle's text names its action, for the tooltip and screen readers.
    // Recording and Settings scroll vertically and are built from titled cards. Each
    // row has a name, an optional note under it and its controls at the right end.
    struct Row {
        std::wstring name;
        std::wstring note;
        bool alert = false; // The note reports a problem.
        std::vector<int> ids;
        int y = 0, height = 0; // Page coordinates, before scrolling.
        struct Placed { int id, x, width; bool operator==(const Placed&) const = default; };
        std::vector<Placed> placed; // Where each control of `ids` sits (x from the content's left edge), set when the page is built.
        int textWidth = 0;          // Room for the name and note before the first control.
    };
    struct Section {
        const wchar_t* title;
        std::vector<Row> rows;
        int x = 0, width = 0;    // From the content's left edge.
        int top = 0, bottom = 0; // Heading top to card bottom.
    };
    std::vector<Section> buildSections(Page page) const; // Only the rows that apply right now, positioned.
    std::array<std::vector<Section>, 3> pages_; // By Page: the rows as of the last rebuild; the library has none.
    std::array<int, 3> pageHeights_{};
    bool saveShown_ = false; // Whether the last layout showed the sidebar's save button.
    int pageHeight(Page page) const { return pageHeights_[static_cast<size_t>(page)]; }
    std::wstring estimateText() const;
    bool toggled(int id) const;
    std::array<int, 3> scroll_{}; // Per page, in logical pixels; the library never scrolls.
    int scrollMax(Page page) const;
    void scrollPage(int delta); // The page on screen.
    void reveal(HWND child);    // Scrolls the page so a focused control is fully visible.
    void navigate(Page page);
    void command(int id);
    void chooseSource();
    void chooseAudioDevice(bool desktop);
    void resetAudio(bool desktop); // 100% and the system default device.
    UINT trackMenu(HMENU menu, int x, int y);
    void chooseFolder();
    void chooseResolution();
    void chooseFrameRate();
    void chooseQuality();
    void chooseCodec();
    void probeCodecs();
    const capture::CodecSupport* codecSupport(capture::Codec codec) const;
    capture::Size sourceSize() const;
    void saveReplay();
    void takeScreenshot();
    std::wstring saveName() const; // What clips and screenshots of the current source are named after.
    void shortcutPressed(int shortcut); // A global shortcut fired (saveShortcut or screenshotShortcut).
    // The storage limit and low-disk rules (app_storage.cpp). Nothing here reads a disk on the window thread: the
    // folder size and free space come from the library worker's last scan.
    bool storageAllowsSave(std::wstring& note); // Before each save: false when it must not go ahead; `note` is the status for one that does.
    std::uint64_t estimatedClipBytes(int seconds = -1) const; // The size of a clip of `seconds` (the replay length by default), as the quality row estimates it.
    int bufferedClipSeconds() const;           // The length the next save would have: the replay length, or less just after a start.
    StripState storageStrip() const;           // The text and colour beside the folder button.
    void updateStorageInput();
    void chooseStorageBehaviour();
    void askStorageQuestion(std::uint64_t used, std::uint64_t limit, std::uint64_t estimate, bool late = false);
    void askQueuedStorageQuestion();           // On activation: the question a save over a game could not ask.
    bool ownWindowForeground() const;
    bool storageAsked_ = false;                // The first-time question was shown this session.
    bool storageQuestionQueued_ = false;       // It was due while a game was in front; asked when Refarter is activated.
    bool askAfterSave_ = false;                // The question is due: ask it once the save in progress has started.
    bool asking_ = false;                      // The question is open (a nested save must not run under it).
    std::chrono::steady_clock::time_point bufferingSince_{}; // When the replay buffer started filling (zero: not).
    void invalidateStrip();
    bool gameTrackOn() const;                  // A game window with desktop audio records a "game" track too.
    void loadSettings(); // Reads refarter.ini and the auto-capture list into the state; once, when the window is created.
    bool configureDiagnostics(bool enabled);
    UserSettings settings() const;
    void saveSettings();
    RawHotkey rawHotkey_;
    std::array<PressReports, ui::shortcutCount> pressReports_; // Raw Input and WM_HOTKEY reports of one press act once.
    bool registerHotkey(int shortcut, Hotkey hotkey);
    void retryHotkeys(); // On activation: shortcuts another app held at start-up or while assigning, registered if free now.
    std::wstring describe(Hotkey hotkey) const;
    std::wstring clashNote(int shortcut) const; // The note of a shortcut whose combination the other one holds.
    void refreshAssigning(); // Row note and button text now; the row keeps its height, so no relayout.
    UINT currentModifiers() const;
    void modifiersChanged();
    void startAssigning(int shortcut);
    void assignKey(UINT key);
    void finishAssigning(std::optional<Hotkey> hotkey);
    void refreshControls();
    void receiveEvents();
    void paint(HDC dc, const RECT& dirty); // Draws only what intersects `dirty`.
    void drawButton(const DRAWITEMSTRUCT& item);
    void drawVolumeSlider(HWND slider, HDC dc);
    void paintVolumeSlider(HWND slider, HDC fallback);
    ValueBubble bubble_;      // The value above a slider knob while it is dragged or changed by key.
    bool bubbleDrag_ = false; // The mouse holds a knob, so the key timer must not hide the bubble.
    bool sliderPainting_ = false; // Between a slider's custom pre-paint and post-paint: the post-paint composes the one frame.
    void showSliderBubble(HWND slider);
    void hideSliderBubble();
    ValueBubble gateBubble_;       // What is wrong with a gate value, above the field (see syncGateBubble).
    int gateHover_ = 0;            // The gate field under the pointer, or 0.
    bool gateCloseAboveOpen_ = false; // Both levels parse, but close is above open.
    void syncGateBubble();         // Shows the bubble for the focused (else hovered) invalid gate field, or hides it.
    void hideGateBubble();
    int volumePercent(int slider) const;
    void updateVolumeLabel(int slider); // The value beside a slider, rewritten only when it changed (not while it is typed in).
    int typingVolume_ = 0; // The percentage field being typed in (its label id), or 0.
    int hoverVolume_ = 0;  // The percentage field under the pointer, or 0.
    void beginVolumeTyping(int label);
    HWND focusedTextField() const;                // The focused volume percentage, number field or library search, or null.
    RECT fieldFrame(int id) const; // The frame painted around a text field (see app_controls.cpp).
    void invalidateFieldFrame(HWND edit);         // Repaints all painted around an edit: frame, unit, gate caption and focus outline.
    void endVolumeTyping(int label); // Leaves editing: applies the typed value as a slider drag would.
    bool pickerStale() const; // A menu stayed open while replay started or the recorder got busy: its choice is dropped.
    void addButton(int id, const wchar_t* title, Page page,
                   ButtonBackdrop backdrop = ButtonBackdrop::Canvas, bool global = false);
    HWND control(int id) const;
    const Control* entry(int id) const; // Null for ids outside the table.
    int px(int value) const;
    void label(HDC dc, const std::wstring& value, int x, int y, int w, int h,
               Font font, COLORREF color, UINT alignment = DT_LEFT) const;
};

} // namespace refarter
