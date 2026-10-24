// App: every menu the app opens. The setting and source pickers, the clip menu, and the one place they are tracked.
#include "app_internal.h"
#include "message_box.h"
#include "popup_menu.h"
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

void App::chooseResolution() {
    PopupMenu styled;
    const auto menu = styled.root;
    for (int i = 0; i < static_cast<int>(std::size(resolutionNames)); ++i) styled.append(menu, i + 1, resolutionNames[i],
        static_cast<int>(video_.resolution) == i ? MFS_CHECKED : MFS_ENABLED);
    RECT rect{}; GetWindowRect(control(Resolution), &rect);
    const auto selected = trackMenu(menu, rect.left, rect.bottom);
    if (selected && !pickerStale()) video_.resolution = static_cast<capture::Resolution>(selected - 1);
}

void App::chooseFrameRate() {
    PopupMenu styled;
    for (const int fps : {30, 60})
        styled.append(styled.root, fps, std::to_wstring(fps) + L" fps", video_.fps == fps ? MFS_CHECKED : MFS_ENABLED);
    RECT rect{}; GetWindowRect(control(FrameRate), &rect);
    if (const auto selected = trackMenu(styled.root, rect.left, rect.bottom); selected && !pickerStale()) video_.fps = static_cast<int>(selected);
}

bool App::pickerStale() const {
    // Nothing is saved or changed for a menu that outlived the state it was opened in (an auto-capture start, a save, a quit).
    return closing_ || busy_ || recording_ != capture::RecorderState::Off;
}

void App::chooseQuality() {
    PopupMenu styled;
    const auto menu = styled.root;
    const wchar_t* entries[] = {L"small · fewest mb, softer in fast motion", L"balanced · about obs replay size",
        L"high · more detail in fast motion", L"custom · constant bitrate you choose"};
    for (int i = 0; i < 4; ++i) {
        styled.append(menu, i + 1, entries[i], static_cast<int>(video_.quality) == i ? MFS_CHECKED : MFS_ENABLED);
    }
    RECT rect{}; GetWindowRect(control(QualityPreset), &rect);
    if (const auto selected = trackMenu(menu, rect.left, rect.bottom); selected && !pickerStale())
        video_.quality = static_cast<capture::Quality>(selected - 1);
}

void App::chooseCodec() {
    probeCodecs();
    PopupMenu styled;
    const auto menu = styled.root;
    const capture::Codec codecs[] = {capture::Codec::H264, capture::Codec::HEVC, capture::Codec::AV1};
    for (int i = 0; i < 3; ++i) {
        std::wstring text = capture::codecName(codecs[i]);
        bool available = i == 0; // h.264 is the baseline every supported gpu records.
        if (i == 0) text += L" · most compatible";
        if (i > 0) {
            const auto* support = codecSupport(codecs[i]);
            if (probing_) text += L" · checking your gpu…";
            else if (!support) text += L" · stop replay to check your gpu";
            else if (!support->records) text += L" · unavailable: " + lower(support->problem);
            else {
                available = true;
                text += L" · recording supported";
            }
        }
        styled.append(menu, i + 1, std::move(text),
            (available ? MFS_ENABLED : MFS_DISABLED) | (video_.codec == codecs[i] ? MFS_CHECKED : 0));
    }
    enum Help { AboutPlayback = 100, WindowsAV1, WindowsHEVC, VLC };
    const auto help = styled.submenu();
    styled.append(help, AboutPlayback, L"about recording and playback");
    styled.separator(help);
    styled.append(help, WindowsAV1, L"windows av1 extension · free");
    styled.append(help, WindowsHEVC, L"windows hevc extension · may require payment");
    styled.append(help, VLC, L"vlc · independent playback support");
    styled.separator(menu);
    styled.append(menu, 0, L"playback help", MFS_ENABLED, help);
    RECT rect{}; GetWindowRect(control(VideoCodec), &rect);
    if (const auto selected = trackMenu(menu, rect.left, rect.bottom)) {
        if (selected >= 1 && selected <= std::size(codecs)) {
            if (!pickerStale()) video_.codec = codecs[selected - 1];
        }
        else if (selected == AboutPlayback) {
            const auto* support = codecSupport(video_.codec);
            const std::wstring playback = std::wstring(capture::codecName(video_.codec)) +
                (support ? (support->plays ? L" · windows playback supported\n\n" :
                    L" · windows playback check failed\n\n") : L" · windows playback not checked\n\n");
            messageBox(window_,
                (playback + L"recording and playback are separate.\n\n"
                L"recording supported means your gpu encoded a test clip and windows saved it successfully. "
                L"a failed windows playback check does not prevent recording, and does not test vlc or other independent players.\n\n"
                L"windows media player may need a decoder extension. a failed check can also have other causes. "
                L"the playback help menu links to the official extensions and vlc. "
                L"buying a playback extension is not required to use a working gpu encoder.\n\n"
                L"h.264 is the default for compatibility. hevc and av1 can make smaller clips, but sizes and quality depend on the content and settings.").c_str(),
                L"codec playback", MB_OK | MB_ICONINFORMATION);
        } else {
            const wchar_t* url = selected == WindowsAV1 ? L"https://apps.microsoft.com/detail/9mvzqvxjbq9v" :
                selected == WindowsHEVC ? L"https://apps.microsoft.com/detail/9nmzlz57r3t7" :
                selected == VLC ? L"https://www.videolan.org/vlc/" : nullptr;
            if (url) shellOpen(url, L"windows could not open the link in your browser.");
        }
    }
}

