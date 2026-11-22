// App: where everything goes. Sizes, the Recording and Settings rows, control placement and scrolling.
#include "app_internal.h"
#include "replay_duration.h"
#include "text.h"
#include "version.h"
#include "capture/audio_levels.h"
#include "capture/audio_track.h"

#include <algorithm>
#include <bitset>

namespace refarter {
namespace {
constexpr int handleWidth = 24, handleHeight = 48; // The sidebar's collapse handle.
// Library controls replaced by the player while a clip is open.
constexpr int gridIds[] = {AllClips, Favourites, Compressed, SortClips, GroupFolders, LargeTiles, SmallTiles, OpenFolder, Clips, ClipSearch};
// "2–250" for a range of whole numbers.
std::wstring rangeText(int low, int high) { return std::to_wstring(low) + L"–" + std::to_wstring(high); }
constexpr int sectionsTop = 68;          // The first card's heading, under the page title.
constexpr int settingsMaxWidth = 800;    // Settings rows stay readable in a wide window.
constexpr int twoColumnWidth = 800;      // Narrower pages stack Recording's cards in one column.
constexpr int columnGap = 16;
constexpr int nameWidth = 150;           // Kept free for a row's name before sliders shrink.
constexpr int deviceMenuMinimum = 150;  // The device menus shrink to this before a slider does.
constexpr int sectionGap = 28;
constexpr int rowHeight = 50, notedRowHeight = 60, detailRowHeight = 76; // The last: a note and a detail line.
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
        if (page == Page::Recording && window_) measureRecordingHeadings(fresh);
        pageHeights_[index] = bottom + pageTop;
        pages_[index] = std::move(fresh);
    }
    // A reset arrow keeps its slot when hidden, so a game setting or clearing a value moves nothing but still needs
    // the controls placed again to show or hide it. "use defaults" on the title line is outside the rows: it takes
    // the bit after the arrows.
    int resets = 0;
    for (int i = 0; i < static_cast<int>(std::size(videoResetIds)); ++i) if (scopeOwns(videoResetIds[i])) resets |= 1 << i;
    if (scopeEntry()) resets |= 1 << std::size(videoResetIds);
    if (renameShown()) resets |= 1 << (std::size(videoResetIds) + 1); // The rename pencil moves "settings for".
    moved = moved || resets != shownResets_;
    shownResets_ = resets;
    return moved;
}

// The locked note sits after each heading's title; measured here so painting does not measure text.
void App::measureRecordingHeadings(std::vector<Section>& sections) {
    const HDC dc = GetDC(window_);
    const auto previous = SelectObject(dc, fontOf(Font::Heading));
    const int dpi = static_cast<int>(dpi_);
    for (auto& section : sections) {
        SIZE title{};
        GetTextExtentPoint32W(dc, section.title, static_cast<int>(wcslen(section.title)), &title);
        section.titleWidth = MulDiv(title.cx, 96, dpi);
    }
    SelectObject(dc, fontOf(Font::Small));
    const auto measure = [&](const wchar_t* text) {
        SIZE extent{};
        GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &extent);
        return MulDiv(extent.cx, 96, dpi);
    };
    lockedNoteWidth_ = measure(lockedHeadingNote());
    lockedWordWidth_ = measure(L"locked");
    for (auto& section : sections) {
        section.lockNoteWidth = 0;
        section.shortLockNote = false;
        if (!std::any_of(section.rows.begin(), section.rows.end(), [&](const Row& row) { return rowLockedByReplay(row); })) continue;
        const int room = section.width - 8 - section.titleWidth - 10;
        // Keep space for the game's name/shared-audio aside instead of painting two notes over each other.
        const int asideRoom = section.aside.empty() ? 0 : 64;
        if (room >= lockedNoteWidth_ + asideRoom) section.lockNoteWidth = lockedNoteWidth_;
        else if (room >= lockedWordWidth_) {
            section.lockNoteWidth = lockedWordWidth_;
            section.shortLockNote = true;
        }
    }
    SelectObject(dc, previous);
    ReleaseDC(window_, dc);
}

// After a state change: move controls only if a row or control moved, otherwise repaint the window.
void App::refreshPages() {
    if (rebuildSections() || saveShown_ != (recording_ != capture::RecorderState::Off)) placeControls();
    else {
        InvalidateRect(window_, nullptr, FALSE);
        updateLockedTooltips(); // Which rows are locked can change without moving a row (record discord).
    }
}

