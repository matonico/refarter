// App: the clip folder's storage limit and low-disk rules. The decisions are pure (storage_limit.h); this file
// measures the folder and the drive, asks the first-time question and acts on the answer. Nothing here polls: the
// drive is read when the library changes and before a save, never while painting.
#include "app_internal.h"
#include "message_box.h"
#include "popup_menu.h"
#include "text.h"

namespace refarter {

bool App::gameTrackOn() const {
    return target_.source.window && audio_.desktop.enabled();
}

std::uint64_t App::estimatedClipBytes(int seconds) const {
    const auto rate = capture::bitrateFor(video_, capture::outputSize(video_.resolution, sourceSize()));
    // Discord gets its own track unless hidden or inseparable (only clips where it spoke).
    const bool discord = audio_.desktop.enabled() && !audio_.hideDiscord && !discordUnavailable_ && !discordSharesOutput_;
    const int sources = audio_.desktop.enabled() + audio_.microphone.enabled() + discord + gameTrackOn();
    const int tracks = sources >= 2 ? sources + 1 : sources; // Two or more: a mix first.
    return capture::replayBytes(rate.averageKbps + tracks * 192, seconds < 0 ? state_.replaySeconds : seconds);
}

// A save writes what the buffer holds: shorter than the setting just after a start. The time since the buffer began
// filling bounds it from above (it also counts a sleep pause, so the estimate errs on the large side).
int App::bufferedClipSeconds() const {
    if (recording_ == capture::RecorderState::Off || bufferingSince_ == std::chrono::steady_clock::time_point{}) return state_.replaySeconds;
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - bufferingSince_).count();
    return estimateSeconds(state_.replaySeconds, elapsed);
}

StripState App::storageStrip() const {
    return stripState(clipGrid_.bytes(), limitBytes(state_.maxStorageGb), clipGrid_.driveFree(), estimatedClipBytes(bufferedClipSeconds()));
}

void App::invalidateStrip() {
    RECT client{};
    GetClientRect(window_, &client);
    RECT strip{px(contentLeft()), client.bottom - px(footerHeight + 40), client.right, client.bottom - px(footerHeight + 4)};
    InvalidateRect(window_, &strip, FALSE);
}

void App::updateStorageInput() {
    if (!control(StorageLimitField)) return; // EDIT can notify while it is still being created.
    wchar_t text[8]{};
    GetWindowTextW(control(StorageLimitField), text, static_cast<int>(std::size(text)));
    const auto value = text[0] ? parseNumber(text, 0, static_cast<int>(maxStorageGb)) : std::optional<int>(0);
    if (value) state_.maxStorageGb = static_cast<unsigned>(*value); // Written to the file once the field is left.
    invalidateStrip();
    refreshPages(); // The limit row's note depends on whether a limit is set.
}

void App::chooseStorageBehaviour() {
    PopupMenu styled;
    const auto menu = styled.root;
    const wchar_t* items[] = {L"warn and save · save anyway and say so", L"refuse the save · keep the replay, save nothing",
                              L"bin oldest clips · skips favourites and compressed copies"};
    for (int i = 0; i < 3; ++i)
        styled.append(menu, i + 1, items[i], MFS_ENABLED | (static_cast<int>(state_.storageFull) == i ? MFS_CHECKED : 0));
    RECT rect{};
    GetWindowRect(control(StorageBehaviour), &rect);
    if (const auto selected = trackMenu(menu, rect.left, rect.bottom); selected >= 1 && selected <= 3) {
        state_.storageFull = static_cast<StorageFull>(selected - 1);
        state_.storageFullChosen = true;
    }
}

bool App::ownWindowForeground() const {
    const HWND front = GetForegroundWindow();
    return front && IsWindowVisible(window_) && !IsIconic(window_) && GetAncestor(front, GA_ROOTOWNER) == window_;
}

