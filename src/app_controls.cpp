// App: the child controls. Creating and registering them, their subclass procedures, enabling and
// captioning them from state, number-field validation, tooltips and keyboard focus.
#include "app_internal.h"
#include "accessible_name.h"
#include "replay_duration.h"
#include "volume_input.h"
#include "text.h"
#include "popup_menu.h"
#include "capture/audio_capture.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <cassert>

namespace refarter {
namespace {
std::wstring deviceText(const wchar_t* source, const capture::AudioDevice& device) {
    using Mode = capture::AudioDevice::Mode;
    return std::wstring(source) + L" · " + (device.mode == Mode::Off ? L"off" : device.mode == Mode::SystemDefault ?
        L"system default" : device.name.empty() ? L"chosen device" : lower(device.name));
}
} // namespace

void App::updateReplayInput() {
    // EDIT can notify while CreateWindowEx is still returning, before registration; syncScopeFields sets it itself.
    if (!control(CustomSeconds) || syncingFields_) return;
    wchar_t text[16]{};
    GetWindowTextW(control(CustomSeconds), text, static_cast<int>(std::size(text)));
    const auto seconds = parseReplaySeconds(text);
    state_.replayInputValid = seconds.has_value();
    // Kept once the field is left: all games in refarter.ini (see saveSettings), a game in its file (commitScopeFields).
    // A game's invalid text drops what was typed, so leaving it keeps the stored length, not "6" on the way from 60.
    if (gameScope()) typedSeconds_ = seconds;
    else if (seconds) state_.replaySeconds = *seconds;
    for (int id = ThirtySeconds; id <= FiveMinutes; ++id) InvalidateRect(control(id), nullptr, FALSE);
    refreshControls();
    refreshPages(); // The length row's note and the video estimate follow.
}

void App::updateBitrateInput() {
    if (!control(Bitrate) || syncingFields_) return;
    wchar_t text[8]{};
    GetWindowTextW(control(Bitrate), text, static_cast<int>(std::size(text)));
    const auto value = parseNumber(text, capture::minBitrateMbps, capture::maxBitrateMbps);
    bitrateValid_ = value.has_value();
    if (gameScope()) typedBitrate_ = value; // Stored when the field is left, like the length; invalid text drops it.
    else if (value) video_.bitrateMbps = *value;
    refreshControls();
    refreshPages();
}

const App::Control* App::entry(int id) const {
    const int index = id - firstId;
    return index >= 0 && index < idCount ? &controls_[static_cast<size_t>(index)] : nullptr;
}

HWND App::control(int id) const {
    const auto* found = entry(id);
    return found ? found->window : nullptr;
}

void App::addButton(int id, const wchar_t* title, Page page, ButtonBackdrop backdrop, bool global) {
    auto button = CreateWindowExW(0, L"BUTTON", title,
        WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    controls_[static_cast<size_t>(id - firstId)] = {button, page, global, backdrop};
    SetWindowSubclass(button, buttonProc, 1, reinterpret_cast<DWORD_PTR>(this));
}

void App::createControls() {
    const auto volume = [&](int id, int labelId, const wchar_t* name, int percent, int maximum, Page page) {
        auto slider = CreateWindowExW(0, TRACKBAR_CLASSW, name,
            WS_CHILD | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        controls_[static_cast<size_t>(id - firstId)] = {slider, page};
        // We draw every slider part; native themed hover/pressed sprites must
        // never animate over the dark custom rail and orb.
        SetWindowTheme(slider, L"", L"");
        SetWindowSubclass(slider, volumeProc, 1, reinterpret_cast<DWORD_PTR>(this));
        SendMessageW(slider, TBM_SETRANGE, FALSE, MAKELPARAM(0, maximum));
        SendMessageW(slider, TBM_SETPAGESIZE, 0, 10);
        SendMessageW(slider, TBM_SETPOS, FALSE, percent);
        // The value beside the slider. A source's is an edit (created after the slider, so Tab reaches it next)
        // that looks like plain text until hovered or focused.
        const bool typed = contains(typedVolumeIds, labelId);
        auto title = CreateWindowExW(0, typed ? L"EDIT" : L"STATIC", L"",
            WS_CHILD | (typed ? WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL : SS_LEFT),
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(labelId)), GetModuleHandleW(nullptr), nullptr);
        controls_[static_cast<size_t>(labelId - firstId)] = {title, page};
        if (typed) {
            SendMessageW(title, EM_SETLIMITTEXT, 3, 0);
            SendMessageW(title, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
            SetWindowSubclass(title, editProc, 1, reinterpret_cast<DWORD_PTR>(this));
        }
    };
    addButton(SidebarToggle, L"collapse sidebar", Page::Library, ButtonBackdrop::Sidebar, true);
    addButton(Library, L"library", Page::Library, ButtonBackdrop::Sidebar, true);
    addButton(Recording, L"recording", Page::Recording, ButtonBackdrop::Sidebar, true);
    addButton(Settings, L"settings", Page::Settings, ButtonBackdrop::Sidebar, true);
    // In the sidebar so replay can be saved and switched on or off from every page.
    addButton(SaveReplay, L"save replay", Page::Recording, ButtonBackdrop::Sidebar, true);
    addButton(StartReplay, L"enable replay", Page::Recording, ButtonBackdrop::Sidebar, true);
    // Before the tabs, so Tab reaches it first on the library page.
    auto search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ClipSearch)), GetModuleHandleW(nullptr), nullptr);
    controls_[static_cast<size_t>(ClipSearch - firstId)] = {search, Page::Library};
    SendMessageW(search, EM_SETLIMITTEXT, 100, 0);
    SetWindowSubclass(search, editProc, 1, reinterpret_cast<DWORD_PTR>(this));
    addButton(AllClips, L"all clips", Page::Library);
    addButton(Favourites, L"favourites", Page::Library);
    addButton(Compressed, L"compressed", Page::Library);
    // Left to right as placed: the sort menu, the folders toggle, then the two tile sizes. The grid and the folder button under it follow below.
    addButton(SortClips, L"sort", Page::Library);
    addButton(GroupFolders, L"folders by game", Page::Library);
    addButton(LargeTiles, L"large tiles", Page::Library);
    addButton(SmallTiles, L"small tiles", Page::Library);
    const auto numberField = [&](int id, int value, int limit, Page page) {
        auto edit = CreateWindowExW(0, L"EDIT", std::to_wstring(value).c_str(), WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        controls_[static_cast<size_t>(id - firstId)] = {edit, page};
        SendMessageW(edit, EM_SETLIMITTEXT, limit, 0);
        SetWindowSubclass(edit, editProc, 1, reinterpret_cast<DWORD_PTR>(this));
    };
    // Recording and Settings controls are created in row order so Tab follows each page down.
    // Recording · settings for, on the title line over the page background (left to right: use defaults, then the drop-down)
    addButton(UseDefaults, L"use defaults", Page::Recording);
    addButton(SettingsFor, L"settings for · all games", Page::Recording);
    addButton(RenameGame, L"rename this game", Page::Recording); // Its text is the tooltip and the accessible name.
    // Recording · replay
    addButton(SelectProcess, L"choose source", Page::Recording, ButtonBackdrop::Card);
    addButton(AutoCaptureSource, L"auto-capture", Page::Recording, ButtonBackdrop::Card);
    addButton(ThirtySeconds, L"30 s", Page::Recording, ButtonBackdrop::Card);
    addButton(OneMinute, L"1 min", Page::Recording, ButtonBackdrop::Card);
    addButton(ThreeMinutes, L"3 min", Page::Recording, ButtonBackdrop::Card);
    addButton(FiveMinutes, L"5 min", Page::Recording, ButtonBackdrop::Card);
    // Each reset arrow comes just before its control, shown only while the chosen game sets that value.
    addButton(ReplayReset, L"reset replay length to all games", Page::Recording, ButtonBackdrop::Card);
    numberField(CustomSeconds, state_.replaySeconds, 4, Page::Recording);
    // Recording · video
    addButton(ResolutionReset, L"reset resolution to all games", Page::Recording, ButtonBackdrop::Card);
    addButton(Resolution, L"resolution · native", Page::Recording, ButtonBackdrop::Card);
    addButton(FrameRateReset, L"reset frame rate to all games", Page::Recording, ButtonBackdrop::Card);
    addButton(FrameRate, L"frame rate · 60 fps", Page::Recording, ButtonBackdrop::Card);
    addButton(QualityReset, L"reset quality to all games", Page::Recording, ButtonBackdrop::Card);
    addButton(QualityPreset, L"quality · balanced", Page::Recording, ButtonBackdrop::Card);
    addButton(BitrateReset, L"reset bitrate to all games", Page::Recording, ButtonBackdrop::Card);
    numberField(Bitrate, video_.bitrateMbps, 3, Page::Recording);
    addButton(CodecReset, L"reset codec to all games", Page::Recording, ButtonBackdrop::Card);
    addButton(VideoCodec, L"codec · h.264", Page::Recording, ButtonBackdrop::Card);
    // Recording · audio (the gate fields follow the gate switch, below)
    volume(DesktopVolume, DesktopVolumeLabel, L"desktop recording volume", audio_.levels.desktopPercent, 200, Page::Recording);
    addButton(DesktopAudio, L"desktop audio · on", Page::Recording, ButtonBackdrop::Card);
    addButton(DesktopReset, L"reset desktop audio", Page::Recording, ButtonBackdrop::Card);
    volume(MicrophoneVolume, MicrophoneVolumeLabel, L"microphone recording volume", audio_.levels.microphonePercent, 200, Page::Recording);
    addButton(Microphone, L"microphone · off", Page::Recording, ButtonBackdrop::Card);
    addButton(MicrophoneReset, L"reset microphone", Page::Recording, ButtonBackdrop::Card);
    addButton(RecordDiscord, L"record discord · on", Page::Recording, ButtonBackdrop::Card);
    addButton(Suppression, L"noise suppression · off", Page::Recording, ButtonBackdrop::Card);
    addButton(Gate, L"noise gate · off", Page::Recording, ButtonBackdrop::Card);
    // Settings · saving, storage, trimming, sound, app
    addButton(ChooseFolder, L"choose folder", Page::Settings, ButtonBackdrop::Card);
    // "<shortcut> · <keys>" for screen readers; the paint shows only the keys (refreshControls sets them).
    addButton(AssignHotkey, L"save shortcut", Page::Settings, ButtonBackdrop::Card);
    addButton(AssignScreenshot, L"screenshot shortcut", Page::Settings, ButtonBackdrop::Card);
    addButton(AskBeforeBinning, L"ask before binning · on", Page::Settings, ButtonBackdrop::Card);
    numberField(StorageLimitField, static_cast<int>(state_.maxStorageGb), 5, Page::Settings);
    if (!state_.maxStorageGb) SetWindowTextW(control(StorageLimitField), L""); // Empty: no limit.
    addButton(StorageBehaviour, L"when full · warn and save", Page::Settings, ButtonBackdrop::Card);
    addButton(CompressCopyAfterTrim, L"compress & copy after trim · off", Page::Settings, ButtonBackdrop::Card);
    addButton(GameColours, L"game colours · on", Page::Settings, ButtonBackdrop::Card);
    volume(ChimeVolume, ChimeVolumeLabel, L"save chime volume", chimes_.percent(), 100, Page::Settings);
    addButton(AutoCaptureStart, L"start listed games automatically · on", Page::Settings, ButtonBackdrop::Card);
    addButton(AutoCaptureOffers, L"offer new fullscreen games · on", Page::Settings, ButtonBackdrop::Card);
    addButton(CloseToTray, L"close to tray · off", Page::Settings, ButtonBackdrop::Card);
    addButton(StartWithWindows, L"start with windows · off", Page::Settings, ButtonBackdrop::Card);
    addButton(StartMinimised, L"start minimised · on", Page::Settings, ButtonBackdrop::Card);
    addButton(SignInReplay, L"start replay · off", Page::Settings, ButtonBackdrop::Card);
    addButton(ShortGameNames, L"short game names · on", Page::Settings, ButtonBackdrop::Card);
    addButton(DiagnosticLogging, L"diagnostic logging · off", Page::Settings, ButtonBackdrop::Card);
    addButton(ShowNotes, L"changelog", Page::Settings, ButtonBackdrop::Card); // First in the updates row, so first in tab order.
    addButton(UpdateCheckNow, L"check for updates", Page::Settings, ButtonBackdrop::Card);
    addButton(UpdateCheckSwitch, L"updates · off", Page::Settings, ButtonBackdrop::Card);
    HWND previous = control(Gate);
    const auto& filters = audio_.micFilters;
    const int gateValues[] = {static_cast<int>(filters.gateOpenDb), static_cast<int>(filters.gateCloseDb),
        filters.gateAttackMs, filters.gateHoldMs, filters.gateReleaseMs};
    for (size_t i = 0; i < std::size(gateFields); ++i) {
        const auto& field = gateFields[i];
        // Levels are negative, so only the millisecond fields can be digits-only.
        const DWORD style = WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | (field.id >= GateAttack ? ES_NUMBER : 0);
        auto input = CreateWindowExW(0, L"EDIT", std::to_wstring(gateValues[i]).c_str(), style, 0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(field.id)), GetModuleHandleW(nullptr), nullptr);
        controls_[static_cast<size_t>(field.id - firstId)] = {input, Page::Recording};
        SendMessageW(input, EM_SETLIMITTEXT, 5, 0);
        SetWindowSubclass(input, editProc, 1, reinterpret_cast<DWORD_PTR>(this));
        SetWindowPos(input, previous, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        previous = input;
    }
    if (auto grid = clipGrid_.create(window_, Clips)) controls_[Clips - firstId] = {grid, Page::Library};
    addButton(OpenFolder, L"open clip folder", Page::Library); // Under the grid: Tab reaches it after the clips.
    clipGrid_.setGrouped(state_.groupFolders);
    clipGrid_.setOrder(state_.clipOrder);
    clipGrid_.setAskBeforeBinning(state_.askBeforeBinning);
    clipGrid_.setGameColours(state_.gameColours);
    shareListedGames(); // Loaded with the settings, before the grid existed.
    if (auto view = player_.create(window_, Player, playerCallbacks())) controls_[Player - firstId] = {view, Page::Library};
    // The welcome screen (shown only while welcome_). Texts are "<row> · <value>" for screen readers; the button shows the value.
    // The two "recording starts" buttons show a fixed word and are named with their state (refreshControls).
    addButton(WelcomeAuto, L"recording starts automatically", Page::Library, ButtonBackdrop::Raised);
    addButton(WelcomeManual, L"recording starts manually", Page::Library, ButtonBackdrop::Raised);
    addButton(WelcomeLength, L"replay length", Page::Library, ButtonBackdrop::Raised);
    addButton(WelcomeShortcut, L"save shortcut", Page::Library, ButtonBackdrop::Raised);
    addButton(WelcomeFolder, L"clip folder", Page::Library, ButtonBackdrop::Raised);
    addButton(WelcomeStartup, L"start with windows · off", Page::Library, ButtonBackdrop::Raised);
    addButton(WelcomeTray, L"close to tray · off", Page::Library, ButtonBackdrop::Raised);
    addButton(WelcomeUpdates, L"check updates automatically · on", Page::Library, ButtonBackdrop::Raised);
    addButton(WelcomeSkip, L"skip", Page::Library, ButtonBackdrop::Card); // On the panel.
    addButton(WelcomeStart, L"start", Page::Library, ButtonBackdrop::Card);
    for (const int slider : sliderIds) updateVolumeLabel(slider);
    // Fields show only their value ("180", "100%"); the row's name is what a screen reader should say.
    nameControls({{control(ClipSearch), L"search clips"}, {control(CustomSeconds), L"custom replay length"},
        {control(Bitrate), L"bitrate"}, {control(DesktopVolumeLabel), L"desktop audio volume"},
        {control(MicrophoneVolumeLabel), L"microphone volume"}, {control(StorageLimitField), L"max storage for clips"},
        {control(GateOpen), L"noise gate open level"}, {control(GateClose), L"noise gate close level"},
        {control(GateAttack), L"noise gate attack"}, {control(GateHold), L"noise gate hold"},
        {control(GateRelease), L"noise gate release"}});
    // A tooltip names the icon-only buttons (collapsed sidebar, library view and folder, resets, also while locked)
    // and the level meters.
    // No fade or slide: they draw through WM_PRINT, which would show the system's black frame for a moment.
    tooltips_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP | TTS_NOFADE | TTS_NOANIMATE,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(tooltips_, TTM_SETTIPBKCOLOR, raised, 0);
    SendMessageW(tooltips_, TTM_SETTIPTEXTCOLOR, ink, 0);
    // Like the value bubble: small rounded corners and a border-coloured outline drawn by Windows 11. The tooltip's own
    // frame then takes the surface colour; where DWM cannot round (Windows 10) it is painted in the border colour.
    const bool rounded = theme::roundWindowCorners(tooltips_, 3); // DWMWCP_ROUNDSMALL
    if (rounded) theme::setWindowBorderColour(tooltips_, border);
    SetWindowSubclass(tooltips_, tooltipFrameProc, 1, rounded); // The font and padding come from createFonts (styleTooltips).
    for (const int id : {SidebarToggle, Library, Recording, Settings, SaveReplay, StartReplay, SortClips, GroupFolders, LargeTiles, SmallTiles, OpenFolder, DesktopReset, MicrophoneReset,
                         ReplayReset, ResolutionReset, FrameRateReset, QualityReset, BitrateReset, CodecReset, RenameGame}) {
        TOOLINFOW tool{toolInfoSize};
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = window_;
        tool.uId = reinterpret_cast<UINT_PTR>(control(id));
        tool.lpszText = LPSTR_TEXTCALLBACKW; // The button's current text, e.g. "stop replay".
        SendMessageW(tooltips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
    updateSidebarToggleText();
}

// Tooltips look like the app's value bubble: the raised surface, ink text, the small font and padding (colours set in
// createControls). The font and padding follow the DPI, so createFonts sets them, after createControls.
void App::styleTooltips() {
    if (!tooltips_) return;
    SendMessageW(tooltips_, WM_SETFONT, reinterpret_cast<WPARAM>(fontOf(Font::Small)), FALSE);
    RECT margin{px(9), px(4), px(9), px(4)}; // The value bubble's padding.
    SendMessageW(tooltips_, TTM_SETMARGIN, 0, reinterpret_cast<LPARAM>(&margin));
    SendMessageW(tooltips_, TTM_UPDATE, 0, 0); // A tip already showing takes the new font and margins (DPI change).
}

// The system draws a black frame round a tooltip. With DWM's outline (`rounded`) it takes the surface colour, otherwise
// one border-coloured pixel suits the dark surface.
LRESULT CALLBACK App::tooltipFrameProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR rounded) {
    if (message == WM_NCPAINT) {
        RECT frame{};
        GetWindowRect(window, &frame);
        OffsetRect(&frame, -frame.left, -frame.top);
        const HDC dc = GetWindowDC(window);
        SetDCBrushColor(dc, rounded ? raised : border);
        FrameRect(dc, &frame, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        ReleaseDC(window, dc);
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, tooltipFrameProc, 1);
    return DefSubclassProc(window, message, wParam, lParam);
}

namespace {
constexpr UINT_PTR lockedToolId = 0x4C4B; // Tooltip tools: this and the next ones, one per locked row.
constexpr size_t lockedToolLimit = 0x100; // Keeps the ids below the meters' (0x4D54); a page has far fewer rows.
}

bool App::isLockedTool(UINT_PTR id) const { return id >= lockedToolId && id < lockedToolId + lockedTools_; }

bool App::lockedTooltipAt(POINT point) const {
    return std::any_of(lockedToolRects_.begin(), lockedToolRects_.end(),
                       [&](const RECT& rect) { return PtInRect(&rect, point) != FALSE; });
}

// A kept replay is cleared, not stopped; the sidebar button says the same.
const wchar_t* App::lockedToolText() const {
    return recording_ == capture::RecorderState::Stopped ? L"clear the retained replay to change this setting" :
        L"stop replay to change this setting";
}

const wchar_t* App::lockedHeadingNote() const {
    return recording_ == capture::RecorderState::Stopped ? L"clear the retained replay to unlock" : L"stop replay to unlock";
}

bool App::recordingControlEnabled(int id, bool editable) const {
    const bool desktop = audio_.desktop.enabled(), microphone = audio_.microphone.enabled();
    switch (id) {
    case AutoCaptureSource: return !closing_; // The list only affects the next time a game comes to the front.
    case Bitrate: return editable && scopeVideo().quality == capture::Quality::Custom;
    // Discord can be left out wherever Windows can split it (a chosen output: if Discord plays elsewhere).
    case RecordDiscord: return editable && desktop && !discordUnavailable_;
    case DesktopVolume: case DesktopVolumeLabel: return editable && desktop;
    case Suppression: case Gate: case MicrophoneVolume: case MicrophoneVolumeLabel: return editable && microphone;
    default:
        for (const auto& field : gateFields)
            if (field.id == id) return editable && microphone && audio_.micFilters.noiseGate;
        return editable;
    }
}

bool App::rowLockedByReplay(const Row& row) const {
    if (recording_ == capture::RecorderState::Off) return false;
    const bool editableIfOff = !closing_; // Busy passes; stopping replay is still what unlocks the row.
    return std::any_of(row.ids.begin(), row.ids.end(),
        [&](int id) { return !recordingControlEnabled(id, false) && recordingControlEnabled(id, editableIfOff); });
}

void App::updateLockedTooltips() {
    if (!tooltips_) return;
    std::vector<RECT> rows;
    // Fullscreen playback is only on the Library page, so the page check covers it.
    if (recording_ != capture::RecorderState::Off && state_.page == Page::Recording) {
        const int left = contentLeft(), scroll = scroll_[static_cast<int>(Page::Recording)], bottom = footerTop();
        for (const auto& section : pages_[static_cast<size_t>(Page::Recording)]) {
            for (const auto& row : section.rows) {
                if (!rowLockedByReplay(row)) continue;
                const int top = row.y - scroll, end = std::min(row.y + row.height - scroll, bottom);
                if (end > top && rows.size() < lockedToolLimit)
                    rows.push_back({px(left + section.x), px(top), px(left + section.x + section.width), px(end)});
            }
        }
    }
    // Replay off: the rectangles are already empty, so most layouts and scrolls send nothing.
    if (rows.empty() && !lockedToolsShown_) return;
    // Tools are added as rows need them and kept; one the control refuses is tried again on the next update.
    while (lockedTools_ < rows.size()) {
        TOOLINFOW tool{toolInfoSize};
        tool.uFlags = 0; // The main window relays once, for meter and locked-row tools alike.
        tool.hwnd = window_;
        tool.uId = lockedToolId + lockedTools_;
        tool.lpszText = LPSTR_TEXTCALLBACKW; // lockedToolText: it depends on whether the replay is kept.
        if (!SendMessageW(tooltips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool))) break;
        ++lockedTools_;
    }
    lockedToolsShown_ = false;
    lockedToolRects_.assign(rows.begin(), rows.begin() + std::min(rows.size(), lockedTools_));
    for (size_t i = 0; i < lockedTools_; ++i) {
        TOOLINFOW tool{toolInfoSize};
        tool.hwnd = window_;
        tool.uId = lockedToolId + i;
        if (i < rows.size()) tool.rect = rows[i]; // Unused tools get an empty rectangle and never show.
        lockedToolsShown_ = lockedToolsShown_ || i < rows.size();
        SendMessageW(tooltips_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
    }
}

// Keyboard focus shows an outline; a click hides it again.
void App::setKeyboardFocus(bool on) {
    if (on == keyboardFocus_) return;
    keyboardFocus_ = on;
    if (const HWND focused = GetFocus(); focused && IsChild(window_, focused)) InvalidateRect(focused, nullptr, FALSE);
}

void App::updateSidebarToggleText() {
    SetWindowTextW(control(SidebarToggle), state_.sidebarCollapsed ? L"expand sidebar" : L"collapse sidebar");
}

int App::volumePercent(int slider) const {
    return slider == DesktopVolume ? audio_.levels.desktopPercent : slider == MicrophoneVolume ? audio_.levels.microphonePercent : chimes_.percent();
}

void App::updateVolumeLabel(int slider) {
    // The row names the source; the label beside the slider shows only its value.
    const int label = volumeLabelOf(slider);
    if (typingVolume_ == label) return; // The user is typing in it; the slider's value waits.
    setTextIfChanged(control(label), std::to_wstring(volumePercent(slider)) + L"%");
}

// Focus turns the "110%" text into its bare number (editProc then selects it), so typing replaces it.
void App::beginVolumeTyping(int label) {
    if (typingVolume_ == label) return;
    typingVolume_ = label;
    SetWindowTextW(control(label), std::to_wstring(volumePercent(volumeSliderOf(label))).c_str());
}

void App::endVolumeTyping(int label) {
    if (typingVolume_ != label) return;
    const int slider = volumeSliderOf(label);
    wchar_t text[16]{};
    GetWindowTextW(control(label), text, static_cast<int>(std::size(text)));
    typingVolume_ = 0; // Cleared first: the label below follows the slider again, and focus changes re-enter here.
    const HWND bar = control(slider);
    const int maximum = static_cast<int>(SendMessageW(bar, TBM_GETRANGEMAX, 0, 0));
    const auto percent = parseVolumePercent(text, 0, maximum);
    // The same path as a drag: the slider moves, then its parent hears the position.
    if (percent && *percent != static_cast<int>(SendMessageW(bar, TBM_GETPOS, 0, 0)) && IsWindowEnabled(bar)) {
        SendMessageW(bar, TBM_SETPOS, TRUE, *percent);
        SendMessageW(window_, WM_HSCROLL, MAKEWPARAM(TB_THUMBPOSITION, *percent), reinterpret_cast<LPARAM>(bar));
    }
    updateVolumeLabel(slider); // Empty or invalid text ends on the real value.
}

bool App::gateInputValid() const {
    return std::all_of(gateFieldValid_.begin(), gateFieldValid_.end(), [](bool valid) { return valid; });
}

void App::updateGateInput() {
    // EDIT can notify while CreateWindowEx is still returning, before registration.
    for (const auto& field : gateFields) if (!control(field.id)) return;
    int values[std::size(gateFields)]{};
    for (size_t i = 0; i < std::size(gateFields); ++i) {
        wchar_t text[8]{};
        GetWindowTextW(control(gateFields[i].id), text, static_cast<int>(std::size(text)));
        const bool level = gateFields[i].id == GateOpen || gateFields[i].id == GateClose;
        const auto value = level ? parseNumber(text, gateLevelMinDb, gateLevelMaxDb) : parseNumber(text, 0, gateTimeMaxMs);
        gateFieldValid_[i] = value.has_value();
        values[i] = value.value_or(0);
    }
    // Hysteresis needs the close level at or below the open level.
    gateCloseAboveOpen_ = gateFieldValid_[0] && gateFieldValid_[1] && values[1] > values[0];
    if (gateCloseAboveOpen_) gateFieldValid_[0] = gateFieldValid_[1] = false;
    // Only complete, valid sets reach the recorder (and, once a field is left, the file); it rejects anything else at start.
    if (gateInputValid()) {
        auto& f = audio_.micFilters;
        f.gateOpenDb = static_cast<float>(values[0]);
        f.gateCloseDb = static_cast<float>(values[1]);
        f.gateAttackMs = values[2];
        f.gateHoldMs = values[3];
        f.gateReleaseMs = values[4];
    }
    refreshControls();
    refreshPages(); // A problem adds or changes the gate row's note.
    syncGateBubble(); // After the page: it hides the bubble when controls move.
}

// The reason a gate value is refused, in a bubble above the field (above the pair, for a close level over the open
// one). Shown for the focused invalid field, else the hovered one; gone when the value is valid, the field is left,
// the page, window or scroll position changes, or after a few seconds (a one-shot timer, no polling).
void App::syncGateBubble() {
    const auto invalid = [this](int id) {
        const auto* field = gateFieldOf(id);
        return field && !gateFieldValid_[field - std::begin(gateFields)];
    };
    const HWND focus = GetFocus();
    const int focused = focus && GetParent(focus) == window_ ? GetDlgCtrlID(focus) : 0;
    const int target = invalid(focused) ? focused : invalid(gateHover_) ? gateHover_ : 0;
    // Only a visible field of the front window gets one; hovering over a window behind another app shows nothing.
    if (!target || !IsWindowVisible(control(target)) || IsIconic(window_) || GetForegroundWindow() != window_) {
        hideGateBubble();
        return;
    }
    const auto problem = gateProblem(static_cast<int>(gateFieldOf(target) - std::begin(gateFields)), gateCloseAboveOpen_,
        gateLevelMinDb, gateLevelMaxDb, gateTimeMaxMs);
    RECT anchor{};
    for (int i = problem.first; i <= problem.last; ++i) {
        const RECT frame = fieldFrame(gateFields[i].id);
        UnionRect(&anchor, &anchor, &frame);
    }
    MapWindowPoints(window_, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
    gateBubble_.showNote(window_, anchor, problem.text, fontOf(Font::Body), dpi_);
    SetTimer(window_, gateBubbleTimer, 4000, nullptr); // One shot; restarted by each change of the text.
}

void App::hideGateBubble() {
    KillTimer(window_, gateBubbleTimer);
    gateBubble_.hide();
}

void App::refreshControls() {
    RawHotkey::Shortcuts registered{};
    for (int shortcut = 0; shortcut < shortcutCount; ++shortcut)
        if (hotkeyRegistered_[shortcut]) registered[shortcut] = hotkeys_[shortcut];
    rawHotkey_.configure(window_, registered, (registered[saveShortcut] || registered[screenshotShortcut]) &&
        !assigning_ && !closing_ && recording_ != capture::RecorderState::Off);
    if (!control(StartReplay)) return;
    const bool off = recording_ == capture::RecorderState::Off;
    const bool editable = off && !busy_ && !closing_;
    // The same rules tell which rows replay alone locks (rowLockedByReplay), so tips and headings follow them.
    const int settings[] = {SelectProcess, AutoCaptureSource, ThirtySeconds, OneMinute, ThreeMinutes, FiveMinutes,
        CustomSeconds, DesktopAudio, Microphone, DesktopReset, MicrophoneReset, Suppression, Gate, Resolution, FrameRate,
        QualityPreset, Bitrate, VideoCodec, UseDefaults, ReplayReset, ResolutionReset, FrameRateReset, QualityReset, BitrateReset, CodecReset, RenameGame, RecordDiscord, DesktopVolume, MicrophoneVolume, DesktopVolumeLabel, MicrophoneVolumeLabel};
    for (int id : settings) EnableWindow(control(id), recordingControlEnabled(id, editable));
    for (const auto& field : gateFields) EnableWindow(control(field.id), recordingControlEnabled(field.id, editable));
    // "settings for" locks with the rest of the page; per-game settings that could not be read leave only all games.
    EnableWindow(control(SettingsFor), editable && gameSettingsReadable_);
    setTextIfChanged(control(SettingsFor), L"settings for · " + (gameScope() ? shownGameName(settingsFor_, settingsForLabel_) : std::wstring(L"all games")));
    const auto shown = scopeVideo(); // The rows show the scope the page edits.
    // What the source would start with decides whether the fields must be valid (see startReplay).
    const bool fieldsOk = fieldsReady(sourceVideo());
    const bool gateReady = gateInputValid() || !audio_.microphone.enabled() || !audio_.micFilters.noiseGate;
    const bool welcoming = welcome_ && !closing_;
    EnableWindow(control(StartReplay), !welcoming && !busy_ && !closing_ && (!off ||
        ((target_.source.window || target_.source.monitor) && fieldsOk && gateReady)));
    EnableWindow(control(SaveReplay), !off && !busy_ && !saving_ && !closing_);
    const bool folderFree = !busy_ && !saving_ && !closing_;
    EnableWindow(control(ChooseFolder), folderFree);
    EnableWindow(control(WelcomeFolder), folderFree);
    // Start waits for a folder check the screen began, so "automatically" can take over at once (useAutoCapture).
    EnableWindow(control(WelcomeStart), folderFree);
    // Unreadable, or another copy's entry: start would not change it (Settings says why).
    EnableWindow(control(WelcomeStartup), !closing_ && !startupReadFailed_ && !startupOtherCopy_);
    EnableWindow(control(CloseToTray), !closing_);
    EnableWindow(control(StartWithWindows), !closing_);
    EnableWindow(control(StartMinimised), !closing_ && state_.startWithWindows && !startupReadFailed_);
    EnableWindow(control(SignInReplay), !closing_ && state_.startWithWindows && !startupReadFailed_);
    EnableWindow(control(DiagnosticLogging), !closing_);
    EnableWindow(control(CompressCopyAfterTrim), !closing_);
    EnableWindow(control(GameColours), !closing_);
    setTextIfChanged(control(GameColours), state_.gameColours ? L"game colours · on" : L"game colours · off");
    EnableWindow(control(ShortGameNames), !closing_);
    setTextIfChanged(control(ShortGameNames), state_.shortGameNames ? L"short game names · on" : L"short game names · off");
    EnableWindow(control(AskBeforeBinning), !closing_);
    EnableWindow(control(UpdateCheckSwitch), !closing_);
    const auto updateUi = updateView();
    EnableWindow(control(UpdateCheckNow), updateUi.buttonEnabled);
    EnableWindow(control(StorageLimitField), !closing_);
    EnableWindow(control(StorageBehaviour), !closing_);
    setTextIfChanged(control(StorageBehaviour), state_.storageFull == StorageFull::Warn ? L"when full · warn and save" :
        state_.storageFull == StorageFull::Refuse ? L"when full · refuse the save" : L"when full · bin oldest clips");
    setTextIfChanged(control(CompressCopyAfterTrim), state_.compressCopyAfterTrim ? L"compress & copy after trim · on" : L"compress & copy after trim · off");
    setTextIfChanged(control(AskBeforeBinning), state_.askBeforeBinning ? L"ask before binning · on" : L"ask before binning · off");
    setTextIfChanged(control(DiagnosticLogging), state_.diagnosticLogging ? L"diagnostic logging · on" : L"diagnostic logging · off");
    setTextIfChanged(control(UpdateCheckSwitch), state_.updateCheck == UpdateChoice::On ? L"updates · on" : L"updates · off");
    // With a newer version known the button updates to it, or opens its page for a portable copy (as the footer's text
    // does, but only for the mouse).
    // Orange while the label is an action on the newer version (updateView); a change of state always changes the
    // label too, so the button is repainted with it.
    setTextIfChanged(control(UpdateCheckNow), updateUi.button);
    // The sort icon's text is its tooltip and what a screen reader says: the order, and the date filter when one is set.
    setTextIfChanged(control(SortClips), std::wstring(L"sort · ") + clipOrderLabel(clipGrid_.order()) +
        (clipGrid_.filtering() ? std::wstring(L" · ") + clipPeriodNames[static_cast<int>(clipGrid_.period())] : std::wstring{}));
    const auto games = autoCapture_.games.size();
    setTextIfChanged(control(AutoCaptureSource), std::wstring(target_.source.monitor ? L"auto-capture · games only" :
        target_.source.window ? (autoCapture_.find(target_.path) ? L"auto-capture · on" : L"auto-capture · off") :
        (L"auto-capture · " + std::to_wstring(games) + (games == 1 ? L" game" : L" games"))));
    updateTrayTip();
    EnableWindow(control(ChimeVolume), !closing_); // Not a recording setting: never locked by replay.
    setTextIfChanged(control(CloseToTray), state_.closeToTray ? L"close to tray · on" : L"close to tray · off");
    setTextIfChanged(control(StartMinimised), state_.startMinimised ? L"start minimised · on" : L"start minimised · off");
    setTextIfChanged(control(SignInReplay), state_.signInReplay ? L"start replay · on" : L"start replay · off");
    setTextIfChanged(control(StartWithWindows), startupReadFailed_ ? L"startup · unavailable" :
        startupOtherCopy_ ? L"startup · other copy" : state_.startWithWindows ? L"start with windows · on" : L"start with windows · off");
    EnableWindow(control(AssignHotkey), !closing_);
    EnableWindow(control(AssignScreenshot), !closing_);
    // The button shows the current keys; while assigning, what is held so far.
    const auto shortcutKeys = [&](int shortcut) {
        if (!assigning_ || assigningShortcut_ != shortcut) return describe(hotkeys_[shortcut]);
        const auto held = heldModifiersText(heldModifiers_);
        return held.empty() ? std::wstring(L"press a new key") : held;
    };
    setTextIfChanged(control(AssignHotkey), L"save shortcut · " + shortcutKeys(saveShortcut));
    setTextIfChanged(control(AssignScreenshot), L"screenshot shortcut · " + shortcutKeys(screenshotShortcut));
    EnableWindow(control(AutoCaptureStart), !closing_);
    EnableWindow(control(AutoCaptureOffers), !closing_);
    setTextIfChanged(control(AutoCaptureStart), autoCaptureOn_ ? L"start listed games automatically · on" : L"start listed games automatically · off");
    setTextIfChanged(control(AutoCaptureOffers), autoCapturePrompt_ ? L"offer new fullscreen games · on" : L"offer new fullscreen games · off");
    setTextIfChanged(control(StartReplay), busy_ ? L"please wait…" : off ? L"enable replay" :
        recording_ == capture::RecorderState::Stopped ? L"clear retained replay" : L"stop replay");
    setTextIfChanged(control(SaveReplay), saving_ ? L"saving…" : L"save replay");
    setTextIfChanged(control(DesktopAudio), deviceText(L"desktop audio", audio_.desktop));
    setTextIfChanged(control(Microphone), deviceText(L"microphone", audio_.microphone));
    setTextIfChanged(control(Suppression), audio_.micFilters.noiseSuppression ? L"noise suppression · on" : L"noise suppression · off");
    setTextIfChanged(control(Gate), audio_.micFilters.noiseGate ? L"noise gate · on" : L"noise gate · off");
    setTextIfChanged(control(RecordDiscord), audio_.hideDiscord ? L"record discord · off" : L"record discord · on");
    setTextIfChanged(control(Resolution), std::wstring(L"resolution · ") + resolutionNames[static_cast<int>(shown.resolution)]);
    setTextIfChanged(control(FrameRate), shown.fps == 60 ? L"frame rate · 60 fps" : L"frame rate · 30 fps");
    setTextIfChanged(control(QualityPreset), std::wstring(L"quality · ") + qualityNames[static_cast<int>(shown.quality)]);
    setTextIfChanged(control(VideoCodec), (std::wstring(L"codec · ") + capture::codecName(shown.codec)));
    for (const int id : {SidebarToggle, Library, Recording, Settings}) EnableWindow(control(id), !welcoming);
    setTextIfChanged(control(WelcomeShortcut), L"save shortcut · " + shortcutKeys(saveShortcut)); // As Settings shows it.
    setTextIfChanged(control(WelcomeFolder), L"clip folder · " + lower(clipFolder_.wstring()));
    setTextIfChanged(control(WelcomeAuto), welcomeAutomatic_ ? L"recording starts automatically · selected" : L"recording starts automatically");
    setTextIfChanged(control(WelcomeManual), welcomeAutomatic_ ? L"recording starts manually" : L"recording starts manually · selected");
    setTextIfChanged(control(WelcomeUpdates), welcomeUpdates_ ? L"check updates automatically · on" : L"check updates automatically · off");
    setTextIfChanged(control(WelcomeLength), L"replay length · " + formatReplayDuration(welcomeSeconds_));
    setTextIfChanged(control(WelcomeStartup), welcomeStartup_ ? L"start with windows · on" : L"start with windows · off");
    setTextIfChanged(control(WelcomeTray), welcomeTray_ ? L"close to tray · on" : L"close to tray · off");
}

LRESULT CALLBACK App::volumeProc(HWND slider, UINT message, WPARAM wParam, LPARAM lParam,
                                UINT_PTR, DWORD_PTR ref) {
    if (message == WM_ERASEBKGND) return 1;
    // The wheel scrolls the page; only drags and keys change a volume.
    if (message == WM_MOUSEWHEEL) return SendMessageW(GetParent(slider), message, wParam, lParam);
    if (message == WM_SETFOCUS) reinterpret_cast<App*>(ref)->reveal(slider);
    if (message == WM_LBUTTONDOWN && IsWindowEnabled(slider)) {
        RECT thumb{}, channel{};
        SendMessageW(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        const POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
        if (!PtInRect(&thumb, point)) {
            SendMessageW(slider, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
            const int minimum = static_cast<int>(SendMessageW(slider, TBM_GETRANGEMIN, 0, 0));
            const int maximum = static_cast<int>(SendMessageW(slider, TBM_GETRANGEMAX, 0, 0));
            const int position = std::clamp(minimum + MulDiv(point.x - channel.left,
                maximum - minimum, std::max(1L, channel.right - channel.left - 1)), minimum, maximum);
            SendMessageW(slider, TBM_SETPOS, TRUE, position);
            SendMessageW(GetParent(slider), WM_HSCROLL, MAKEWPARAM(TB_THUMBPOSITION, position),
                         reinterpret_cast<LPARAM>(slider));
            SendMessageW(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
            // Start native THUMB tracking at the new orb, never native page
            // tracking. Subsequent mouse moves can freely reverse direction.
            lParam = MAKELPARAM((thumb.left + thumb.right) / 2, (thumb.top + thumb.bottom) / 2);
        }
    }
    auto* app = reinterpret_cast<App*>(ref);
    // The bubble ends with the drag (release or lost capture), with focus, or when the slider goes.
    if (message == WM_CAPTURECHANGED || message == WM_KILLFOCUS || message == WM_ENABLE || message == WM_NCDESTROY) app->hideSliderBubble();
    if (message == WM_NCDESTROY) RemoveWindowSubclass(slider, volumeProc, 1);
    const LRESULT result = DefSubclassProc(slider, message, wParam, lParam);
    // The native control only invalidates the old and new knob; the fill between them is ours, so any
    // position change (reset, settings, keys, drag) repaints the whole slider.
    if (message == TBM_SETPOS || message == WM_KEYDOWN) InvalidateRect(slider, nullptr, FALSE);
    if (message == WM_LBUTTONDOWN && GetCapture() == slider) {
        app->bubbleDrag_ = true;
        KillTimer(app->window_, bubbleTimer);
        app->showSliderBubble(slider);
    } else if (message == WM_MOUSEMOVE && app->bubbleDrag_ && GetCapture() == slider) {
        app->showSliderBubble(slider); // Follows the knob.
    } else if (message == WM_KEYDOWN && !app->bubbleDrag_ && IsWindowEnabled(slider)) {
        switch (wParam) {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END:
            app->showSliderBubble(slider);
            SetTimer(app->window_, bubbleTimer, 1000, nullptr); // One shot; restarted by each key.
            break;
        }
    }
    return result;
}

// The current value in a bubble centred above the knob. The anchor takes the knob's columns and the slider's
// full height, so the bubble stays clear of the slider (and its focus outline) whatever the knob rect is.
void App::showSliderBubble(HWND slider) {
    RECT thumb{}, rect{};
    SendMessageW(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    GetClientRect(slider, &rect);
    RECT anchor{thumb.left, rect.top, thumb.right, rect.bottom};
    MapWindowPoints(slider, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
    const int percent = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
    bubble_.show(window_, anchor, std::to_wstring(percent) + L"%", fontOf(Font::Body), dpi_);
}

void App::hideSliderBubble() {
    bubbleDrag_ = false;
    KillTimer(window_, bubbleTimer);
    bubble_.hide();
}

LRESULT CALLBACK App::buttonProc(HWND button, UINT message, WPARAM wParam, LPARAM lParam,
                                 UINT_PTR, DWORD_PTR ref) {
    auto* app = reinterpret_cast<App*>(ref);
    if (message == WM_ERASEBKGND) return 1; // Owner drawing covers the entire control.
    // A quick second click arrives as a double-click, which BUTTON never turns into BN_CLICKED; no button here
    // uses double-clicks, so it counts as another press and the toggle is not lost.
    if (message == WM_LBUTTONDBLCLK) message = WM_LBUTTONDOWN;
    if (message == WM_MOUSEMOVE && app->hovered_ != GetDlgCtrlID(button)) {
        app->hovered_ = GetDlgCtrlID(button);
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, button, 0};
        TrackMouseEvent(&tracking);
        InvalidateRect(button, nullptr, FALSE);
    } else if (message == WM_MOUSELEAVE) {
        if (app->hovered_ == GetDlgCtrlID(button)) app->hovered_ = 0; // Another button may already be under the cursor.
        InvalidateRect(button, nullptr, FALSE);
    } else if (message == WM_SETFOCUS) {
        app->reveal(button);
    } else if (message == WM_KILLFOCUS && (GetDlgCtrlID(button) == AssignHotkey || GetDlgCtrlID(button) == AssignScreenshot ||
                                           GetDlgCtrlID(button) == WelcomeShortcut)) {
        app->finishAssigning(std::nullopt); // Clicking elsewhere or switching apps cancels.
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(button, buttonProc, 1);
    }
    return DefSubclassProc(button, message, wParam, lParam);
}

HWND App::focusedTextField() const {
    const HWND focus = GetFocus();
    const int id = focus && GetParent(focus) == window_ ? GetDlgCtrlID(focus) : 0;
    return contains(typedVolumeIds, id) || contains(numberIds, id) || id == ClipSearch ? focus : nullptr;
}

// Typing in the library's search filters the clips at once.
void App::updateSearch() {
    const HWND search = control(ClipSearch);
    if (!search) return;
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(search)) + 1, L'\0');
    text.resize(static_cast<size_t>(GetWindowTextW(search, text.data(), static_cast<int>(text.size()))));
    clipGrid_.setSearch(text);
    const bool empty = text.empty();
    if (empty != searchEmpty_) {
        searchEmpty_ = empty;
        // The edit redraws its line itself when the text changes, so the placeholder is repainted by a WM_PAINT.
        InvalidateRect(search, nullptr, TRUE);
    }
}

// Keys in the search field: Esc clears it, then leaves it; Enter or Down goes to the matching clips.
bool App::searchKey(WPARAM key) {
    const HWND search = control(ClipSearch);
    if (key == VK_ESCAPE) {
        if (GetWindowTextLengthW(search) > 0) SetWindowTextW(search, L"");
        else SetFocus(window_);
        return true;
    }
    if (key == VK_RETURN || key == VK_DOWN) {
        if (clipGrid_.hasTiles()) {
            clipGrid_.showKeyboardFocus(); // Before the focus moves, so the next Down goes to the second match.
            SetFocus(control(Clips));
        }
        return true;
    }
    return false;
}

// The frame painted around a text field, in client pixels: a number field's includes its unit, a volume
// percentage's is the hover and focus frame. paint and invalidateFieldFrame both use it.
RECT App::fieldFrame(int id) const {
    RECT frame{};
    GetWindowRect(control(id), &frame);
    MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&frame), 2);
    if (contains(typedVolumeIds, id)) {
        InflateRect(&frame, px(5), px(2));
        return frame;
    }
    if (id == ClipSearch) {
        InflateRect(&frame, px(searchFramePad), px(7));
        frame.left -= px(searchIconRoom);
        return frame;
    }
    InflateRect(&frame, px(8), px(7));
    frame.right += px(fieldUnit(id).width + 4);
    return frame;
}

void App::invalidateFieldFrame(HWND edit) {
    const int id = GetDlgCtrlID(edit);
    RECT box = fieldFrame(id);
    if (gateFieldOf(id)) box.left -= px(50); // The gate's caption.
    InflateRect(&box, px(2), px(2)); // The outline's anti-aliasing.
    InvalidateRect(window_, &box, FALSE);
}

// The text fields of the main window: the number fields and the volume percentages. A click focuses and selects
// all (a click while focused places the caret); a number field is saved when left, a volume percentage committed.
LRESULT CALLBACK App::editProc(HWND edit, UINT message, WPARAM wParam, LPARAM lParam,
                               UINT_PTR, DWORD_PTR ref) {
    auto* app = reinterpret_cast<App*>(ref);
    const int id = GetDlgCtrlID(edit);
    const bool volume = contains(typedVolumeIds, id);
    if (message == WM_LBUTTONDOWN && GetFocus() != edit) {
        SetFocus(edit);
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(edit, editProc, 1);
    // A volume percentage is plain text until the pointer is over it, so it tracks hovering.
    const int hover = app->hoverVolume_;
    if (volume && message == WM_MOUSEMOVE && hover != id) {
        // WM_MOUSELEAVE for the old field can arrive after this move. Erase its
        // frame now; that later message must not clear the new field's hover.
        if (hover) app->invalidateFieldFrame(app->control(hover));
        app->hoverVolume_ = id;
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, edit, 0};
        TrackMouseEvent(&tracking);
    } else if (volume && message == WM_MOUSELEAVE && hover == id) {
        app->hoverVolume_ = 0;
    }
    const bool gate = gateFieldOf(id) != nullptr;
    // A gate value that is refused explains itself in a bubble when the pointer comes over it.
    if (gate && message == WM_MOUSEMOVE && app->gateHover_ != id) {
        app->gateHover_ = id;
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, edit, 0};
        TrackMouseEvent(&tracking);
        app->syncGateBubble();
    } else if (gate && message == WM_MOUSELEAVE && app->gateHover_ == id) {
        app->gateHover_ = 0;
        if (GetFocus() != edit) app->syncGateBubble(); // The focused field's bubble, if any; otherwise none.
    }
    const LRESULT result = DefSubclassProc(edit, message, wParam, lParam);
    // What makes the edit redraw its line outside WM_PAINT: focus, selection, a left click, and the end of a drag.
    const bool repaints = message == WM_SETFOCUS || message == WM_KILLFOCUS || message == EM_SETSEL ||
                          message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK ||
                          message == WM_CAPTURECHANGED;
    // searchEmpty_ is tracked by updateSearch: asking the field here would send WM_GETTEXTLENGTH back through this procedure.
    if (id == ClipSearch && (message == WM_PAINT || repaints) && app->searchEmpty_) {
        if (message == WM_PAINT) {
            // The empty search names itself, in the muted colour, on the line where the text would be: the edit
            // clears that line when it redraws, so nothing of the name is left outside it.
            if (const HDC dc = GetDC(edit)) {
                RECT text{};
                SendMessageW(edit, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&text));
                text.left += 1;
                const auto old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0)));
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, muted);
                DrawTextW(dc, L"search clips", -1, &text, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                SelectObject(dc, old);
                ReleaseDC(edit, dc);
            }
        } else {
            // The edit redraws its line for a click, a selection or the focus outside WM_PAINT, which wipes the name;
            // paint it again.
            InvalidateRect(edit, nullptr, TRUE);
        }
    }
    if (message == WM_SETFOCUS) {
        if (volume) app->beginVolumeTyping(id);
        SendMessageW(edit, EM_SETSEL, 0, -1);
        app->reveal(edit);
        if (gate) app->syncGateBubble(); // After reveal, which may scroll the page.
    } else if (message == WM_KILLFOCUS) {
        if (gate) app->hideGateBubble();
        if (volume) app->endVolumeTyping(id); // Applies the value as a slider drag would.
        else if (id != ClipSearch) {
            // A game's typed value goes to its own file; the page then shows its notes and reset arrows once.
            const bool typed = app->typedSeconds_ || app->typedBitrate_;
            const bool stored = app->commitScopeFields();
            app->saveSettings(); // A number field is written to the file when left; typing only validates.
            if (typed && stored) {
                app->refreshControls();
                app->refreshPages();
            }
        }
    }
    if (app->hoverVolume_ != hover || message == WM_SETFOCUS || message == WM_KILLFOCUS) app->invalidateFieldFrame(edit);
    return result;
}

} // namespace refarter
