// App: opening clips. The built-in player and its callbacks, fullscreen, external players, and compression.
#include "app_internal.h"
#include "clip_selection.h"
#include "trim_follow_up.h"
#include "message_box.h"
#include "popup_menu.h"
#include "text.h"
#include "player/export_plan.h"

#include <shellapi.h>
#include <shlobj.h>

namespace refarter {

player::ClipPlayer::Callbacks App::playerCallbacks() {
    player::ClipPlayer::Callbacks callbacks;
    callbacks.back = [this] { closePlayer(); };
    callbacks.fullscreen = [this](bool on) { setFullscreen(on); };
    callbacks.external = [this](const std::filesystem::path& path) { openExternally(path); };
    callbacks.folder = [this](const std::filesystem::path& path) { showInFolder(path); };
    callbacks.saved = [this](const std::filesystem::path& clip, const std::filesystem::path& source) {
        clipGrid_.inheritSource(clip, source); // A trim of a renamed clip stays in its folder.
        if (playerOpen_) clipGrid_.setOpenClip(player_.clipPath()); // The player may now show the new clip.
        clipGrid_.scan(clipFolder_);
    };
    callbacks.rename = [this](const std::filesystem::path& clip, const std::wstring& name, std::filesystem::path& renamed) {
        auto problem = clipGrid_.renameClip(clip, name, renamed);
        if (!renamed.empty()) clipGrid_.setOpenClip(renamed);
        return problem;
    };
    callbacks.trimEnded = [this] { if (!playerOpen_) clipGrid_.setOpenClip({}); };
    callbacks.trimFailed = [this](const std::wstring& reason) { clipGrid_.say(L"the trim could not be saved · " + reason); };
    callbacks.trimSaved = [this](const player::ClipPlayer::SavedTrim& trim) { trimSaved(trim); };
    callbacks.cancelTrimCompression = [this] {
        if (trimFollowUp_.pending()) { compression_.cancel(); trimFollowUp_.cancel(); }
    };
    callbacks.replacing = [this](const std::filesystem::path& path, const std::filesystem::path& replacement) { clipGrid_.keepFavourite(path, replacement); };
    callbacks.volume = [this](int volume) { playerVolume_ = volume; saveSettings(); };
    callbacks.choose = [this](const std::vector<std::wstring>& items, const std::vector<bool>& checked, POINT at) {
        PopupMenu styled;
        for (size_t i = 0; i < items.size(); ++i)
            styled.append(styled.root, static_cast<UINT>(i + 1), items[i], i < checked.size() && checked[i] ? MFS_CHECKED : MFS_ENABLED);
        return static_cast<int>(trackMenu(styled.root, at.x, at.y)) - 1;
    };
    return callbacks;
}

void App::openClip(const std::filesystem::path& path) {
    if (path == compression_.clip()) {
        messageBox(window_, L"finish or cancel compression before editing this clip.", L"refarter", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        messageBox(window_, L"this clip was moved or deleted.", L"refarter", MB_OK | MB_ICONERROR);
        clipGrid_.scan(clipFolder_);
        return;
    }
    // The built-in player reads MP4 and MOV; other formats go to the default player.
    const auto extension = lower(path.extension().wstring());
    if (extension != L".mp4" && extension != L".m4v" && extension != L".mov") {
        openExternally(path);
        return;
    }
    if (state_.page != Page::Library) navigate(Page::Library);
    playerOpen_ = true;
    clipGrid_.setOpenClip(path);
    player_.open(path, playerVolume_, clipGrid_.dateOf(path));
    layout();
    SetFocus(control(Player));
    InvalidateRect(window_, nullptr, FALSE);
}

bool App::shellOpen(const wchar_t* target, const wchar_t* failure, UINT icon) {
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", target, nullptr, nullptr, SW_SHOWNORMAL)) > 32) return true;
    if (failure) messageBox(window_, failure, L"refarter", MB_OK | icon);
    return false;
}

bool App::openExternally(const std::filesystem::path& path) {
    // The user's default player for this file type, exactly as Explorer opens it.
    if (shellOpen(path.c_str())) return true;
    std::error_code error;
    messageBox(window_, std::filesystem::exists(path, error) ? L"windows could not open this clip. check that a video player is installed." :
        L"this clip was moved or deleted.", L"refarter", MB_OK | MB_ICONERROR);
    clipGrid_.scan(clipFolder_);
    return false;
}

void App::showInFolder(const std::filesystem::path& path) {
    HRESULT result = E_FAIL;
    if (auto* item = ILCreateFromPathW(path.c_str())) {
        result = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        ILFree(item);
    }
    if (FAILED(result))
        messageBox(window_, L"windows could not show this clip in its folder. it may have been moved or deleted.",
                    L"refarter", MB_OK | MB_ICONERROR);
}

void App::closePlayer() {
    if (!playerOpen_) return;
    if (fullscreen_.on) setFullscreen(false);
    player_.close();
    playerOpen_ = false;
    clipGrid_.setOpenClip(player_.trimSource()); // A trim still being written keeps its source from the bin.
    layout();
    SetFocus(clipGrid_.count() ? control(Clips) : control(Library));
    clipGrid_.scan(clipFolder_); // Shows clips trimmed in the player.
    InvalidateRect(window_, nullptr, FALSE);
}

void App::startCompression(const std::filesystem::path& path, bool discardIntermediate) {
    if (closing_ || compression_.active() || path.empty()) return;
    if (!SetTimer(window_, exportTimer, 250, nullptr)) {
        messageBox(window_, L"could not start compression.", L"refarter", MB_OK | MB_ICONERROR);
        return;
    }
    clipGrid_.compression(path, 0, false);
    if (!compression_.start(path, discardIntermediate)) {
        KillTimer(window_, exportTimer);
        clipGrid_.compression({}, 0, false);
        messageBox(window_, L"could not start the compression worker.", L"refarter", MB_OK | MB_ICONERROR);
        return;
    }
    player_.setCompressionBusy(true, path); // The trim bar's "compress & copy" waits for this job.
}

// A trim was saved: when the player's box was ticked, compress the resulting clip and copy the outcome.
void App::trimSaved(const player::ClipPlayer::SavedTrim& trim) {
    const auto& clip = trim.clip;
    const bool compressCopy = trim.compressCopy;
    const auto session = trim.session;
    if (quietDialogs()) { player_.finishTrimCompression({}, false, session); return; }
    if (afterTrim(compressCopy, !clip.empty() && !closing_, compression_.active()) != AfterTrim::Compress) {
        const bool busy = compressCopy && compression_.active();
        const bool originOpen = playerOpen_ && player_.session() == session;
        // The trim is kept: open it as a normal trim does, then say why it was not compressed.
        if (compressCopy) player_.finishTrimCompression(busy && !clip.empty() && !closing_ ? clip : std::filesystem::path{}, false, session);
        if (busy) {
            if (originOpen) player_.showToast(compressionBusyHint, player::ToastKind::Notice, false);
            clipGrid_.say(compressionBusyHint);
        }
        return;
    }
    trimFollowUp_.arm(clip, trim.source, trim.replace);
    clipGrid_.setTrimSource(trim.source);
    trimPlayerSession_ = session;
    startCompression(clip, true);
    if (!compression_.active() || compression_.clip() != clip) {
        trimFollowUp_.drop(); // Could not start; startCompression has said so.
        clipGrid_.setTrimSource({});
        player_.finishTrimCompression({}, false, session);
        trimPlayerSession_ = 0;
        return;
    }
    progressNote_ = compressingNote(0);
    if (playerOpen_ && player_.session() == session) player_.showToast(progressNote_, player::ToastKind::Progress, true);
}

void App::receiveCompression() {
    if (!compression_.active()) return;
    const auto clip = compression_.clip();
    clipGrid_.compression(clip, compression_.progress(), compression_.cancelling());
    const bool followUp = trimFollowUp_.pending() && trimFollowUp_.clip() == clip;
    const bool originOpen = playerOpen_ && trimPlayerSession_ != 0 && player_.session() == trimPlayerSession_;
    if (followUp && !quietDialogs() && !compression_.cancelling()) {
        if (originOpen) player_.trimCompressionProgress(compression_.progress(), trimPlayerSession_);
        // Only the progress text itself, and never over another toast; the strip note carries it otherwise.
        const auto note = compressingNote(compression_.progress());
        const bool free = player_.toastText().empty() || player_.toastText() == progressNote_;
        if (note != progressNote_) {
            if (originOpen && free) player_.showToast(note, player::ToastKind::Progress, true);
            progressNote_ = note;
            clipGrid_.say(note);
        }
    }
    const auto result = compression_.poll();
    if (!result) return;
    KillTimer(window_, exportTimer);
    clipGrid_.compression({}, 0, false);
    player_.setCompressionBusy(false);
    if (!result->path.empty()) {
        clipGrid_.inheritSource(result->path, followUp ? trimFollowUp_.source() : clip);
        clipGrid_.scan(clipFolder_);
    }
    if (closing_) {
        player_.finishTrimCompression({}, false, trimPlayerSession_);
        clipGrid_.setTrimSource({});
        trimFollowUp_.drop();
        trimPlayerSession_ = 0;
        if (recorderClosed_) DestroyWindow(window_);
        return;
    }
    if (followUp) {
        if (quietDialogs()) {
            // Keep the finished file, but consume the automatic copy/navigation. Never replay it after waking.
            player_.finishTrimCompression({}, false, trimPlayerSession_);
            clipGrid_.setTrimSource({});
            trimFollowUp_.drop();
            trimPlayerSession_ = 0;
            if (originOpen && player_.toastText() == progressNote_) player_.showToast(L"", player::ToastKind::Done, false);
            return;
        }
        // The copy happens only for a finished file: never after a failure or a cancel.
        const bool replace = trimFollowUp_.replacesSource();
        const auto plan = trimFollowUp_.finish(clip, *result);
        std::wstring note = plan.note;
        std::filesystem::path preview;
        if (plan.action == AfterCompression::CopyFile) {
            std::error_code error;
            if (!std::filesystem::exists(plan.file, error)) note = copyMissingNote;
            else preview = plan.file;
        }
        clipGrid_.setTrimSource({});
        // A trim that already fits is the result itself: it takes the original's name before it is copied.
        std::wstring replacementWarning;
        bool finished = false;
        if (replace && result->alreadyFits && !preview.empty()) {
            replacementWarning = player_.finishTrimCompression(preview, true, trimPlayerSession_, true, &preview);
            finished = true;
        }
        bool copied = false;
        if (!preview.empty()) {
            copied = copyFilesToClipboard(window_, {preview});
            note = copied ? copiedToClipboardNote : copyFailedNote;
        }
        if (!finished) replacementWarning = player_.finishTrimCompression(preview, replace, trimPlayerSession_);
        if (replace && originOpen && !preview.empty() && replacementWarning.empty()) note += L" · the original is in the recycle bin";
        if (!replacementWarning.empty()) note += L" · " + replacementWarning;
        if (!result->cleanupWarning.empty()) note += L" · " + result->cleanupWarning;
        if (replace && !originOpen && !preview.empty()) note += L" · original kept because the player changed";
        trimPlayerSession_ = 0;
        if (originOpen) player_.showToast(note, plan.action == AfterCompression::CopyFile ?
            (copied && replacementWarning.empty() && result->cleanupWarning.empty() ? player::ToastKind::Done :
                preview.empty() || !copied ? player::ToastKind::Error : player::ToastKind::Partial) : plan.kind, false);
        clipGrid_.say(note);
        return;
    }
    if (!result->error.empty()) report(lower(result->error), MB_ICONERROR);
    else if (result->alreadyFits) report(L"this clip already fits under " + formatClipBytes(player::exportLimit) + L".", MB_ICONINFORMATION);
}

void App::setFullscreen(bool on) {
    if (on == fullscreen_.on || !playerOpen_) return;
    if (on) {
        // Borderless over the whole monitor the window is on; the taskbar stays below.
        fullscreen_.style = GetWindowLongPtrW(window_, GWL_STYLE);
        GetWindowPlacement(window_, &fullscreen_.placement);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
        fullscreen_.on = true;
        SetWindowLongPtrW(window_, GWL_STYLE, (fullscreen_.style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        const auto& r = monitor.rcMonitor;
        SetWindowPos(window_, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        fullscreen_.on = false;
        SetWindowLongPtrW(window_, GWL_STYLE, fullscreen_.style);
        SetWindowPlacement(window_, &fullscreen_.placement);
        SetWindowPos(window_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    player_.setFullscreen(on);
    layout();
    SetFocus(control(Player));
}

} // namespace refarter
