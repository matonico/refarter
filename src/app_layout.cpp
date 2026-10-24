// App: where everything goes. Sizes, the Recording and Settings rows, control placement and scrolling.
#include "app_internal.h"
#include "replay_duration.h"
#include "text.h"
#include "capture/audio_track.h"

#include <algorithm>
#include <bitset>

namespace refarter {
namespace {
constexpr int handleWidth = 24, handleHeight = 48; // The sidebar's collapse handle.
// Library controls replaced by the player while a clip is open.
constexpr int gridIds[] = {AllClips, Favourites, Compressed, GroupFolders, LargeTiles, SmallTiles, OpenFolder, Clips, ClipSearch};
// "2–250" for a range of whole numbers.
std::wstring rangeText(int low, int high) { return std::to_wstring(low) + L"–" + std::to_wstring(high); }
constexpr int sectionsTop = 68;          // The first card's heading, under the page title.
constexpr int settingsMaxWidth = 800;    // Settings rows stay readable in a wide window.
constexpr int recordingMaxWidth = 1040;  // Recording: two cards side by side, audio across both.
constexpr int twoColumnWidth = 800;      // Narrower pages stack Recording's cards in one column.
constexpr int columnGap = 16;
constexpr int nameWidth = 150;           // Kept free for a row's name before sliders shrink.
constexpr int deviceMenuMinimum = 150;  // The device menus shrink to this before a slider does.
constexpr int sectionGap = 28;
constexpr int rowHeight = 50, notedRowHeight = 60;
} // namespace

int App::px(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }

int App::sidebarWidth() const { return state_.sidebarCollapsed ? collapsedSidebar : expandedSidebar; }

int App::contentLeft() const { return sidebarWidth() + contentGap; }

int App::footerTop() const {
    RECT client{};
    GetClientRect(window_, &client);
    return MulDiv(client.bottom, 96, static_cast<int>(dpi_)) - footerHeight;
}

int App::tabWidth(int id) const {
    return tabWidths_[std::find(std::begin(viewTabs), std::end(viewTabs), id) - std::begin(viewTabs)];
}

void App::layout() {
    rebuildSections();
    placeControls();
}

// Rebuilds both scrolling pages from the current state (they are painted from this cache).
// Returns true when a row appeared, vanished or moved, or a card or control changed place or size.
bool App::rebuildSections() {
    bool moved = false;
    for (const Page page : {Page::Recording, Page::Settings}) {
        const auto index = static_cast<size_t>(page);
        auto fresh = buildSections(page);
        const auto& old = pages_[index];
        moved = moved || old.size() != fresh.size() ||
            !std::equal(old.begin(), old.end(), fresh.begin(), [](const Section& a, const Section& b) {
                return a.x == b.x && a.width == b.width && a.top == b.top && a.bottom == b.bottom &&
                    std::equal(a.rows.begin(), a.rows.end(), b.rows.begin(), b.rows.end(), [](const Row& r, const Row& s) {
                        return r.y == s.y && r.height == s.height && r.ids == s.ids && r.placed == s.placed;
                    });
            });
        int bottom = 0;
        for (const auto& section : fresh) bottom = std::max(bottom, section.bottom);
        pageHeights_[index] = bottom + pageTop;
        pages_[index] = std::move(fresh);
    }
    return moved;
}

// After a state change: move controls only if a row or control moved, otherwise repaint the window.
void App::refreshPages() {
    if (rebuildSections() || saveShown_ != (recording_ != capture::RecorderState::Off)) placeControls();
    else InvalidateRect(window_, nullptr, FALSE);
}

void App::placeControls() {
    hideGateBubble(); // Controls may move (scrolling, resizing, a new page); the bubble is anchored to where one was.
    RECT client;
    GetClientRect(window_, &client);
    const int w = MulDiv(client.right, 96, static_cast<int>(dpi_));
    const int bottom = footerTop();
    const int left = contentLeft();
    const int contentWidth = w - left - 36;
    for (const Page page : {Page::Recording, Page::Settings}) // The window may have grown.
        scroll_[static_cast<int>(page)] = std::clamp(scroll_[static_cast<int>(page)], 0, scrollMax(page));
    auto positions = BeginDeferWindowPos(idCount);
    std::bitset<idCount> placed;
    const auto place = [&](int id, int x, int y, int width, int height, bool shown = true) {
        const auto* found = entry(id);
        if (!found || !found->window) return;
        placed.set(static_cast<size_t>(id - firstId));
        // Row controls are in page coordinates; move them with their scrolled page.
        // A control reaching under the footer hides, so it never draws over it.
        if (!found->global && found->page != Page::Library) {
            y -= scroll_[static_cast<int>(found->page)];
            shown = shown && y + height <= bottom;
        }
        // The clip grid shows only with clips to show; empty states are painted instead.
        // An open clip replaces the grid and its actions; fullscreen shows only the player.
        const bool grid = contains(gridIds, id);
        const bool visible = fullscreen_.on ? id == Player : shown &&
            (found->global || found->page == state_.page) &&
            (id != Clips || clipGrid_.count() > 0) && !(grid && playerOpen_) && (id != Player || playerOpen_);
        const UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
        RECT bounds{px(x), px(y), px(x + width), px(y + height)};
        if (fullscreen_.on && id == Player) bounds = client;
        if (positions) positions = DeferWindowPos(positions, found->window, nullptr, bounds.left, bounds.top,
            bounds.right - bounds.left, bounds.bottom - bounds.top, flags);
        else SetWindowPos(found->window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, flags);
    };
    // Sidebar: collapsed, the buttons become icon squares. The collapse handle straddles the sidebar's edge, mid-height.
    const bool collapsed = state_.sidebarCollapsed;
    const int navX = collapsed ? 12 : 16, navWidth = sidebarWidth() - 2 * navX;
    place(SidebarToggle, sidebarWidth() - handleWidth / 2, (bottom - handleHeight) / 2, handleWidth, handleHeight);
    place(Library, navX, 130, navWidth, 46);
    place(Recording, navX, 184, navWidth, 46);
    place(Settings, navX, 238, navWidth, 46);
    // At the sidebar's foot (the footer shows the state): save above stop while there is a replay to save.
    saveShown_ = recording_ != capture::RecorderState::Off;
    place(SaveReplay, navX, bottom - 108, navWidth, 42, saveShown_);
    place(StartReplay, navX, bottom - 58, navWidth, 42);
    // Library: filters and view buttons on the top row; the tiles fill the card below it,
    // inset so its rounded border stays visible. Tile size changes only the tiles.
    const int filter = libraryRowHeight;
    // Folder-style tabs standing on the card: the chosen one is a pixel taller so it joins the card.
    int tabX = left + 8;
    for (int i = 0; i < 3; ++i) {
        const bool chosen = viewTabs[i] == tabFor(state_.clipView);
        place(viewTabs[i], tabX, libraryRowTop, tabWidth(viewTabs[i]), chosen ? filter + 1 : filter);
        // A divider follows between two tabs that are both unchosen (painted).
        tabX += tabWidth(viewTabs[i]) + (i < 2 && tabDivider(state_.clipView, i) ? 1 : 0);
    }
    // The two tile sizes as a pair of square icon buttons at the right end, the folders toggle before them.
    place(SmallTiles, w - contentGap - tileButton, tileButtonTop, tileButton, tileButton);
    place(LargeTiles, w - contentGap - tileGap - 2 * tileButton, tileButtonTop, tileButton, tileButton);
    place(GroupFolders, w - contentGap - libraryButtonsWidth, tileButtonTop, tileButton, tileButton);
    // The search field ends at the content edge on the title's line; its frame and magnifier are painted around it.
    place(ClipSearch, w - contentGap - searchFramePad - searchWidth, searchTop, searchWidth, searchHeight);
    // Under the card: the folder button at the right edge, the folder size (painted) to its left.
    place(OpenFolder, left + contentWidth - footerButton, bottom - 38, footerButton, footerButton);
    place(Clips, left + 8, libraryCardTop + 8, contentWidth - 16, bottom - libraryCardBottom - libraryCardTop - 16);
    place(Player, left, 24, contentWidth, bottom - 48);
    // Recording and Settings: each row's controls line up from the right end of the row (see buildSections);
    // controls of rows that do not apply right now are hidden.
    for (const Page page : {Page::Recording, Page::Settings}) {
        for (const auto& section : pages_[static_cast<size_t>(page)]) {
            for (const auto& row : section.rows) {
                for (const auto& slot : row.placed) {
                    place(slot.id, left + slot.x, row.y + (row.height - rowSlot(slot.id).height) / 2, slot.width, rowSlot(slot.id).height);
                }
            }
        }
    }
    for (int id = firstId; id <= LastId; ++id) {
        const auto* c = entry(id);
        if (c->window && !c->global && c->page != Page::Library && !placed.test(static_cast<size_t>(id - firstId)))
            place(id, 0, 0, 0, 0, false);
    }
    if (positions) EndDeferWindowPos(positions);
    // Publish page geometry and visibility together, then paint the complete tree.
    // Children are moved with SWP_NOREDRAW and the parent erases nothing, so this one pass shows the finished frame.
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    syncMeters(); // The page or window state decides whether meters run.
    updateMeterTooltip();
}

std::vector<App::Section> App::buildSections(Page page) const {
    const auto& filters = audio_.micFilters;
    std::vector<Section> result;
    RECT client;
    GetClientRect(window_, &client);
    const int available = MulDiv(client.right, 96, static_cast<int>(dpi_)) - contentLeft() - contentGap;
    // Lines the row's controls up from the right end of its card; the name and note get what is left of the left.
    const auto arrange = [](Row& row, int sectionX, int sectionWidth) {
        const int rowLeft = sectionX + 20, rowRight = sectionX + sectionWidth - 20;
        // A device menu gives way first (down to its minimum), then a slider takes what the row has left
        // after its name and other controls, up to its full width.
        int others = 0, sliderWant = 0;
        bool menu = false;
        for (const int id : row.ids) {
            const auto slot = rowSlot(id);
            if (contains(sliderIds, id)) sliderWant += slot.lead + slot.width + slot.trail;
            else if (id == DesktopAudio || id == Microphone) menu = true;
            else others += slot.lead + slot.width + slot.trail;
        }
        const int free = rowRight - rowLeft - nameWidth - others;
        const int menuWidth = menu ? std::clamp(free - sliderWant, deviceMenuMinimum, rowSlot(DesktopAudio).width) : 0;
        const int room = free - menuWidth;
        row.placed.assign(row.ids.size(), {});
        int right = rowRight;
        for (size_t i = row.ids.size(); i-- > 0;) {
            const int id = row.ids[i];
            auto slot = rowSlot(id);
            if (contains(sliderIds, id)) slot.width = std::clamp(room - slot.lead - slot.trail, 100, slot.width);
            else if (id == DesktopAudio || id == Microphone) slot.width = menuWidth;
            const int x = right - slot.trail - slot.width;
            row.placed[i] = {id, x, slot.width};
            right = x - slot.lead;
        }
        // Names and notes end before the row's first control (and a number field's caption).
        row.textWidth = row.placed.front().x - rowSlot(row.ids.front()).lead - sectionX - 36;
    };
    // Places a section's heading and rows from `top`; returns the card's bottom.
    const auto stack = [&](Section& section, int top, int x, int width) {
        section.top = top;
        section.x = x;
        section.width = width;
        int y = top + headingHeight;
        for (auto& row : section.rows) {
            row.y = y;
            row.height = row.note.empty() ? rowHeight : notedRowHeight;
            arrange(row, x, width);
            y += row.height;
        }
        section.bottom = y;
        return y;
    };

    if (page == Page::Settings) {
        // Assignment hints first, then the last problem, then the active shortcut.
        const auto held = heldModifiersText(heldModifiers_);
        const auto attention = [&](int shortcut) {
            return (assigning_ && assigningShortcut_ == shortcut) || !hotkeyNotes_[shortcut].empty() || !hotkeyRegistered_[shortcut];
        };
        const auto shortcutNote = [&](int shortcut) -> std::wstring {
            const auto& note = hotkeyNotes_[shortcut];
            if (assigning_ && assigningShortcut_ == shortcut && note.empty())
                return held.empty() ? L"press the new shortcut · esc cancels · backspace restores the default" : held + L" · esc cancels";
            if (!note.empty()) return note;
            if (hotkeyRegistered_[shortcut]) return describe(hotkeys_[shortcut]);
            return describe(hotkeys_[shortcut]) + (shortcut == saveShortcut ? L" is taken by another app · assign another or use save" :
                L" is taken by another app · assign another");
        };
        result.push_back({L"saving", {
            Row{L"clip folder", lower(clipFolder_.wstring()), false, {ChooseFolder}},
            Row{L"save shortcut", shortcutNote(saveShortcut), attention(saveShortcut), {AssignHotkey}},
            Row{L"screenshot shortcut", shortcutNote(screenshotShortcut), attention(screenshotShortcut), {AssignScreenshot}},
        }});
        const wchar_t* startupNote = startupReadFailed_ ? L"windows startup settings could not be read" :
            startupOtherCopy_ ? L"another copy of refarter starts with windows" :
            state_.startWithWindows ? L"windows startup apps can override this" : L"opens refarter when you sign in";
        const std::wstring fullNote = state_.storageFull == StorageFull::Warn ? L"saves still go ahead, with a warning" :
            state_.storageFull == StorageFull::Refuse ? L"nothing is saved; the replay is kept" : L"oldest clips go to the bin, not favourites or compressed ones";
        result.push_back({L"storage", {
            Row{L"ask before binning", state_.askBeforeBinning ? L"the library asks before moving clips to the recycle bin" : L"clips go to the recycle bin without asking", false, {AskBeforeBinning}},
            Row{L"max storage for clips", state_.maxStorageGb ? L"the strip under the library shows how much is used" : L"empty or 0 is no limit", false, {StorageLimitField}},
            Row{L"when storage is full", state_.storageFullChosen ? fullNote : L"not chosen yet · asked once when a save would pass the limit", false, {StorageBehaviour}},
        }});
        result.push_back({L"trimming", {
            Row{L"compress & copy after trim", L"the trim bar's checkbox starts ticked", false, {CompressCopyAfterTrim}},
        }});
        result.push_back({L"sound", {
            Row{L"save chime", L"plays when a save starts or fails", false, {ChimeVolume, ChimeVolumeLabel}},
        }});
        result.push_back({L"app", {
            Row{L"close to tray", L"closing the window keeps replay and the shortcut running", false, {CloseToTray}},
            Row{L"start with windows", startupNote, startupReadFailed_ || startupOtherCopy_, {StartWithWindows}},
            Row{L"start minimised", L"starts in the tray instead of opening a window", false, {StartMinimised}},
            Row{L"diagnostic logging", L"local session events · up to 6 mb", false, {DiagnosticLogging}},
            Row{L"updates", updateRowNote(), false, {UpdateCheckNow, UpdateCheckSwitch}},
        }});
        const int width = std::min(settingsMaxWidth, available);
        int y = sectionsTop;
        for (auto& section : result) y = stack(section, y, 0, width) + sectionGap;
        return result;
    }

    // Recording: what gets captured and how. Everything here is locked while replay runs.
    const bool off = recording_ == capture::RecorderState::Off;
    const bool waitingForGame = !target_.source.window && !target_.source.monitor && autoCaptureOn_ && autoCaptureReadable_;
    const auto& sourceLabel = target_.displayName.empty() ? target_.name : target_.displayName;
    // Routine prompts are left out; the row's note then says what kind of source it is.
    const std::wstring sourceNote = !statusRoutine_ ? status_ : target_.source.monitor ? L"whole monitor" :
        target_.source.window ? L"game window" : waitingForGame ? L"waiting for a listed game" : L"a game window or a whole monitor";
    const auto duration = formatReplayDuration(state_.replaySeconds);
    const auto replayRange = rangeText(capture::minReplaySeconds, capture::maxReplaySeconds);
    Section replay{L"replay", {
        Row{target_.name.empty() ? (waitingForGame ? L"auto-capture" : L"no source selected") : lower(sourceLabel), sourceNote,
            recording_ == capture::RecorderState::Buffering && !statusRoutine_, {SelectProcess}},
        Row{L"auto-capture", L"starts replay for listed games", false, {AutoCaptureSource}},
        Row{L"replay length", off ? replayRange + L" seconds" : L"stop replay to change settings", false,
            {ThirtySeconds, OneMinute, ThreeMinutes, FiveMinutes}},
        Row{L"custom length", state_.replayInputValid ? L"= " + duration : L"enter " + replayRange + L" seconds · kept " + duration,
            !state_.replayInputValid, {CustomSeconds}},
    }};

    Section video{L"video", {
        Row{L"resolution", L"", false, {Resolution}},
        Row{L"frame rate", L"", false, {FrameRate}},
        Row{L"quality", estimateText(), false, {QualityPreset}},
    }};
    const auto bitrateRange = rangeText(capture::minBitrateMbps, capture::maxBitrateMbps);
    if (video_.quality == capture::Quality::Custom)
        video.rows.push_back(Row{L"bitrate", bitrateValid_ ? L"constant, " + bitrateRange : L"enter " + bitrateRange + L" mbps", !bitrateValid_, {Bitrate}});
    video.rows.push_back(Row{L"codec", L"", false, {VideoCodec}});

    // A missing device keeps recording silence; say so on its row while replay runs.
    const bool buffering = recording_ == capture::RecorderState::Buffering;
    const wchar_t* missing = L"unavailable · recording silence until it returns";
    // Only problems get a note here; the audio card stays quiet otherwise.
    const wchar_t* discordNote = discordUnavailable_ ? L"this windows version cannot record it apart, so it stays in desktop audio" :
        discordSharesOutput_ ? L"it plays on this output too · choose another output in discord for its own track" : L"";
    // Each source with its volume beside it, then Discord and the microphone filters.
    // Microphone filters appear once a microphone is chosen; gate values once the gate is on.
    Section audio{L"audio", {
        Row{L"desktop audio", buffering && desktopMissing_ ? missing : L"", buffering && desktopMissing_,
            {DesktopVolume, DesktopVolumeLabel, DesktopAudio, DesktopReset}},
        Row{L"microphone", buffering && microphoneMissing_ ? missing : L"", buffering && microphoneMissing_,
            {MicrophoneVolume, MicrophoneVolumeLabel, Microphone, MicrophoneReset}},
        Row{L"record discord", discordNote, discordUnavailable_ || discordSharesOutput_, {RecordDiscord}},
    }};
    const int width = std::min(recordingMaxWidth, available);
    if (audio_.microphone.enabled()) {
        audio.rows.push_back(Row{L"noise suppression", L"", false, {Suppression}});
        audio.rows.push_back(Row{L"noise gate", L"", false, {Gate}});
        if (filters.noiseGate) {
            // No note: a note makes the row taller and pushes the layout. What is wrong shows in a bubble above the
            // field (see syncGateBubble), and the field itself takes the accent outline.
            // All five values on one row when it fits beside the name; otherwise levels, then times.
            int fields = 0;
            for (const auto& field : gateFields) fields += rowSlot(field.id).lead + rowSlot(field.id).width + rowSlot(field.id).trail;
            if (width - 40 - fields >= 120) {
                audio.rows.push_back(Row{L"gate", L"", false, {GateOpen, GateClose, GateAttack, GateHold, GateRelease}});
            } else {
                audio.rows.push_back(Row{L"gate levels", L"", false, {GateOpen, GateClose}});
                audio.rows.push_back(Row{L"gate timing", L"", false, {GateAttack, GateHold, GateRelease}});
            }
        }
    }

    // Replay and video side by side when there is room, audio across both below;
    // a narrow page stacks all three.
    if (width >= twoColumnWidth) {
        const int column = (width - columnGap) / 2;
        const int below = std::max(stack(replay, sectionsTop, 0, column), stack(video, sectionsTop, column + columnGap, width - column - columnGap));
        stack(audio, below + sectionGap, 0, width);
    } else {
        stack(audio, stack(video, stack(replay, sectionsTop, 0, width) + sectionGap, 0, width) + sectionGap, 0, width);
    }
    result.push_back(std::move(replay));
    result.push_back(std::move(video));
    result.push_back(std::move(audio));
    return result;
}

std::wstring App::estimateText() const {
    // Clip size at the average rate (calm scenes come out smaller); memory at the peak rate.
    const auto rate = capture::bitrateFor(video_, capture::outputSize(video_.resolution, sourceSize()));
    // Discord gets its own track unless hidden or inseparable (only clips where it spoke).
    const bool discord = audio_.desktop.enabled() && !audio_.hideDiscord && !discordUnavailable_ && !discordSharesOutput_;
    const int sources = audio_.desktop.enabled() + audio_.microphone.enabled() + discord + gameTrackOn();
    const auto clip = estimatedClipBytes();
    // The video buffer keeps the same headroom as the audio rings (see Recorder::start).
    const auto memory = capture::replayBytes(rate.peakKbps, capture::AudioTrack::ringSeconds(state_.replaySeconds)) +
        static_cast<std::uint64_t>(sources) * capture::AudioTrack::ringBytes(state_.replaySeconds); // One PCM ring per source.
    // Short enough for a half-width card: ≈ for a constant rate, ≤ for presets that vary.
    return (rate.constant ? L"clip ≈ " : L"clip ≤ ") + std::to_wstring(clip / 1'000'000) + L" mb · ram ≤ " +
        std::to_wstring(memory / 1'000'000) + L" mb";
}

int App::scrollMax(Page page) const {
    if (page == Page::Library) return 0;
    return std::max(0, pageHeight(page) - footerTop());
}

void App::scrollPage(int delta) {
    auto& scroll = scroll_[static_cast<int>(state_.page)];
    const int previous = scroll;
    scroll = std::clamp(scroll + delta, 0, scrollMax(state_.page));
    if (scroll != previous) placeControls(); // Only the positions change.
}

void App::reveal(HWND child) {
    const auto* found = entry(GetDlgCtrlID(child));
    if (!found || found->window != child || found->global || found->page == Page::Library || found->page != state_.page) return;
    RECT box{};
    GetWindowRect(child, &box);
    MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&box), 2);
    const int dpi = static_cast<int>(dpi_);
    const int top = MulDiv(box.top, 96, dpi) - pageTop;
    const int bottom = MulDiv(box.bottom, 96, dpi) + pageTop - footerTop();
    if (top < 0) scrollPage(top);
    else if (bottom > 0) scrollPage(bottom);
}

} // namespace refarter
