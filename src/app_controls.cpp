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

namespace refarter {
namespace {
std::wstring deviceText(const wchar_t* source, const capture::AudioDevice& device) {
    using Mode = capture::AudioDevice::Mode;
    return std::wstring(source) + L" · " + (device.mode == Mode::Off ? L"off" : device.mode == Mode::SystemDefault ?
        L"system default" : device.name.empty() ? L"chosen device" : lower(device.name));
}
} // namespace

void App::updateReplayInput() {
    // EDIT can notify while CreateWindowEx is still returning, before registration.
    if (!control(CustomSeconds)) return;
    wchar_t text[16]{};
    GetWindowTextW(control(CustomSeconds), text, static_cast<int>(std::size(text)));
    const auto seconds = parseReplaySeconds(text);
    state_.replayInputValid = seconds.has_value();
    if (seconds) state_.replaySeconds = *seconds; // Written to the file once the field is left (see saveSettings).
    for (int id = ThirtySeconds; id <= FiveMinutes; ++id) InvalidateRect(control(id), nullptr, FALSE);
    refreshControls();
    refreshPages(); // The length row's note and the video estimate follow.
}

void App::updateBitrateInput() {
    if (!control(Bitrate)) return;
    wchar_t text[8]{};
    GetWindowTextW(control(Bitrate), text, static_cast<int>(std::size(text)));
    const auto value = parseNumber(text, capture::minBitrateMbps, capture::maxBitrateMbps);
    bitrateValid_ = value.has_value();
    if (value) video_.bitrateMbps = *value;
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
    // Left to right as placed: the folders toggle, then the two tile sizes. The grid and the folder button under it follow below.
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
    // Recording · replay
    addButton(SelectProcess, L"choose source", Page::Recording, ButtonBackdrop::Card);
    addButton(AutoCaptureSource, L"auto-capture", Page::Recording, ButtonBackdrop::Card);
    addButton(ThirtySeconds, L"30 s", Page::Recording, ButtonBackdrop::Card);
    addButton(OneMinute, L"1 min", Page::Recording, ButtonBackdrop::Card);
    addButton(ThreeMinutes, L"3 min", Page::Recording, ButtonBackdrop::Card);
    addButton(FiveMinutes, L"5 min", Page::Recording, ButtonBackdrop::Card);
    numberField(CustomSeconds, state_.replaySeconds, 4, Page::Recording);
    // Recording · video
    addButton(Resolution, L"resolution · native", Page::Recording, ButtonBackdrop::Card);
    addButton(FrameRate, L"frame rate · 60 fps", Page::Recording, ButtonBackdrop::Card);
    addButton(QualityPreset, L"quality · balanced", Page::Recording, ButtonBackdrop::Card);
    numberField(Bitrate, video_.bitrateMbps, 3, Page::Recording);
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
    // The button text is the accessible name, so each says which shortcut it assigns; the paint shows "assign hotkey".
    addButton(AssignHotkey, L"assign save shortcut", Page::Settings, ButtonBackdrop::Card);
    addButton(AssignScreenshot, L"assign screenshot shortcut", Page::Settings, ButtonBackdrop::Card);
    addButton(AskBeforeBinning, L"ask before binning · on", Page::Settings, ButtonBackdrop::Card);
    numberField(StorageLimitField, static_cast<int>(state_.maxStorageGb), 5, Page::Settings);
    if (!state_.maxStorageGb) SetWindowTextW(control(StorageLimitField), L""); // Empty: no limit.
    addButton(StorageBehaviour, L"when full · warn and save", Page::Settings, ButtonBackdrop::Card);
    addButton(CompressCopyAfterTrim, L"compress & copy after trim · off", Page::Settings, ButtonBackdrop::Card);
    volume(ChimeVolume, ChimeVolumeLabel, L"save chime volume", chimes_.percent(), 100, Page::Settings);
    addButton(CloseToTray, L"close to tray · off", Page::Settings, ButtonBackdrop::Card);
    addButton(StartWithWindows, L"start with windows · off", Page::Settings, ButtonBackdrop::Card);
    addButton(StartMinimised, L"start minimised · on", Page::Settings, ButtonBackdrop::Card);
    addButton(DiagnosticLogging, L"diagnostic logging · off", Page::Settings, ButtonBackdrop::Card);
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
    clipGrid_.setAskBeforeBinning(state_.askBeforeBinning);
    if (auto view = player_.create(window_, Player, playerCallbacks())) controls_[Player - firstId] = {view, Page::Library};
    for (const int slider : sliderIds) updateVolumeLabel(slider);
    // Fields show only their value ("180", "100%"); the row's name is what a screen reader should say.
    nameControls({{control(ClipSearch), L"search clips"}, {control(CustomSeconds), L"custom replay length"},
        {control(Bitrate), L"bitrate"}, {control(DesktopVolumeLabel), L"desktop audio volume"},
        {control(MicrophoneVolumeLabel), L"microphone volume"}, {control(StorageLimitField), L"max storage for clips"},
        {control(GateOpen), L"noise gate open level"}, {control(GateClose), L"noise gate close level"},
        {control(GateAttack), L"noise gate attack"}, {control(GateHold), L"noise gate hold"},
        {control(GateRelease), L"noise gate release"}});
    // Collapsed sidebar buttons show icons only; a tooltip gives each its name.
    tooltips_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, GetModuleHandleW(nullptr), nullptr);
    for (const int id : {SidebarToggle, Library, Recording, Settings, SaveReplay, StartReplay, GroupFolders, LargeTiles, SmallTiles, OpenFolder, DesktopReset, MicrophoneReset}) {
        TOOLINFOW tool{sizeof(tool)};
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = window_;
        tool.uId = reinterpret_cast<UINT_PTR>(control(id));
        tool.lpszText = LPSTR_TEXTCALLBACKW; // The button's current text, e.g. "stop replay".
        SendMessageW(tooltips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
    updateSidebarToggleText();
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
    const int settings[] = {SelectProcess, ThirtySeconds, OneMinute, ThreeMinutes, FiveMinutes,
        CustomSeconds, DesktopAudio, Microphone, DesktopReset, MicrophoneReset, Suppression, Gate, Resolution, FrameRate, QualityPreset, VideoCodec};
    for (int id : settings) EnableWindow(control(id), editable);
    const bool custom = video_.quality == capture::Quality::Custom;
    EnableWindow(control(Bitrate), editable && custom);
    EnableWindow(control(Suppression), editable && audio_.microphone.enabled());
    // Discord can be left out wherever Windows can split it (a chosen output: if Discord plays elsewhere).
    EnableWindow(control(RecordDiscord), editable && audio_.desktop.enabled() && !discordUnavailable_);
    EnableWindow(control(Gate), editable && audio_.microphone.enabled());
    EnableWindow(control(DesktopVolume), editable && audio_.desktop.enabled());
    EnableWindow(control(MicrophoneVolume), editable && audio_.microphone.enabled());
    EnableWindow(control(DesktopVolumeLabel), editable && audio_.desktop.enabled());
    EnableWindow(control(MicrophoneVolumeLabel), editable && audio_.microphone.enabled());
    for (const auto& field : gateFields) EnableWindow(control(field.id), editable && audio_.microphone.enabled() && audio_.micFilters.noiseGate);
    const bool gateReady = gateInputValid() || !audio_.microphone.enabled() || !audio_.micFilters.noiseGate;
    EnableWindow(control(StartReplay), !busy_ && !closing_ && (!off ||
        ((target_.source.window || target_.source.monitor) && state_.replayInputValid && (bitrateValid_ || !custom) && gateReady)));
    EnableWindow(control(SaveReplay), !off && !busy_ && !saving_ && !closing_);
    EnableWindow(control(ChooseFolder), !busy_ && !saving_ && !closing_);
    EnableWindow(control(CloseToTray), !closing_);
    EnableWindow(control(StartWithWindows), !closing_);
    EnableWindow(control(StartMinimised), !closing_ && state_.startWithWindows && !startupReadFailed_);
    EnableWindow(control(DiagnosticLogging), !closing_);
    EnableWindow(control(CompressCopyAfterTrim), !closing_);
    EnableWindow(control(AskBeforeBinning), !closing_);
    EnableWindow(control(UpdateCheckSwitch), !closing_);
    EnableWindow(control(UpdateCheckNow), !closing_ && !(updates_.running() && updateManual_));
    EnableWindow(control(StorageLimitField), !closing_);
    EnableWindow(control(StorageBehaviour), !closing_);
    setTextIfChanged(control(StorageBehaviour), state_.storageFull == StorageFull::Warn ? L"when full · warn and save" :
        state_.storageFull == StorageFull::Refuse ? L"when full · refuse the save" : L"when full · bin oldest clips");
    setTextIfChanged(control(CompressCopyAfterTrim), state_.compressCopyAfterTrim ? L"compress & copy after trim · on" : L"compress & copy after trim · off");
    setTextIfChanged(control(AskBeforeBinning), state_.askBeforeBinning ? L"ask before binning · on" : L"ask before binning · off");
    setTextIfChanged(control(DiagnosticLogging), state_.diagnosticLogging ? L"diagnostic logging · on" : L"diagnostic logging · off");
    setTextIfChanged(control(UpdateCheckSwitch), state_.updateCheck == UpdateChoice::On ? L"updates · on" : L"updates · off");
    // With a newer version known the button opens its page (the footer's text does too, but only for the mouse).
    setTextIfChanged(control(UpdateCheckNow), updates_.running() && updateManual_ ? L"checking…" :
        updateTag_.empty() ? L"check for updates" : L"open release page");
    // The list can change while recording; it only affects the next time a game comes to the front.
    EnableWindow(control(AutoCaptureSource), !closing_);
    const auto games = autoCapture_.games.size();
    setTextIfChanged(control(AutoCaptureSource), std::wstring(target_.source.monitor ? L"auto-capture · games only" :
        target_.source.window ? (autoCapture_.find(target_.path) ? L"auto-capture · on" : L"auto-capture · off") :
        (L"auto-capture · " + std::to_wstring(games) + (games == 1 ? L" game" : L" games"))));
    updateTrayTip();
    EnableWindow(control(ChimeVolume), !closing_); // Not a recording setting: never locked by replay.
    setTextIfChanged(control(CloseToTray), state_.closeToTray ? L"close to tray · on" : L"close to tray · off");
    setTextIfChanged(control(StartMinimised), state_.startMinimised ? L"start minimised · on" : L"start minimised · off");
    setTextIfChanged(control(StartWithWindows), startupReadFailed_ ? L"startup · unavailable" :
        startupOtherCopy_ ? L"startup · other copy" : state_.startWithWindows ? L"start with windows · on" : L"start with windows · off");
    EnableWindow(control(AssignHotkey), !closing_);
    EnableWindow(control(AssignScreenshot), !closing_);
    setTextIfChanged(control(AssignHotkey), assigning_ && assigningShortcut_ == saveShortcut ? L"press keys…" : L"assign save shortcut");
    setTextIfChanged(control(AssignScreenshot), assigning_ && assigningShortcut_ == screenshotShortcut ? L"press keys…" : L"assign screenshot shortcut");
    setTextIfChanged(control(StartReplay), busy_ ? L"please wait…" : off ? L"enable replay" :
        recording_ == capture::RecorderState::Stopped ? L"clear retained replay" : L"stop replay");
    setTextIfChanged(control(SaveReplay), saving_ ? L"saving…" : L"save replay");
    setTextIfChanged(control(DesktopAudio), deviceText(L"desktop audio", audio_.desktop));
    setTextIfChanged(control(Microphone), deviceText(L"microphone", audio_.microphone));
    setTextIfChanged(control(Suppression), audio_.micFilters.noiseSuppression ? L"noise suppression · on" : L"noise suppression · off");
    setTextIfChanged(control(Gate), audio_.micFilters.noiseGate ? L"noise gate · on" : L"noise gate · off");
    setTextIfChanged(control(RecordDiscord), audio_.hideDiscord ? L"record discord · off" : L"record discord · on");
    setTextIfChanged(control(Resolution), std::wstring(L"resolution · ") + resolutionNames[static_cast<int>(video_.resolution)]);
    setTextIfChanged(control(FrameRate), video_.fps == 60 ? L"frame rate · 60 fps" : L"frame rate · 30 fps");
    setTextIfChanged(control(QualityPreset), std::wstring(L"quality · ") + qualityNames[static_cast<int>(video_.quality)]);
    setTextIfChanged(control(VideoCodec), (std::wstring(L"codec · ") + capture::codecName(video_.codec)));
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
    } else if (message == WM_KILLFOCUS && (GetDlgCtrlID(button) == AssignHotkey || GetDlgCtrlID(button) == AssignScreenshot)) {
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
    InvalidateRect(search, nullptr, TRUE); // The placeholder comes and goes with the text.
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
    if (message == WM_PAINT && id == ClipSearch && GetWindowTextLengthW(edit) == 0) {
        // The empty search names itself, in the muted colour, where the text would start.
        if (const HDC dc = GetDC(edit)) {
            RECT text{};
            GetClientRect(edit, &text);
            text.left += LOWORD(SendMessageW(edit, EM_GETMARGINS, 0, 0)) + 1;
            const auto old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0)));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, muted);
            DrawTextW(dc, L"search clips", -1, &text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(dc, old);
            ReleaseDC(edit, dc);
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
        else if (id != ClipSearch) app->saveSettings(); // A number field is written to the file when left; typing only validates.
    }
    if (app->hoverVolume_ != hover || message == WM_SETFOCUS || message == WM_KILLFOCUS) app->invalidateFieldFrame(edit);
    return result;
}

} // namespace refarter