void App::placeControls() {
    hideGateBubble(); // Controls may move (scrolling, resizing, a new page); the bubble is anchored to where one was.
    RECT client;
    GetClientRect(window_, &client);
    const int w = MulDiv(client.right, 96, static_cast<int>(dpi_));
    const int bottom = footerTop();
    const int left = contentLeft();
    const int contentWidth = w - left - contentGap;
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
        // The welcome screen shows only its own controls and the (disabled) sidebar.
        const bool welcomeControl = contains(welcomeIds, id);
        const bool visible = fullscreen_.on ? id == Player : welcome_ ? shown && (welcomeControl || (found->global && id != SidebarToggle)) : shown && !welcomeControl &&
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
    // The replay buttons are 4 px wider each side so "clear retained replay" fits the 196 px sidebar.
    const int replayX = collapsed ? navX : 12, replayWidth = sidebarWidth() - 2 * replayX;
    place(SaveReplay, replayX, bottom - 108, replayWidth, 42, saveShown_);
    place(StartReplay, replayX, bottom - 58, replayWidth, 42);
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
    place(GroupFolders, w - contentGap - folderButtonOffset, tileButtonTop, tileButton, tileButton);
    place(SortClips, w - contentGap - libraryButtonsWidth, tileButtonTop, tileButton, tileButton);
    // The search field ends at the content edge on the title's line; its frame and magnifier are painted around it.
    place(ClipSearch, w - contentGap - searchFramePad - searchWidth, searchTop, searchWidth, searchHeight);
    // Under the card: the folder button at the right edge, the folder size (painted) to its left.
    place(OpenFolder, left + contentWidth - footerButton, bottom - 38, footerButton, footerButton);
    place(Clips, left + 8, libraryCardTop + 8, contentWidth - 16, bottom - libraryCardBottom - libraryCardTop - 16);
    place(Player, left, 24, contentWidth, bottom - 48);
    // The welcome card: each row's control at its right end, then skip and start under the card.
    const auto welcome = welcomeBox();
    const int rowRight = welcome.x + welcome.width - 16;
    const auto rowMiddle = [&](int row, int height) { return welcome.cardTop + row * welcome.rowHeight + (welcome.rowHeight - height) / 2; };
    place(WelcomeManual, rowRight - 92, rowMiddle(0, 32), 92, 32);
    place(WelcomeAuto, rowRight - 92 - 4 - 118, rowMiddle(0, 32), 118, 32);
    place(WelcomeLength, rowRight - 140, rowMiddle(1, 34), 140, 34);
    place(WelcomeShortcut, rowRight - 180, rowMiddle(2, 34), 180, 34);
    const int folderWidth = std::max(120, welcome.width - 16 - 110 - 16); // After the "clip folder" name.
    place(WelcomeFolder, rowRight - folderWidth, rowMiddle(3, 34), folderWidth, 34);
    place(WelcomeStartup, rowRight - 40, rowMiddle(4, 22), 40, 22);
    place(WelcomeTray, rowRight - 40, rowMiddle(5, 22), 40, 22);
    place(WelcomeUpdates, rowRight - 40, rowMiddle(6, 22), 40, 22);
    place(WelcomeStart, welcome.x + welcome.width - 96, welcome.buttonsTop, 96, 36);
    place(WelcomeSkip, welcome.x + welcome.width - 96 - 10 - 80, welcome.buttonsTop, 80, 36);
    // Recording's title line: "settings for" ends at the cards' right edge, "use defaults" before it while the chosen
    // game has its own values. In page coordinates, so they scroll with the title.
    const int titleRight = left + std::min(recordingMaxWidth, contentWidth);
    // While the page edits one game, the rename pencil ends the line and "settings for" moves left of it.
    const bool rename = renameShown();
    const auto pencil = rowSlot(RenameGame);
    place(RenameGame, titleRight - pencil.width, titleControlsTop(), pencil.width, pencil.height, rename);
    const int settingsForWidth = rowSlot(SettingsFor).width;
    const int settingsForRight = titleRight - (rename ? pencil.lead + pencil.width : 0);
    place(SettingsFor, settingsForRight - settingsForWidth, titleControlsTop(), settingsForWidth, rowSlot(SettingsFor).height);
    const auto defaults = rowSlot(UseDefaults);
    place(UseDefaults, titleRight - titleControlsWidth(true, rename) + defaults.lead, titleControlsTop(), defaults.width, defaults.height,
        scopeEntry() != nullptr);
    // Recording and Settings: each row's controls line up from the right end of the row (see buildSections);
    // controls of rows that do not apply right now are hidden.
    for (const Page page : {Page::Recording, Page::Settings}) {
        for (const auto& section : pages_[static_cast<size_t>(page)]) {
            for (const auto& row : section.rows) {
                for (const auto& slot : row.placed) {
                    // A value the chosen game inherits keeps its reset arrow's room empty, so the rows stay aligned.
                    const bool shown = !contains(videoResetIds, slot.id) || scopeOwns(slot.id);
                    place(slot.id, left + slot.x, row.y + (row.height - rowSlot(slot.id).height) / 2, slot.width, rowSlot(slot.id).height, shown);
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
    updateLockedTooltips();
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
            row.height = row.note.empty() && !row.noteRoom ? rowHeight : row.detail.empty() ? notedRowHeight : detailRowHeight;
            arrange(row, x, width);
            y += row.height;
        }
        section.bottom = y;
        return y;
    };

    if (page == Page::Settings) {
        // The button shows the keys; the note only the assignment hint or a problem.
        Section saving{L"saving", {
            Row{L"clip folder", lower(clipFolder_.wstring()), false, {ChooseFolder}},
            Row{L"save shortcut", shortcutNote(saveShortcut), shortcutAlert(saveShortcut), {AssignHotkey}},
            Row{L"screenshot shortcut", shortcutNote(screenshotShortcut), shortcutAlert(screenshotShortcut), {AssignScreenshot}},
        }};
        saving.rows[1].noteRoom = saving.rows[2].noteRoom = true; // Assigning shows a hint.
        result.push_back(std::move(saving));
        // Notes carry only values and problems; a switch or menu already shows its state.
        const wchar_t* startupNote = startupReadFailed_ ? L"windows startup settings could not be read" :
            startupOtherCopy_ ? L"another copy of refarter starts with windows" : L"";
        result.push_back({L"storage", {
            Row{L"ask before binning", L"", false, {AskBeforeBinning}},
            Row{L"max storage for clips", L"0 = no limit", false, {StorageLimitField}},
            Row{L"when storage is full", state_.storageFullChosen ? L"" : L"not chosen yet", false, {StorageBehaviour}},
        }});
        result.push_back({L"trimming", {
            Row{L"compress & copy after trim", L"", false, {CompressCopyAfterTrim}},
        }});
        result.push_back({L"library", {
            Row{L"game colours", L"", false, {GameColours}},
        }});
        result.push_back({L"sound", {
            Row{L"save chime", L"", false, {ChimeVolume, ChimeVolumeLabel}},
        }});
        result.push_back({L"auto-capture", {
            Row{L"start listed games automatically", L"", false, {AutoCaptureStart}},
            Row{L"offer new fullscreen games", L"", false, {AutoCaptureOffers}},
        }});
        result.push_back({L"app", {
            Row{L"close to tray", L"", false, {CloseToTray}},
            Row{L"start with windows", startupNote, startupReadFailed_ || startupOtherCopy_, {StartWithWindows}},
            Row{L"start minimised", L"", false, {StartMinimised}},
            // Always a note, so choosing a source never changes the row's height.
            Row{L"start replay", sourceMonitor_.empty() ? L"needs a monitor as the source" : L"records the chosen monitor from sign-in",
                state_.signInReplay && sourceMonitor_.empty(), {SignInReplay}},
            Row{L"short game names", L"shorten game names instead of showing the whole window title", false, {ShortGameNames}},
            Row{L"diagnostic logging", L"", false, {DiagnosticLogging}},
            Row{L"updates", updateRowNote(), false, {ShowNotes, UpdateCheckNow, UpdateCheckSwitch}},
        }});
        const int width = std::min(settingsMaxWidth, available);
        int y = sectionsTop;
        for (auto& section : result) y = stack(section, y, 0, width) + sectionGap;
        return result;
    }

    // Recording: what gets captured and how. Everything here but the auto-capture list is locked while replay runs;
    // each heading says so (app_paint.cpp) and each locked row has a tooltip (updateLockedTooltips).
    const bool waitingForGame = !target_.source.window && !target_.source.monitor && autoCaptureOn_ && autoCaptureReadable_;
    const auto sourceLabel = shownSourceName(target_); // Lower case and cut, as the footer shows it.
    // Routine prompts are left out; then only waiting for a listed game gets a note.
    const std::wstring sourceNote = !statusRoutine_ ? status_ : waitingForGame ? L"waiting for a listed game" : L"";
    // "settings for" (on the title line, see placeControls): which scope the replay length and video rows edit, all
    // games or one game's own values.
    const bool gameChosen = gameScope();
    const auto shown = scopeVideo(); // The rows show the scope's effective values.
    const auto chosenLabel = shownGameName(settingsFor_, settingsForLabel_);
    // With a game chosen, a row it sets says what all games use (its value is orange); an inherited row keeps its usual
    // note. The reset arrow sits before the control (hidden, its room kept, while inherited).
    const auto valueNote = [&](int reset, const std::wstring& allGames, std::wstring usual = {}) -> std::wstring {
        return gameChosen && scopeOwns(reset) ? L"all games: " + allGames : usual;
    };
    const auto withReset = [&](std::vector<int> ids, int reset) {
        if (gameChosen) ids.insert(ids.begin(), reset);
        return ids;
    };
    const auto duration = formatReplayDuration(shown.replaySeconds);
    const auto replayRange = rangeText(capture::minReplaySeconds, capture::maxReplaySeconds);
    Section replay{L"replay", {
        Row{target_.name.empty() ? (waitingForGame ? L"auto-capture" : L"no source selected") : sourceLabel, sourceNote,
            recording_ == capture::RecorderState::Buffering && !statusRoutine_, {SelectProcess}},
        Row{L"auto-capture", L"", false, {AutoCaptureSource}},
        Row{L"replay length", valueNote(ReplayReset, formatReplayDuration(state_.replaySeconds), replayRange + L" seconds"), false,
            {ThirtySeconds, OneMinute, ThreeMinutes, FiveMinutes}},
        Row{L"custom length", state_.replayInputValid ? L"= " + duration : L"enter " + replayRange + L" seconds · kept " + duration,
            !state_.replayInputValid, withReset({CustomSeconds}, ReplayReset)},
    }};
    replay.rows[0].noteRoom = true; // The status comes and goes on the source row.

    Section video{L"video", {
        Row{L"resolution", valueNote(ResolutionReset, resolutionNames[static_cast<int>(video_.resolution)]), false,
            withReset({Resolution}, ResolutionReset)},
        Row{L"frame rate", valueNote(FrameRateReset, std::to_wstring(video_.fps) + L" fps"), false, withReset({FrameRate}, FrameRateReset)},
        Row{L"quality", estimateText(), false, withReset({QualityPreset}, QualityReset)},
    }};
    if (gameChosen && scopeOwns(QualityReset)) {
        // The game's note first, the estimate on a second line.
        auto& quality = video.rows.back();
        quality.detail = std::move(quality.note);
        quality.note = valueNote(QualityReset, qualityNames[static_cast<int>(video_.quality)]);
    }
    // What the card's values belong to; an unreadable per-game file leaves "settings for" on all games, so say why.
    if (!gameSettingsReadable_) video.aside = L" · per-game settings could not be read";
    else if (gameChosen) video.aside = L" · " + chosenLabel;
    const auto bitrateRange = rangeText(capture::minBitrateMbps, capture::maxBitrateMbps);
    if (shown.quality == capture::Quality::Custom)
        video.rows.push_back(Row{L"bitrate", !bitrateValid_ ? L"enter " + bitrateRange + L" mbps" :
            valueNote(BitrateReset, std::to_wstring(video_.bitrateMbps) + L" mbps", L"constant, " + bitrateRange),
            !bitrateValid_, withReset({Bitrate}, BitrateReset)});
    // A game's own codec that this PC's check refused records h.264: the row says so (the menu keeps the choice).
    std::optional<capture::Codec> refusedCodec;
    if (gameChosen) scopeRecordVideo(&refusedCodec);
    video.rows.push_back(Row{L"codec", refusedCodec ? std::wstring(capture::codecName(*refusedCodec)) + L" can't record on this pc · records h.264" :
        valueNote(CodecReset, capture::codecName(video_.codec)), refusedCodec.has_value(), withReset({VideoCodec}, CodecReset)});

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

    if (gameChosen) audio.aside = L" · shared by all games";

    // Replay and video side by side when there is room, audio across both below; a narrow page stacks all three.
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

std::wstring App::shortcutNote(int shortcut) const {
    const auto& note = hotkeyNotes_[shortcut];
    if (!note.empty()) return note;
    if (assigning_ && assigningShortcut_ == shortcut) return L"esc cancels · backspace resets";
    if (hotkeyRegistered_[shortcut]) return watchedOnly(hotkeys_[shortcut]) ? L"works while replay runs, still types in other apps" : L"";
    // The welcome screen has no save button to point to.
    return shortcut == saveShortcut && !welcome_ ? L"taken by another app · assign another or use save" : L"taken by another app · assign another";
}

bool App::shortcutAlert(int shortcut) const {
    return (assigning_ && assigningShortcut_ == shortcut) || !hotkeyNotes_[shortcut].empty() || !hotkeyRegistered_[shortcut];
}

// The welcome screen: a panel holding the title, the box of rows, a line for a shortcut problem, then skip and
// start. Centred in the content area, never above the top margin.
App::WelcomeBox App::welcomeBox() const {
    RECT client{};
    GetClientRect(window_, &client);
    const int w = MulDiv(client.right, 96, static_cast<int>(dpi_));
    const int available = w - contentLeft() - contentGap;
    constexpr int pad = 28, title = 40, titleGap = 14, noteGap = 6, note = 20, buttonsGap = 6, buttons = 36;
    const int panelWidth = std::min(welcomeMaxWidth + 2 * pad, available);
    constexpr int fixed = pad + title + titleGap + noteGap + note + buttonsGap + buttons + pad;
    const int welcomeRow = std::clamp((footerTop() - 2 * pageTop - fixed) / welcomeRowCount, welcomeMinRowHeight, welcomeRowHeight);
    const int panelHeight = fixed + welcomeRowCount * welcomeRow;
    WelcomeBox box{};
    box.rowHeight = welcomeRow;
    box.panelX = contentLeft() + std::max(0, (available - panelWidth) / 2);
    // Centred, never above the top margin unless only that keeps skip and start above the footer.
    box.panelTop = std::max({0, std::min(pageTop, footerTop() - panelHeight), (footerTop() - panelHeight) / 2});
    box.panelWidth = panelWidth;
    box.panelHeight = panelHeight;
    box.x = box.panelX + pad;
    box.width = panelWidth - 2 * pad;
    box.top = box.panelTop + pad;
    box.cardTop = box.top + title + titleGap;
    box.noteTop = box.cardTop + welcomeRowCount * welcomeRow + noteGap;
    box.buttonsTop = box.noteTop + note + buttonsGap;
    return box;
}

std::wstring App::estimateText() const {
    // For the scope the page edits (all games, or the chosen game's effective values).
    const auto video = scopeRecordVideo(); // What the scope would record with, a refused game codec as h.264.
    const int seconds = video.replaySeconds;
    const auto size = scopeSize().size; // The chosen game's window, not whatever the source is now.
    // Clip size at the average rate (calm scenes come out smaller); memory at the peak rate.
    const auto rate = capture::bitrateFor(video, encodedSize(video, size));
    // Discord gets its own track unless hidden or inseparable (only clips where it spoke).
    const bool discord = audio_.desktop.enabled() && !audio_.hideDiscord && !discordUnavailable_ && !discordSharesOutput_;
    const int stereoSources = audio_.desktop.enabled() + discord + gameTrackOn();
    const auto clip = estimatedClipBytes(seconds, &video, size);
    // One PCM ring per source, the microphone's mono.
    const auto micRing = audio_.microphone.enabled()
        ? capture::AudioTrack::ringBytes(seconds, capture::sourceChannels(capture::MicrophoneSource)) : 0;
    // The video buffer keeps the same headroom as the audio rings (see Recorder::start).
    const auto memory = capture::replayBytes(rate.peakKbps, capture::AudioTrack::ringSeconds(seconds)) +
        static_cast<std::uint64_t>(stereoSources) * capture::AudioTrack::ringBytes(seconds) + micRing;
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
