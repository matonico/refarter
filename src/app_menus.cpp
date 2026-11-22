// App: every menu the app opens. The setting and source pickers, the clip menu, and the one place they are tracked.
#include "app_internal.h"
#include "codec_choice.h"
#include "message_box.h"
#include "notes_view.h"
#include "path_list.h"
#include "popup_menu.h"
#include "replay_duration.h"
#include "source_names.h"
#include "text.h"
#include "window_target.h"
#include "capture/audio_capture.h"
#include "capture/discord_audio.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>

namespace refarter {

// The source picker's windows, enumerated only while a menu opens: sorted, with a distinct menu label in displayName.
std::vector<RecordingTarget> App::runningWindows() const {
    std::vector<RecordingTarget> targets;
    EnumWindows([](HWND window, LPARAM param) -> BOOL {
        if (auto target = windowTarget(window)) reinterpret_cast<std::vector<RecordingTarget>*>(param)->push_back(std::move(*target));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&targets));
    std::stable_sort(targets.begin(), targets.end(), [](const auto& a, const auto& b) { return a.displayName < b.displayName; });
    labelSources(targets);
    return targets;
}

void App::labelSources(std::vector<RecordingTarget>& targets) const {
    std::vector<SourceName> names;
    // The custom name, else the short name whatever the switch says now: turning it on later must not make two picked
    // windows alike. (A game renamed after its window was picked is told apart from the next pick on.)
    for (const auto& target : targets) {
        const auto* custom = gameNames_.find(target.path);
        names.push_back({target.title, target.name, custom ? lower(custom->name) : gameName(storedGameName(target), target.path)});
    }
    auto labels = sourceLabels(names);
    for (size_t i = 0; i < targets.size(); ++i) targets[i].displayName = std::move(labels[i]);
}

// The video menus show and change the scope the page edits: all games, or the chosen game (see setScopeValue).
void App::chooseResolution() {
    PopupMenu styled;
    const auto menu = styled.root;
    const auto shown = scopeVideo();
    for (int i = 0; i < static_cast<int>(std::size(resolutionNames)); ++i) styled.append(menu, i + 1, resolutionNames[i],
        static_cast<int>(shown.resolution) == i ? MFS_CHECKED : MFS_ENABLED);
    RECT rect{}; GetWindowRect(control(Resolution), &rect);
    const auto selected = trackMenu(menu, rect.left, rect.bottom);
    if (selected && !pickerStale()) setScopeValue(ResolutionReset, static_cast<int>(selected - 1));
}

void App::chooseFrameRate() {
    PopupMenu styled;
    const auto shown = scopeVideo();
    for (const int fps : {30, 60})
        styled.append(styled.root, fps, std::to_wstring(fps) + L" fps", shown.fps == fps ? MFS_CHECKED : MFS_ENABLED);
    RECT rect{}; GetWindowRect(control(FrameRate), &rect);
    if (const auto selected = trackMenu(styled.root, rect.left, rect.bottom); selected && !pickerStale()) setScopeValue(FrameRateReset, static_cast<int>(selected));
}

// The welcome screen's replay length: the four presets of the Recording page, applied by start.
void App::chooseWelcomeLength() {
    PopupMenu styled;
    for (const int seconds : {30, 60, 180, 300})
        styled.append(styled.root, seconds, formatReplayDuration(seconds), welcomeSeconds_ == seconds ? MFS_CHECKED : MFS_ENABLED);
    RECT rect{}; GetWindowRect(control(WelcomeLength), &rect);
    const auto selected = trackMenu(styled.root, rect.left, rect.bottom);
    if (!selected || !welcome_ || closing_) return;
    welcomeSeconds_ = static_cast<int>(selected);
    refreshControls();
    InvalidateRect(control(WelcomeLength), nullptr, FALSE);
}

bool App::pickerStale() const {
    // Nothing is saved or changed for a menu that outlived the state it was opened in (an auto-capture start, a save, a quit).
    return closing_ || busy_ || recording_ != capture::RecorderState::Off;
}

void App::chooseQuality() {
    PopupMenu styled;
    const auto menu = styled.root;
    const wchar_t* entries[] = {L"small · lightest files, softer in fast action", L"balanced · good for most games",
        L"high · stays sharp in fast action", L"custom · a fixed bitrate you set"};
    const auto shown = scopeVideo();
    for (int i = 0; i < 4; ++i) {
        styled.append(menu, i + 1, entries[i], static_cast<int>(shown.quality) == i ? MFS_CHECKED : MFS_ENABLED);
    }
    RECT rect{}; GetWindowRect(control(QualityPreset), &rect);
    if (const auto selected = trackMenu(menu, rect.left, rect.bottom); selected && !pickerStale())
        setScopeValue(QualityReset, static_cast<int>(selected - 1));
}

namespace {
// Each codec in menu order: what it is for (in the menu and the help's heading), then the help's two notes.
struct CodecText { capture::Codec codec; const wchar_t* pitch; const wchar_t* good; const wchar_t* watchOut; };
constexpr CodecText codecTexts[] = {
    {capture::Codec::H264, L"plays everywhere",
     L"every phone, browser, chat app and editor plays it. smooth editing. every supported gpu records it.",
     L"biggest files. fast motion can look blocky at low bitrates."},
    {capture::Codec::HEVC, L"fewer apps play it",
     L"about 10% smaller than h.264. most recent gpus record it.",
     L"a little softer than h.264. many browsers and chat apps can't play it. windows may need a paid extension."},
    {capture::Codec::AV1, L"smallest files",
     L"about 25% smaller than h.264. discord and youtube play it.",
     L"a little softer than h.264. needs an rtx 40, rx 7000, arc or newer gpu. older iphones and older editors may not play it."},
};

// The probe's reason as part of a sentence in the help's markdown: one line (a Windows error message may span several),
// no markdown markers, lower case, without a closing full stop.
std::wstring reasonText(const std::wstring& problem) {
    std::wstring text;
    for (const wchar_t c : problem) {
        if (c == L'*' || c == L'`' || c == L'<' || c == L'[') continue;
        const bool space = c == L'\r' || c == L'\n' || c == L'\t' || c == L' ';
        if (space) { if (!text.empty() && text.back() != L' ') text += L' '; }
        else text += c;
    }
    while (!text.empty() && (text.back() == L'.' || text.back() == L' ')) text.pop_back();
    return text.empty() ? L"no reason was given" : lower(text);
}
} // namespace

void App::chooseCodec() {
    probeCodecs();
    PopupMenu styled;
    const auto menu = styled.root;
    const auto recommended = recommendedCodec(codecSupport(capture::Codec::AV1), probing_);
    const auto shown = scopeVideo();
    for (UINT i = 0; i < std::size(codecTexts); ++i) {
        const auto& entry = codecTexts[i];
        const auto state = codecState(entry.codec, codecSupport(entry.codec), probing_);
        std::wstring text = std::wstring(capture::codecName(entry.codec)) + L" · ";
        switch (state) {
        case CodecState::Records:
            text += entry.pitch;
            if (recommended == entry.codec) text += L"\t(recommended)";
            break;
        case CodecState::Checking: text += L"checking your gpu…"; break;
        case CodecState::NotChecked: text += L"stop replay to check your gpu"; break;
        case CodecState::CheckFailed: text += L"recording check failed"; break;
        case CodecState::TestFailed: text += L"couldn't record a test clip"; break; // The help says why.
        }
        styled.append(menu, i + 1, std::move(text),
            (state == CodecState::Records ? MFS_ENABLED : MFS_DISABLED) | (shown.codec == entry.codec ? MFS_CHECKED : 0));
    }
    constexpr UINT Help = 100;
    styled.separator(menu);
    styled.append(menu, Help, L"which codec should i pick?", dialogOpen() ? MFS_DISABLED : MFS_ENABLED); // One dialog at a time.
    RECT rect{}; GetWindowRect(control(VideoCodec), &rect);
    if (const auto selected = trackMenu(menu, rect.left, rect.bottom)) {
        if (selected >= 1 && selected <= std::size(codecTexts)) {
            if (!pickerStale()) setScopeValue(CodecReset, static_cast<int>(codecTexts[selected - 1].codec));
        } else if (selected == Help) {
            showCodecHelp();
        }
    }
}

void App::showCodecHelp() {
    if (closing_ || dialogOpen()) return;
    const auto blocks = notes::parse(codecHelpMarkdown());

    // Other dialogs wait while it is open, but a save still runs (as with what's new).
    infoOpen_ = true;
    NotesView view;
    // Up to 800 px tall (the dialog keeps within the screen), then shrunk to the text so nothing scrolls.
    contentBox(window_, L"which codec should i pick?", 520, 800, [&](HWND parent, RECT area) {
        const HWND body = view.create(parent, contentBoxBodyId, L"which codec should i pick?");
        if (!body) return body;
        view.setBackground(surface);
        if (!view.setStyle(GetDpiForWindow(parent))) { // Without fonts the body would be blank: the dialog shows plain text.
            DestroyWindow(body);
            return static_cast<HWND>(nullptr);
        }
        view.setNotes(blocks);
        SetWindowPos(body, nullptr, area.left, area.top, area.right - area.left, area.bottom - area.top, SWP_NOZORDER | SWP_SHOWWINDOW);
        return body;
    }, [&] { return view.contentHeight(); }, L"got it", notes::plainText(blocks));
    infoOpen_ = false;
    resumeHeldDialogs(); // A save under the dialog may have queued an error or the storage question.
}

std::wstring App::codecHelpMarkdown() const {
    const auto recommended = recommendedCodec(codecSupport(capture::Codec::AV1), probing_);
    // Each codec's rate at the current quality, frame rate and encoded size; nothing while no source gives a size. Custom
    // quality is one fixed rate for every codec, so it is said once instead. The values are the page's scope: all
    // games, or the chosen game's effective ones.
    const auto video = scopeRecordVideo(); // A game's refused codec as the h.264 it records.
    const auto basis = scopeSize(); // The chosen game's window, or the screen until that game has been the source.
    const auto output = encodedSize(video, basis.size);
    const bool rates = output.width > 0 && output.height > 0;
    const bool custom = video.quality == capture::Quality::Custom;
    std::wstring markdown;
    if (rates) {
        // An invalid custom field holds no rate yet: quoting the last valid one would contradict it.
        if (custom && bitrateValid_) {
            const auto rate = codecRate(video, video.codec, output);
            markdown += L"at custom quality every codec records at " + std::to_wstring(rate.averageMbps) + L" mbps (about " +
                std::to_wstring(rate.mbPerMinute) + L" mb of video a minute), so av1 makes sharper clips at that size, "
                L"not smaller ones.\n\n";
        } else if (!custom) {
            const auto size = std::to_wstring(output.width) + L"×" + std::to_wstring(output.height);
            markdown += L"sizes are for your settings: " + std::wstring(qualityNames[static_cast<int>(video.quality)]) +
                L" quality, " + size + (basis.guessed ? L" (your screen, until " + reasonText(shownGameName(settingsFor_, settingsForLabel_)) + L" is chosen as a source)" : L"") +
                L", " + std::to_wstring(video.fps) + L" fps.\n\n";
        }
    }
    for (const auto& entry : codecTexts) {
        const std::wstring name = capture::codecName(entry.codec);
        markdown += L"## " + name + L" · " + entry.pitch + (recommended == entry.codec ? L" (recommended)" : L"") + L"\n\n";
        markdown += L"- **good:** " + std::wstring(entry.good) + L"\n";
        markdown += L"- **watch out:** " + std::wstring(entry.watchOut) + L"\n";
        if (rates && !custom) {
            const auto rate = codecRate(video, entry.codec, output);
            markdown += L"- **size:** about " + std::to_wstring(rate.averageMbps) + L" mbps, up to " +
                std::to_wstring(rate.peakMbps) + L" in fast motion (about " + std::to_wstring(rate.mbPerMinute) +
                L" mb of video a minute)\n";
        }
        // Only what this PC changes: a check still to come, its failure reason, or a failed Windows playback check.
        const auto* support = codecSupport(entry.codec);
        std::wstring local;
        switch (codecState(entry.codec, support, probing_)) {
        case CodecState::Checking: local = L"checking your gpu…"; break;
        case CodecState::NotChecked: local = L"not checked yet: stop replay, then open the codec menu again."; break;
        case CodecState::CheckFailed:
        case CodecState::TestFailed:
            local = L"the recording check failed (" + reasonText(support->problem) + L").";
            break;
        case CodecState::Records:
            if (support && !support->plays && entry.codec != capture::Codec::H264)
                local = entry.codec == capture::Codec::AV1
                    ? L"windows couldn't play the test clip. it may need the free av1 extension from the microsoft store, but other playback failures are possible. refarter's player supports av1 without it."
                    : L"windows couldn't play the test clip. it may need the hevc extension from the microsoft store, which may cost a little, but other playback failures are possible. refarter's player supports hevc without it.";
            break;
        default: break;
        }
        if (!local.empty()) markdown += L"- **on this pc:** " + local + L"\n";
        markdown += L"\n";
    }
    return markdown;
}

void App::chooseSource() {
    auto targets = runningWindows();
    PopupMenu styled;
    const auto menu = styled.root;
    for (size_t i = 0; i < targets.size(); ++i) {
        // Games on the auto-capture list say so, so the picker shows what starts by itself.
        styled.append(menu, static_cast<UINT>(i + 1),
                      targets[i].displayName + (autoCapture_.find(targets[i].path) ? L" · auto-capture" : L""),
                      target_.source.window == targets[i].source.window ? MFS_CHECKED : MFS_ENABLED);
    }
    if (targets.empty()) styled.append(menu, 0, L"no available game windows · open your game and try again", MFS_DISABLED);
    styled.separator(menu);
    const auto displays = displayMonitors();
    const size_t firstMonitor = targets.size() + 1;
    for (size_t i = 0; i < displays.size(); ++i) {
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(displays[i], &info);
        auto name = L"monitor " + std::to_wstring(i + 1) + L" · entire display · " +
            std::to_wstring(info.rcMonitor.right - info.rcMonitor.left) + L" × " +
            std::to_wstring(info.rcMonitor.bottom - info.rcMonitor.top) +
            ((info.dwFlags & MONITORINFOF_PRIMARY) ? L" · primary" : L"");
        styled.append(menu, static_cast<UINT>(firstMonitor + i), std::move(name),
                      target_.source.monitor == displays[i] ? MFS_CHECKED : MFS_ENABLED);
    }
    // Back to automatic mode: clears the chosen source so listed games start replay themselves.
    const auto automatic = static_cast<UINT>(firstMonitor + displays.size());
    styled.separator(menu);
    const bool waiting = !target_.source.window && !target_.source.monitor && autoCaptureOn_ && autoCaptureReadable_;
    styled.append(menu, automatic, L"auto-capture listed games",
                  (canUseAutoCapture() ? MFS_ENABLED : MFS_DISABLED) | (waiting ? MFS_CHECKED : 0));
    RECT rect{}; GetWindowRect(control(SelectProcess), &rect);
    const auto selection = trackMenu(menu, rect.left, rect.bottom);
    if (selection == automatic) return useAutoCapture(); // Rechecks the state itself.
    if (pickerStale()) return;
    if (selection && selection < firstMonitor) {
        target_ = std::move(targets[selection - 1]);
        setSourceMonitor({}); // Games are remembered through the auto-capture list instead.
    } else if (selection >= firstMonitor && selection - firstMonitor < displays.size()) {
        target_ = {{nullptr, displays[selection - firstMonitor]}, 0, {}, L"monitor " + std::to_wstring(selection - firstMonitor + 1), L"whole monitor"};
        setSourceMonitor(monitorDevice(target_.source.monitor));
    }
    if (selection) {
        setStatus(L"source selected · enable replay when ready", true);
        followSource(); // The page opens on the new source's own settings, if it has them.
        refreshPages(); // The new status too, even when followSource had nothing to change.
    }
}

// A sign-in retry still waiting must not start on a source the user only picked, and Settings' start replay note
// follows the source (followSource refreshes only when the page's scope moves).
void App::setSourceMonitor(std::wstring device) {
    sourceMonitor_ = std::move(device);
    cancelSignInReplay();
    refreshPages();
}

void App::chooseAudioDevice(bool desktop) {
    using Mode = capture::AudioDevice::Mode;
    auto& current = desktop ? audio_.desktop : audio_.microphone;
    // Listed only while the menu opens; nothing watches devices in the background.
    const auto endpoints = capture::listAudioEndpoints(desktop ? capture::AudioCapture::Device::Desktop :
                                                                 capture::AudioCapture::Device::Microphone);
    const auto labels = capture::endpointLabels(endpoints);
    enum { Off = 1, SystemDefault, FirstDevice };
    PopupMenu styled;
    const auto menu = styled.root;
    styled.append(menu, Off, L"off", current.mode == Mode::Off ? MFS_CHECKED : MFS_ENABLED);
    std::wstring standard = L"system default · follows windows";
    for (size_t i = 0; i < endpoints.size(); ++i)
        if (endpoints[i].isDefault) standard = L"system default · now " + lower(labels[i]);
    styled.append(menu, SystemDefault, std::move(standard), current.mode == Mode::SystemDefault ? MFS_CHECKED : MFS_ENABLED);
    styled.separator(menu);
    bool listed = false;
    for (size_t i = 0; i < endpoints.size(); ++i) {
        const bool chosen = current.mode == Mode::Fixed && _wcsicmp(current.id.c_str(), endpoints[i].id.c_str()) == 0;
        listed = listed || chosen;
        styled.append(menu, static_cast<UINT>(FirstDevice + i), lower(labels[i]), chosen ? MFS_CHECKED : MFS_ENABLED);
    }
    // A saved device that is unplugged stays chosen and visibly unavailable; it is never swapped.
    const auto missing = static_cast<UINT>(FirstDevice + endpoints.size());
    if (current.mode == Mode::Fixed && !listed)
        styled.append(menu, missing, (current.name.empty() ? L"chosen device" : lower(current.name)) +
            L" · unavailable · records once connected", MFS_CHECKED);
    if (endpoints.empty())
        styled.append(menu, 0, desktop ? L"no active outputs found" : L"no active microphones found", MFS_DISABLED);
    RECT rect{}; GetWindowRect(control(desktop ? DesktopAudio : Microphone), &rect);
    const auto selected = trackMenu(menu, rect.left, rect.bottom);
    if (pickerStale()) return;
    if (selected == Off) current = {};
    else if (selected == SystemDefault) current = {Mode::SystemDefault};
    else if (selected >= FirstDevice && selected < missing) {
        const auto& endpoint = endpoints[selected - FirstDevice];
        current = {Mode::Fixed, endpoint.id, endpoint.name};
    }
    if (desktop) checkDiscordOutput();
}

void App::chooseAutoCapture() {
    // Just the list: ticked games (click to remove), then ways to add one. The switches live in Settings,
    // and returning to auto-capture from a monitor is in the source picker.
    enum { ThisGame = 1, FirstGame = 100, FirstRunning = 10'000 };
    PopupMenu styled;
    const auto menu = styled.root;
    const bool game = target_.source.window && !target_.path.empty();
    // Copies: an auto-start while the menu is open may change the source.
    // The game's stored name, or the whole title it would be added with (not the picker's label and suffixes).
    const auto path = target_.path, label = storedGameName(target_);
    const auto games = autoCapture_.games;
    const UINT editable = autoCaptureReadable_ ? MFS_ENABLED : MFS_DISABLED;
    if (!autoCaptureReadable_) styled.append(menu, 0, L"the list could not be read", MFS_DISABLED);
    for (size_t i = 0; i < games.size(); ++i)
        styled.append(menu, static_cast<UINT>(FirstGame + i), shownGameName(games[i].path, games[i].label), editable | MFS_CHECKED);
    if (games.empty()) styled.append(menu, 0, L"no games yet", MFS_DISABLED);
    styled.separator(menu);
    if (game && !autoCapture_.find(path)) styled.append(menu, ThisGame, L"+ " + shownName(displayGameName(path, label)), editable);
    // Running windows, read once as the menu opens; listed ones are ticked and a click removes them.
    const auto running = runningWindows();
    const auto add = styled.submenu();
    for (size_t i = 0; i < running.size(); ++i)
        styled.append(add, static_cast<UINT>(FirstRunning + i), oneLineLabel(running[i].displayName),
                      editable | (autoCapture_.find(running[i].path) ? MFS_CHECKED : 0));
    if (running.empty()) styled.append(add, 0, L"no game windows open", MFS_DISABLED);
    styled.append(menu, 0, L"+ add running game", MFS_ENABLED, add);
    RECT rect{}; GetWindowRect(control(AutoCaptureSource), &rect);
    const auto selected = trackMenu(menu, rect.left, rect.bottom);
    if (selected == ThisGame) setAutoCaptured(path, label, true);
    else if (selected >= FirstRunning && selected - FirstRunning < running.size()) {
        // A ticked game is removed, as from the list below; the label is the title, without the picker's suffixes.
        const auto& window = running[selected - FirstRunning];
        setAutoCaptured(window.path, storedGameName(window), !autoCapture_.find(window.path));
    } else if (selected >= FirstGame && selected - FirstGame < games.size()) {
        setAutoCaptured(games[selected - FirstGame].path, {}, false);
    }
}

namespace {
// What a game sets itself, in the page's order: " · 1080p · high · 1 min". Its bitrate only while it records at custom
// quality (`custom`): otherwise the bitrate changes nothing (see ownValuesApply).
std::wstring ownValues(const GameVideo& own, bool custom) {
    std::wstring text;
    if (own.resolution) text += std::wstring(L" · ") + resolutionNames[static_cast<int>(*own.resolution)];
    if (own.fps) text += L" · " + std::to_wstring(*own.fps) + L" fps";
    if (own.quality) text += std::wstring(L" · ") + qualityNames[static_cast<int>(*own.quality)];
    if (own.bitrateMbps && custom) text += L" · " + std::to_wstring(*own.bitrateMbps) + L" mbps";
    if (own.codec) text += std::wstring(L" · ") + capture::codecName(*own.codec);
    if (own.replaySeconds) text += L" · " + formatReplayDuration(*own.replaySeconds);
    return text;
}
} // namespace

// "settings for": which game the Recording page edits. Choosing one never changes the source and creates nothing
// until a value is changed.
void App::chooseSettingsFor() {
    enum : UINT { AllGames = 1, FirstOwn = 100, FirstListed = 5'000, FirstRunning = 10'000 };
    PopupMenu styled;
    const auto menu = styled.root;
    const auto chosen = [this](const std::wstring& path) {
        return gameScope() && samePath(path, settingsFor_) ? MFS_CHECKED : MFS_ENABLED;
    };
    // Copies: an auto-start while the menu is open may change the source or the lists.
    const auto own = gameSettings_.games;
    std::vector<AutoCaptureGame> listed; // Listed games without their own values, then the source game if it is neither.
    for (const auto& game : autoCapture_.games)
        if (!gameSettings_.find(game.path)) listed.push_back(game);
    if (target_.source.window && !target_.path.empty() && !gameSettings_.find(target_.path) && !autoCapture_.find(target_.path))
        listed.push_back({target_.path, storedGameName(target_)});
    const auto running = runningWindows(); // As auto-capture's "add a running game": read once as the menu opens.
    styled.append(menu, AllGames, L"all games · defaults", gameScope() ? MFS_ENABLED : MFS_CHECKED);
    if (!own.empty()) {
        styled.separator(menu);
        styled.append(menu, 0, L"own settings", MFS_DISABLED);
        for (size_t i = 0; i < own.size(); ++i)
            styled.append(menu, static_cast<UINT>(FirstOwn + i), shownGameName(own[i].path, own[i].label) +
                ownValues(own[i].video, resolveVideo(video_, state_.replaySeconds, &own[i].video).quality == capture::Quality::Custom),
                chosen(own[i].path));
    }
    if (!listed.empty()) {
        styled.separator(menu);
        // Listed games and possibly the unlisted source game: "listed" would say the source is auto-captured.
        styled.append(menu, 0, L"other games · using all games", MFS_DISABLED);
        for (size_t i = 0; i < listed.size(); ++i)
            styled.append(menu, static_cast<UINT>(FirstListed + i), shownGameName(listed[i].path, listed[i].label), chosen(listed[i].path));
    }
    styled.separator(menu);
    const auto add = styled.submenu();
    for (size_t i = 0; i < running.size(); ++i)
        styled.append(add, static_cast<UINT>(FirstRunning + i), oneLineLabel(running[i].displayName), chosen(running[i].path));
    if (running.empty()) styled.append(add, 0, L"no available game windows · open your game and try again", MFS_DISABLED);
    styled.append(menu, 0, L"add a running game", MFS_ENABLED, add);
    RECT rect{}; GetWindowRect(control(SettingsFor), &rect);
    const auto selected = trackMenu(menu, rect.left, rect.bottom);
    if (!selected || pickerStale() || !gameSettingsReadable_) return;
    if (selected == AllGames) setScope({}, {});
    else if (selected >= FirstOwn && selected - FirstOwn < own.size()) {
        const auto& entry = own[selected - FirstOwn];
        setScope(entry.path, entry.label);
    } else if (selected >= FirstListed && selected - FirstListed < listed.size())
        setScope(listed[selected - FirstListed].path, listed[selected - FirstListed].label);
    else if (selected >= FirstRunning && selected - FirstRunning < running.size()) {
        // A known game keeps its name; another is named as auto-capture would name it.
        const auto& window = running[selected - FirstRunning];
        setScope(window.path, storedGameName(window));
    }
}

void App::chooseClipOrder() {
    enum { FirstSort = 1, First = 10, Second, FirstPeriod = 20 };
    PopupMenu styled;
    const auto menu = styled.root;
    const auto order = clipGrid_.order();
    for (int i = 0; i < static_cast<int>(std::size(clipSortNames)); ++i)
        styled.append(menu, FirstSort + i, std::wstring(L"sort by ") + clipSortNames[i], static_cast<int>(order.by) == i ? MFS_CHECKED : MFS_ENABLED);
    styled.separator(menu);
    // The key's usual direction first ("newest first", then "oldest first"; "a → z" before "z → a").
    const bool usual = naturallyDescending(order.by);
    styled.append(menu, First, clipOrderLabel({order.by, usual}), order.descending == usual ? MFS_CHECKED : MFS_ENABLED);
    styled.append(menu, Second, clipOrderLabel({order.by, !usual}), order.descending != usual ? MFS_CHECKED : MFS_ENABLED);
    styled.separator(menu);
    // The date filter is for this session only, like the search.
    for (int i = 0; i < static_cast<int>(std::size(clipPeriodNames)); ++i)
        styled.append(menu, FirstPeriod + i, std::wstring(L"from ") + clipPeriodNames[i],
                      static_cast<int>(clipGrid_.period()) == i ? MFS_CHECKED : MFS_ENABLED);
    RECT rect{}; GetWindowRect(control(SortClips), &rect);
    const auto selected = static_cast<int>(trackMenu(menu, rect.left, rect.bottom));
    if (selected >= FirstSort && selected < FirstSort + static_cast<int>(std::size(clipSortNames))) {
        const auto by = static_cast<ClipSort>(selected - FirstSort);
        state_.clipOrder = {by, by == order.by ? order.descending : naturallyDescending(by)};
    } else if (selected == First || selected == Second) {
        state_.clipOrder = {order.by, selected == First ? usual : !usual};
    } else if (selected >= FirstPeriod && selected < FirstPeriod + static_cast<int>(std::size(clipPeriodNames))) {
        clipGrid_.setPeriod(static_cast<ClipPeriod>(selected - FirstPeriod));
        InvalidateRect(control(SortClips), nullptr, FALSE); // Shown as active while a date filter is set.
        InvalidateRect(window_, nullptr, FALSE);           // The empty state's wording follows the filter.
    }
    clipGrid_.setOrder(state_.clipOrder);
}

UINT App::trackMenu(HMENU menu, int x, int y) {
    if (answerMenu_) return answerMenu_(menu); // A test chooses; a modal menu would hold it.
    return showPopupMenu(window_, menu, {x, y}, dpi_);
}

void App::showClipMenu() {
    const auto path = clipGrid_.target();
    const auto at = clipGrid_.menuPoint();
    const auto count = clipGrid_.selectedCount();
    const bool several = count > 1; // Open, rename and the rest work on one clip only.
    enum { Open = 1, External, ShowInFolder, Rename, Copy, Favourite, Recycle, Compress, CancelCompression, Combine, CancelCombine };
    PopupMenu styled;
    const UINT single = several ? MFS_DISABLED : MFS_ENABLED;
    styled.append(styled.root, Open, L"open", single);
    styled.append(styled.root, External, L"open in external player", single);
    styled.append(styled.root, ShowInFolder, L"show in folder", single);
    styled.append(styled.root, Rename, L"rename", !several && !playerOpen_ && clipGrid_.canRename(path) ? MFS_ENABLED : MFS_DISABLED);
    styled.append(styled.root, Copy, several ? L"copy " + std::to_wstring(count) + L" clips" : L"copy");
    const bool addFavourite = !clipGrid_.selectedAreFavourites(); // Decided once: the label and the click agree.
    styled.append(styled.root, Favourite, addFavourite ? L"add favourite" : L"remove favourite");
    if (combining_) styled.append(styled.root, CancelCombine, L"cancel combining clips", compression_.cancelling() ? MFS_DISABLED : MFS_ENABLED);
    else if (several) styled.append(styled.root, Combine, L"combine clips…", !compression_.active() && !playerOpen_ && !player_.trimBusy() && !closing_ ? MFS_ENABLED : MFS_DISABLED);
    if (!combining_ && !several && path == compression_.clip()) styled.append(styled.root, CancelCompression, L"cancel compression", compression_.cancelling() ? MFS_DISABLED : MFS_ENABLED);
    else styled.append(styled.root, Compress, L"compress to 20mb", !several && !compression_.active() && !closing_ ? MFS_ENABLED : MFS_DISABLED);
    styled.append(styled.root, Recycle, several ? L"move " + std::to_wstring(count) + L" clips to recycle bin" : L"move to recycle bin",
        !several && path == compression_.clip() ? MFS_DISABLED : MFS_ENABLED);
    switch (trackMenu(styled.root, at.x, at.y)) {
    case Open: openClip(path); break;
    case External: openExternally(path); break;
    case ShowInFolder: showInFolder(path); break;
    case Rename: clipGrid_.rename(path); break;
    case Copy: clipGrid_.copySelection(); break;
    case Favourite: clipGrid_.favouriteSelection(addFavourite); break;
    case Recycle: clipGrid_.recycleSelection(); break;
    case Compress: startCompression(path); break;
    case Combine: startCombine(); break;
    case CancelCombine: compression_.cancel(); break;
    case CancelCompression:
        compression_.cancel();
        trimFollowUp_.cancel(); // A copy that was waiting for this compression is cancelled with it.
        clipGrid_.compression(compression_.clip(), compression_.progress(), true);
        break;
    }
}

void App::chooseFolder() {
    winrt::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) return;
    DWORD options = 0; dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"choose a clip folder");
    if (SUCCEEDED(dialog->Show(window_))) {
        // The dialog is modal but the recorder is not: if a save, a start or a quit began meanwhile, drop the choice.
        if (busy_ || saving_ || closing_) return;
        winrt::com_ptr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(dialog->GetResult(item.put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            busy_ = true;
            setStatus(L"checking clip folder…");
            recorder_->folder(path);
            CoTaskMemFree(path);
        }
    }
}

} // namespace refarter