void App::chooseSource() {
    std::vector<RecordingTarget> targets;
    EnumWindows([](HWND window, LPARAM param) -> BOOL {
        if (auto target = windowTarget(window)) reinterpret_cast<std::vector<RecordingTarget>*>(param)->push_back(std::move(*target));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&targets));
    std::stable_sort(targets.begin(), targets.end(), [](const auto& a, const auto& b) { return a.displayName < b.displayName; });
    std::vector<SourceName> names;
    for (const auto& target : targets) names.push_back({target.title, target.name});
    auto labels = sourceLabels(names);
    PopupMenu styled;
    const auto menu = styled.root;
    for (size_t i = 0; i < targets.size(); ++i) {
        targets[i].displayName = std::move(labels[i]);
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
    RECT rect{}; GetWindowRect(control(SelectProcess), &rect);
    const auto selection = trackMenu(menu, rect.left, rect.bottom);
    if (pickerStale()) return;
    if (selection && selection < firstMonitor) {
        target_ = std::move(targets[selection - 1]);
        sourceMonitor_.clear(); // Games are remembered through the auto-capture list instead.
    } else if (selection >= firstMonitor && selection - firstMonitor < displays.size()) {
        target_ = {{nullptr, displays[selection - firstMonitor]}, 0, {}, L"monitor " + std::to_wstring(selection - firstMonitor + 1), L"whole monitor"};
        sourceMonitor_ = monitorDevice(target_.source.monitor);
    }
    if (selection) setStatus(L"source selected · enable replay when ready", true);
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
    enum { ThisGame = 1, Automatic, Offers, UseAutomatic, FirstGame = 100 };
    PopupMenu styled;
    const auto menu = styled.root;
    const bool game = target_.source.window && !target_.path.empty();
    const bool listed = game && autoCapture_.find(target_.path);
    // Copies: an auto-start while the menu is open may change the source.
    const auto path = target_.path, label = target_.displayName;
    const auto games = autoCapture_.games;
    const UINT editable = autoCaptureReadable_ ? MFS_ENABLED : MFS_DISABLED;
    if (!autoCaptureReadable_) styled.append(menu, 0, L"the auto-capture list could not be read · nothing is changed", MFS_DISABLED);
    if (game) styled.append(menu, ThisGame, L"auto-capture " + lower(target_.displayName), editable | (listed ? MFS_CHECKED : 0));
    else if (target_.source.monitor) styled.append(menu, 0, L"a whole monitor is never auto-captured", MFS_DISABLED);
    else styled.append(menu, 0, L"choose a game window to add it", MFS_DISABLED);
    styled.append(menu, UseAutomatic, L"use auto-capture", canUseAutoCapture() ? MFS_ENABLED : MFS_DISABLED);
    styled.separator(menu);
    if (games.empty()) styled.append(menu, 0, L"no games on the list yet", MFS_DISABLED);
    for (size_t i = 0; i < games.size(); ++i)
        styled.append(menu, static_cast<UINT>(FirstGame + i), lower(games[i].label) + L" · click to remove",
                      editable | MFS_CHECKED);
    styled.separator(menu);
    styled.append(menu, Automatic, L"start listed games automatically", autoCaptureOn_ ? MFS_CHECKED : MFS_ENABLED);
    styled.append(menu, Offers, L"offer new fullscreen games", autoCapturePrompt_ ? MFS_CHECKED : MFS_ENABLED);
    RECT rect{}; GetWindowRect(control(AutoCaptureSource), &rect);
    const auto selected = trackMenu(menu, rect.left, rect.bottom);
    if (selected == UseAutomatic) useAutoCapture();
    else if (selected == ThisGame) setAutoCaptured(path, label, !listed);
    else if (selected >= FirstGame && selected - FirstGame < games.size()) {
        setAutoCaptured(games[selected - FirstGame].path, {}, false);
    } else if (selected == Automatic) {
        autoCaptureOn_ = !autoCaptureOn_;
        auto_.updateHook();
    } else if (selected == Offers) {
        autoCapturePrompt_ = !autoCapturePrompt_;
        auto_.updateHook();
    }
}

UINT App::trackMenu(HMENU menu, int x, int y) {
    return showPopupMenu(window_, menu, {x, y}, dpi_);
}

void App::showClipMenu() {
    const auto path = clipGrid_.target();
    const auto at = clipGrid_.menuPoint();
    const auto count = clipGrid_.selectedCount();
    const bool several = count > 1; // Open, rename and the rest work on one clip only.
    enum { Open = 1, External, ShowInFolder, Rename, Copy, Favourite, Recycle, Compress, CancelCompression };
    PopupMenu styled;
    const UINT single = several ? MFS_DISABLED : MFS_ENABLED;
    styled.append(styled.root, Open, L"open", single);
    styled.append(styled.root, External, L"open in external player", single);
    styled.append(styled.root, ShowInFolder, L"show in folder", single);
    styled.append(styled.root, Rename, L"rename", !several && !playerOpen_ && clipGrid_.canRename(path) ? MFS_ENABLED : MFS_DISABLED);
    styled.append(styled.root, Copy, several ? L"copy " + std::to_wstring(count) + L" clips" : L"copy");
    const bool addFavourite = !clipGrid_.selectedAreFavourites(); // Decided once: the label and the click agree.
    styled.append(styled.root, Favourite, addFavourite ? L"add favourite" : L"remove favourite");
    if (!several && path == compression_.clip()) styled.append(styled.root, CancelCompression, L"cancel compression", compression_.cancelling() ? MFS_DISABLED : MFS_ENABLED);
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