// The first-time question. Only called where it may open (see questionTiming); the answer applies to the save that
// asked and, when it is a real choice, to every later one.
void App::askStorageQuestion(std::uint64_t used, std::uint64_t limit, std::uint64_t estimate, bool late) {
    storageAsked_ = true;
    storageQuestionQueued_ = false;
    asking_ = true; // A nested save (a second press, the tray) returns at once instead of running under the dialog.
    // Asked on activation after a save already went ahead, the folder has passed the limit; otherwise this save would.
    const auto text = (late ? L"the clip folder has passed its storage limit (" + formatUsage(used, limit) + L") · your last save went ahead"
                            : L"the clip folder is about to pass its storage limit (" + formatUsage(used + estimate, limit) + L")") +
        L". what should a save do when storage is full?\n\n"
        L"warn and save: save anyway and say so.\n"
        L"refuse: save nothing; the replay is kept.\n"
        L"bin oldest: move the oldest clips, but never favourites or compressed copies, to the recycle bin (local drive with one only).\n\n"
        L"you can change this later in settings.";
    const auto answer = resolveAnswer(choiceBox(window_, text.c_str(), L"refarter", {L"warn and save", L"refuse", L"bin oldest", L"not now"}));
    asking_ = false;
    state_.storageFull = answer.behaviour; // Not now acts as warn and is not remembered.
    if (answer.persist) {
        state_.storageFullChosen = true;
        saveSettings();
        refreshPages();
    }
    showQueuedErrors();
}

// Refarter was activated (the player clicked its window after the game): the question a save over the game could not
// ask. Posted from WM_ACTIVATEAPP; never during a shutdown or sleep wait.
void App::askQueuedStorageQuestion() {
    if (!storageQuestionQueued_ || asking_ || showingErrors_ || quietDialogs() || !ownWindowForeground()) return;
    const std::uint64_t limit = limitBytes(state_.maxStorageGb);
    const std::uint64_t estimate = estimatedClipBytes(bufferedClipSeconds());
    const auto verdict = checkSave({limit ? clipGrid_.bytes() : 0, limit, clipGrid_.driveFree(), estimate});
    if (state_.storageFullChosen || storageAsked_ || verdict != StorageVerdict::OverLimit) { storageQuestionQueued_ = false; return; }
    askStorageQuestion(clipGrid_.bytes(), limit, estimate, true);
}

// Runs before every save from the button, the shortcut or the tray (auto-capture only starts and stops replay; it
// never saves). The save always starts first, as "warn"; the first-time question follows it (saveReplay). It reads no disk: the folder size and free space are the library worker's last scan, and the worker
// does the binning. It never opens a dialog during a bounded wait, while a game is in front, or while the window is
// hidden: the save goes ahead as "warn" and the question waits for Refarter's own window. A clip writer failure is
// handled separately, by the writer, which deletes its partial file.
bool App::storageAllowsSave(std::wstring& note) {
    if (!clipGrid_.scanned()) {
        clipGrid_.scan(clipFolder_); // Nothing is known yet (a hidden start): this save goes ahead, the next is checked.
        return true;
    }
    const std::uint64_t limit = limitBytes(state_.maxStorageGb);
    const std::uint64_t used = limit ? clipGrid_.bytes() : 0;
    const std::uint64_t free = clipGrid_.driveFree();
    // At most what the buffer holds, not always the full replay length.
    const std::uint64_t estimate = estimatedClipBytes(bufferedClipSeconds());
    const auto verdict = checkSave({used, limit, free, estimate});
    if (verdict == StorageVerdict::Ok) return true;

    switch (questionTiming(verdict, state_.storageFullChosen, storageAsked_, storageQuestionQueued_, ownWindowForeground(), quietDialogs())) {
    case Question::Now: // Never before the save: the wanted moment must not fall out of the buffer. saveReplay asks afterwards.
        storageQuestionQueued_ = askAfterSave_ = true;
        break;
    case Question::Later: storageQuestionQueued_ = true; break;
    case Question::No: break;
    }

    const bool binAllowed = clipGrid_.binAllowed();
    const auto action = actionFor(verdict, state_.storageFull, binAllowed);
    if (action == StorageAction::Refuse) {
        setStatus(storageStatus(verdict, action));
        clipGrid_.say(storageStatus(verdict, action));
        refreshControls();
        invalidateStrip();
        InvalidateRect(window_, nullptr, FALSE);
        return false;
    }
    note = storageStatus(verdict, action);
    if (state_.storageFull == StorageFull::Bin && verdict == StorageVerdict::OverLimit && !binAllowed) note = binUnavailableStatus();
    // The worker reads the folder afresh, plans and moves; the save does not wait for it.
    if (action == StorageAction::BinThenSave && clipGrid_.binOldest(limit, estimate)) note = binningStatus();
    return true;
}

} // namespace refarter
