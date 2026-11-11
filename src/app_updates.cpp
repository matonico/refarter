// App: "is a newer version available". The only network request Refarter makes by itself, so it is opt-in: a one-time
// question (only with Refarter's own window in front), a Settings switch and a manual button. The rules are pure
// (update_check.h) and the request runs on a worker thread (update_client.cpp). Nothing here polls or keeps a timer:
// an automatic check is one worker started at most once per start, and only while the choice is "on" and the last
// answer is more than a day old. A failed check is never a dialog: it leaves a note on the Settings row.
// Updating itself is the second half: only when the user chooses "update" is the installer downloaded, checked and
// run (installed copies; a portable copy gets the release page).
#include "app_internal.h"
#include "message_box.h"
#include "local_file.h"
#include "update_check.h"
#include "version.h"
#include "notes_view.h"

namespace refarter {
namespace {
constexpr int updateGap = 20;                    // Between the update text and the version text, logical px.

std::int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace

// The start-up rule. Called from window creation, and again when the choice becomes "on" during a session.
void App::scheduleUpdateCheck(std::chrono::seconds delay) {
    if (closing_ || updateTried_ || updates_.running() || state_.updateCheck != UpdateChoice::On) return;
    if (!update::due(state_.lastUpdateCheck, unixNow())) return;
    startUpdateCheck(false, delay);
}

void App::startUpdateCheck(bool manual, std::chrono::seconds delay) {
    if (closing_) return;
    if (!manual) updateTried_ = true;
    updateManual_ = manual;
    if (manual) updateNote_.clear();
    updates_.start(window_, updateChecked, delay, manual, L"" REFARTER_VERSION_FULL_STRING);
    if (manual && !updates_.running()) updateNote_ = L"couldn't reach the release page"; // No thread could be started.
}

// The worker has an answer. Only an answer (newer, latest, or no release at all) counts as a check for the daily rule.
void App::updateCheckDone() {
    const auto result = updates_.take();
    if (!result || closing_) return;
    updateManual_ = false;
    // Another answer than the version being downloaded or installed: that one is no longer the update to offer.
    if (result->outcome != UpdateResult::Outcome::Failed && result->tag != updateTag_) {
        abandonUpdate();
        if (updateStage_ == UpdateStage::ByHand) updateStage_ = UpdateStage::None; // A missing checksum belonged to that release.
    }
    using Outcome = UpdateResult::Outcome;
    switch (result->outcome) {
    case Outcome::Newer:
        // The same version being downloaded, ready, installing or to be fetched by hand keeps the note that says so.
        if (result->tag != updateTag_ || !updateView().keepsNote)
            updateNote_ = L"version " + update::displayVersion(result->tag) + L" is available";
        updateTag_ = result->tag;
        updateUrl_ = result->url;
        break;
    case Outcome::Latest:
        updateTag_.clear();
        updateUrl_.clear();
        updateNote_ = L"you have the latest version";
        break;
    case Outcome::NoRelease:
        updateTag_.clear();
        updateUrl_.clear();
        updateNote_ = L"no release published yet";
        break;
    case Outcome::Failed:
        updateNote_ = L"couldn't reach the release page"; // A known newer version stays on show.
        break;
    }
    if (result->outcome != Outcome::Failed) {
        state_.lastUpdateCheck = unixNow();
        saveSettings();
    }
    refreshControls();
    refreshPages();
    invalidateFooter();
}

void App::invalidateFooter() {
    RECT client{};
    GetClientRect(window_, &client);
    const RECT footer{0, client.bottom - px(footerHeight), client.right, client.bottom};
    InvalidateRect(window_, &footer, FALSE);
}

// A dialog that held back the queued errors, the storage question or the opt-in question (dialogOpen) has closed:
// they open now if they are still due, each under its own rules. Refarter's window is usually already in front, so no
// activation would bring them.
void App::resumeHeldDialogs() {
    if (closing_) return;
    if (state_.updateCheck == UpdateChoice::Ask) PostMessageW(window_, updateQuestion, 0, 0);
    if (!queuedErrors_.empty()) PostMessageW(window_, errorDialogs, 0, 0);
    if (storageQuestionQueued_) PostMessageW(window_, storageQuestion, 0, 0);
    if (notesDue_) PostMessageW(window_, notesDue, 0, 0);
}

// The opt-in question. It opens only where a dialog is welcome: Refarter's own window visible and in front, nothing
// else open. Otherwise it waits for the next activation (WM_ACTIVATEAPP posts updateQuestion) and never appears over a
// game or at a hidden or tray start.
void App::askUpdateQuestion() {
    if (closing_ || startupTray_ || state_.updateCheck != UpdateChoice::Ask || dialogOpen() || showingErrors_ || assigning_ || fullscreen_.on ||
        quietDialogs() || !ownWindowForeground()) return;
    asking_ = true; // A nested save returns at once instead of running under the dialog.
    const int answer = choiceBox(window_,
        L"refarter can ask github once a day whether a newer version exists. only the request itself is sent "
        L"(your ip address and refarter's version as the user agent); nothing is downloaded.\n\n"
        L"you can change this later in settings.",
        L"refarter", {L"check for updates", L"don't check"});
    asking_ = false;
    if (closing_) return;
    if (answer == 0) state_.updateCheck = UpdateChoice::On;
    else if (!quietDialogs()) state_.updateCheck = UpdateChoice::Off; // "don't check", Esc or the close key.
    else return;                                                     // Nothing was shown: ask again later.
    saveSettings();
    refreshControls();
    refreshPages();
    scheduleUpdateCheck(std::chrono::seconds(0)); // Opted in just now: no reason to wait.
    showQueuedErrors();
}

void App::openUpdatePage() {
    const std::wstring url = update::safeReleaseUrl(updateUrl_) ? updateUrl_ : update::fallbackPage();
    shellOpen(url.c_str(), L"windows could not open the link in your browser.");
}

// The note on the Settings row: the last result, or what the switch says.
std::wstring App::updateRowNote() const {
    if (updates_.running() && updateManual_) return L"checking…";
    if (!updateNote_.empty()) return updateNote_;
    switch (state_.updateCheck) {
    case UpdateChoice::On: return L"asks github once a day · nothing is downloaded";
    case UpdateChoice::Off: return L"off · refarter does not contact github";
    case UpdateChoice::Ask: return L"not chosen yet · refarter asks once";
    }
    return {};
}


// ---- Updating itself ----------------------------------------------------------------------------------------------

App::UpdateView::Click App::updateClick() const {
    using Stage = UpdateStage;
    using Click = UpdateView::Click;
    const bool busy = updateStage_ == Stage::Downloading || updateStage_ == Stage::Installing; // Progress is not a link.
    const bool byHand = !installed_ || updateStage_ == Stage::ByHand; // A portable copy, or an update that can't install itself, is replaced by hand.
    return updateTag_.empty() || busy ? Click::Nothing : byHand ? Click::OpenPage : updateStage_ == Stage::Ready ? Click::Install : Click::Download;
}

// The one place that reads the stage for the footer's update text and Settings' update button.
App::UpdateView App::updateView() const {
    using Stage = UpdateStage;
    using Click = UpdateView::Click;
    const bool known = !updateTag_.empty();
    const bool busy = updateStage_ == Stage::Downloading || updateStage_ == Stage::Installing;
    const bool byHand = !installed_ || updateStage_ == Stage::ByHand;
    const bool checking = updates_.running() && updateManual_;
    UpdateView view;
    view.click = updateClick();
    // "installing…" first: setup still runs when a check or the switch has cleared the version meanwhile.
    if (updateStage_ == Stage::Installing) view.footer = L"installing update…";
    else if (!known) {} // Nothing to show.
    else if (updateStage_ == Stage::Downloading)
        view.footer = downloadPercent_ < 0 ? L"downloading update…" : L"downloading update · " + std::to_wstring(downloadPercent_) + L"%";
    else view.footer = (updateStage_ == Stage::Ready ? L"restart to update · v" : L"update available · v") + update::displayVersion(updateTag_);
    view.button = updateStage_ == Stage::Installing ? L"installing…" : checking ? L"checking…" : !known ? L"check for updates" :
        byHand ? L"open release page" : updateStage_ == Stage::Downloading ? L"downloading…" :
        updateStage_ == Stage::Ready ? L"restart to update" : L"update now";
    view.buttonEnabled = !closing_ && !checking && !busy;
    view.primary = !checking && view.click != Click::Nothing; // While the label is an action on the newer version.
    view.keepsNote = updateStage_ != Stage::None;
    return view;
}

void App::updateClicked() {
    if (closing_) return;
    switch (updateClick()) {
    case UpdateView::Click::OpenPage: openUpdatePage(); break;
    case UpdateView::Click::Install: installUpdate(false); break;
    case UpdateView::Click::Download: askToUpdate(); break;
    case UpdateView::Click::Nothing: break;
    }
}

void App::cancelDownload() {
    download_.cancel();
    if (updateStage_ == UpdateStage::Downloading) updateStage_ = UpdateStage::None;
}

// Nothing is downloaded until the user says yes here.
void App::askToUpdate() {
    if (asking_ || quietDialogs()) return;
    const std::wstring tag = updateTag_; // The version the question names.
    const auto version = update::displayVersion(tag);
    const bool replay = recording_ != capture::RecorderState::Off;
    std::wstring text = L"update refarter to version " + version + L"? it downloads the update from github, closes, updates and opens again.";
    if (replay) text += L"\n\nthe replay that has not been saved yet is cleared.";
    // Whatever way this ends, the errors and questions that waited for its dialogs are shown afterwards.
    struct Resume { App& app; ~Resume() { app.resumeHeldDialogs(); } } resume{*this};
    asking_ = true; // A nested save returns at once instead of running under the dialog.
    const int answer = choiceBox(window_, text.c_str(), L"update refarter", {L"yes", L"no"});
    asking_ = false;
    if (closing_ || answer != 0) return;
    // A check that answered under the question: its note says what is known now, and "update" meant the version named.
    if (updateTag_ != tag) return;
    replayAgreed_ = replay;
    downloadPercent_ = -1;
    updateNote_ = L"downloading version " + version + L"…";
    download_.start(window_, updateDownloaded, updateProgress, updateTag_, L"Refarter/" REFARTER_VERSION_FULL_STRING, localAppDataFile(L"updates"));
    if (download_.running()) updateStage_ = UpdateStage::Downloading; // Installs as soon as the download is checked.
    else updateNote_ = L"the update couldn't be downloaded · try again"; // No thread could be started.
    refreshControls();
    refreshPages();
    if (updateStage_ != UpdateStage::Downloading) return;
    // The download's progress, over Refarter's window, until it ends (downloadDone closes it) or the user cancels.
    // Saves keep working under it: it is not a question (asking_ stays false).
    const std::wstring progress = L"downloading version " + version + L"…";
    const bool finished = progressBox(window_, progress.c_str(), L"update refarter", L"cancel", &downloadBox_);
    if (closing_) return;
    if (!finished) {
        if (updateStage_ != UpdateStage::Downloading) return; // It ended as the box closed: downloadDone has its result.
        cancelDownload();
        updateNote_ = L"the update was cancelled";
        refreshControls();
        refreshPages();
        return;
    }
    // downloadDone ran under the box and left the installer (Ready), or a note saying why there is none. The box gives
    // Refarter the focus back only if the user stayed, so a user now in a game gets neither setup (installUpdate
    // waits, "restart to update") nor a failure dialog: the Settings note says why.
    if (updateStage_ == UpdateStage::Ready) { installUpdate(true); return; }
    if (updateTag_ == tag && !quietDialogs() && ownWindowForeground()) // A failure; not a check that replaced the version.
        messageBox(window_, updateNote_.c_str(), L"update refarter", MB_OK | MB_ICONERROR);
}

void App::downloadDone() {
    const auto result = download_.take();
    if (!result) return;
    if (updateStage_ == UpdateStage::Downloading) updateStage_ = UpdateStage::None; // Taken: download_ no longer runs.
    if (closing_) return;
    if (result->tag != updateTag_) return; // A later check found another version meanwhile.
    using Outcome = DownloadResult::Outcome;
    switch (result->outcome) {
    case Outcome::Ready:
        updateSetup_ = result->setup;
        updateLock_ = result->lock;
        updateStage_ = UpdateStage::Ready;
        updateNote_ = L"version " + update::displayVersion(updateTag_) + L" is ready · restart to update";
        break;
    case Outcome::Network: updateNote_ = L"the update couldn't be downloaded · try again"; break;
    case Outcome::NoChecksum: // The button and the footer open the release page from now on.
        updateStage_ = UpdateStage::ByHand;
        updateNote_ = L"this release has no checksum to check the installer with · open the release page";
        break;
    case Outcome::Mismatch: updateNote_ = L"the download didn't match the release's checksum · nothing was installed"; break;
    case Outcome::Disk: updateNote_ = L"the update couldn't be saved on this pc · nothing was installed"; break;
    }
    refreshControls();
    refreshPages();
    invalidateFooter();
    // Under the progress box, the window is disabled: askToUpdate installs once the box has closed.
    if (downloadBox_) { SendMessageW(downloadBox_, progressDone, 0, 0); return; }
    if (result->outcome == Outcome::Ready) installUpdate(true); // Only askToUpdate starts a download: the user chose "update".
}

// The known version is no longer the one to install: its download stops (and its dialog closes; askToUpdate then sees
// no installer) and a checked installer is let go. Installing and ByHand stay.
void App::abandonUpdate() {
    cancelDownload();
    if (downloadBox_) SendMessageW(downloadBox_, progressDone, 0, 0);
    dropSetup();
}

void App::dropSetup() {
    updateSetup_.clear();
    updateLock_.reset();
    if (updateStage_ == UpdateStage::Ready) updateStage_ = UpdateStage::None;
}

// Runs the checked installer without questions but with its progress window (/SILENT); setup closes Refarter (--quit), installs and opens it again
// (/RELAUNCH=1). Never while something is being written, and never by itself while a game is in front: the new
// window would open over it. A replay the user was not warned about is asked for first.
void App::installUpdate(bool chosen) {
    if (closing_ || updateStage_ != UpdateStage::Ready || quietDialogs()) return; // Not during a shutdown or sleep wait.
    // `tell`: the user just chose to update, so a dialog says why nothing happens.
    const auto waitsForWork = [&](bool tell) {
        if (!saving_ && !shooting_ && !compression_.active() && !player_.trimBusy() && !player_.frameSaveBusy()) return false;
        updateNote_ = L"the update waits until the save, trim, frame or compression is done · then choose restart to update";
        refreshPages();
        if (tell) report(L"the update waits until the save, trim, frame or compression is done. then choose \"restart to update\" again.", MB_ICONINFORMATION);
        return true;
    };
    if (waitsForWork(!chosen)) return;
    // Setup closes Refarter, so not by itself under any open dialog (a dialog disables the window, whichever asked it),
    // a shortcut being assigned or a clip in fullscreen. The footer says "restart to update" until the user is back.
    if (chosen && (!ownWindowForeground() || !IsWindowEnabled(window_) || asking_ || showingErrors_ || assigning_ || fullscreen_.on)) return;
    if (recording_ != capture::RecorderState::Off && !(chosen && replayAgreed_)) {
        if (asking_ || quietDialogs()) return;
        asking_ = true;
        const int answer = choiceBox(window_, L"updating closes refarter and clears the replay that has not been saved yet.",
            L"update refarter", {L"update now", L"not now"});
        asking_ = false;
        resumeHeldDialogs();
        // A check that answered under the question may have dropped the setup; a shutdown or sleep may have begun.
        if (answer != 0 || closing_ || updateStage_ != UpdateStage::Ready || quietDialogs()) return;
        if (waitsForWork(true)) return; // A save, screenshot, trim or compression started under the question.
    }
    // No new save, screenshot, trim, compression or frame export from here until setup closes Refarter (setup ends a
    // Refarter still writing after 30 s). The hold starts before setup exists, then the work is checked once more:
    // a frame the player had already read back may have reached its writer meanwhile.
    const auto release = [&](UpdateStage stage, const std::wstring& note) {
        updateStage_ = stage;
        player_.setWritesHeld(false);
        updateNote_ = note;
        refreshControls();
        refreshPages();
    };
    updateStage_ = UpdateStage::Installing; // updateSetup_ stays until setup has started from it.
    player_.setWritesHeld(true, updatingNote);
    if (player_.frameSaveBusy()) { release(UpdateStage::Ready, updateNote_); waitsForWork(!chosen); return; } // Released first: the dialog holds nothing.
    // The installer has been held open read-only since its check (updateLock_), so it is still the file the release
    // lists. Setup closes Refarter itself with --quit (as the tray's quit does: settings are kept) once it is ready to
    // install, so a setup that fails to start or stops early leaves Refarter running. It starts suspended and runs only
    // once the wait for its end is in place, so the hold above can always be lifted again if setup ends without
    // closing Refarter.
    const std::wstring notStarted = L"the installer couldn't be started · try again";
    std::wstring command = L"\"" + updateSetup_.wstring() + L"\" /SILENT /SUPPRESSMSGBOXES /NORESTART /RELAUNCH=1";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(updateSetup_.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr,
                                        &startup, &process);
    const DWORD startError = started ? ERROR_SUCCESS : GetLastError();
    dropSetup(); // A running image cannot be changed; a later try checks the file again. Installing stays.
    // An installation that needs administrator rights (installed for all users) cannot update itself; retrying never helps.
    // ByHand: the button and the footer open the release page from now on.
    if (startError == ERROR_ELEVATION_REQUIRED) {
        release(UpdateStage::ByHand, L"this installation needs administrator rights to update · open the release page");
        return;
    }
    if (!started) { release(UpdateStage::None, notStarted); return; }
    const auto ended = [](void* window, BOOLEAN) { PostMessageW(static_cast<HWND>(window), updateSetupEnded, 0, 0); };
    if (!RegisterWaitForSingleObject(&setupWait_, process.hProcess, ended, window_, INFINITE, WT_EXECUTEONLYONCE)) {
        // Nothing could tell when setup ends, so it never runs: Refarter stays as it was.
        setupWait_ = nullptr;
        TerminateProcess(process.hProcess, ERROR_CANCELLED);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        release(UpdateStage::None, notStarted);
        return;
    }
    setupProcess_ = process.hProcess;
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) TerminateProcess(process.hProcess, ERROR_CANCELLED); // The wait then lifts the hold.
    CloseHandle(process.hThread);
    updateNote_ = L"installing version " + update::displayVersion(updateTag_) + L"…";
    refreshControls();
    refreshPages();
}

void App::setupEnded() {
    if (setupWait_) UnregisterWaitEx(std::exchange(setupWait_, nullptr), INVALID_HANDLE_VALUE); // Its callback has posted already.
    if (setupProcess_) CloseHandle(std::exchange(setupProcess_, nullptr));
    if (!installing() || closing_) return;
    updateStage_ = UpdateStage::None; // Its installer was let go when it started.
    player_.setWritesHeld(false);
    updateNote_ = L"the update didn't finish · try again";
    refreshControls();
    refreshPages();
}

// ---- The footer's "update available" text ------------------------------------------------------------------------

int App::smallTextWidth(const std::wstring& text) const {
    return MulDiv(smallTextExtent(text), 96, static_cast<int>(dpi_));
}

// Measured once per text: the footer's hit test asks on every mouse move over it, and a text that changes (the status,
// the update tag) is simply a new entry. createFonts empties the list when the font or the DPI changes.
int App::smallTextExtent(const std::wstring& text) const {
    for (const auto& [known, width] : textExtents_) if (known == text) return width;
    const HDC dc = GetDC(window_);
    const auto previous = SelectObject(dc, fontOf(Font::Small));
    SIZE extent{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent);
    SelectObject(dc, previous);
    ReleaseDC(window_, dc);
    if (textExtents_.size() >= 8) textExtents_.clear(); // A handful of texts are live at a time; never grows.
    textExtents_.emplace_back(text, static_cast<int>(extent.cx));
    return static_cast<int>(extent.cx);
}

std::wstring App::footerVersion() const {
    return std::wstring(L"refarter v" REFARTER_VERSION_STRING) +
        (*REFARTER_VERSION_SUFFIX_STRING ? L" · " REFARTER_VERSION_SUFFIX_STRING : L"");
}

// The version text ends 16 px before the window's right edge (see paint); a click on it opens what's new.
RECT App::footerVersionBounds() const {
    RECT client{};
    GetClientRect(window_, &client);
    const int width = MulDiv(client.right, 96, static_cast<int>(dpi_));
    return {px(width - 16 - smallTextWidth(footerVersion()) - 2), client.bottom - px(footerHeight), px(width - 16), client.bottom};
}

// ---- What's new ---------------------------------------------------------------------------------------------------

// A popup over Refarter's window: the notes take at most 600 × 560 px (less on a small screen) and scroll inside it.
void App::showNotes() {
    if (closing_ || dialogOpen() || quietDialogs()) return;
    notesDue_ = false;
    if (seenVersion_ != L"" REFARTER_VERSION_FULL_STRING && !embeddedNotes().empty()) {
        seenVersion_ = L"" REFARTER_VERSION_FULL_STRING;
        saveSettings();
    }
    // Other questions wait, but a save still runs: the popup opens by itself and may stay open behind a game.
    infoOpen_ = true;
    NotesView view;
    contentBox(window_, L"what's new in refarter v" REFARTER_VERSION_FULL_STRING, 600, 560, [&](HWND parent, RECT area) {
        const HWND notes = view.create(parent, contentBoxBodyId);
        if (!notes) return notes;
        view.setBackground(surface);
        if (!view.setStyle(GetDpiForWindow(parent))) { // Without fonts the body would be blank: the dialog shows plain text.
            DestroyWindow(notes);
            return static_cast<HWND>(nullptr);
        }
        view.setNotes(embeddedNotes());
        SetWindowPos(notes, nullptr, area.left, area.top, area.right - area.left, area.bottom - area.top, SWP_NOZORDER | SWP_SHOWWINDOW);
        return notes;
    }, L"got it", embeddedNotes().empty() ? L"this build has no release notes." : notes::plainText(embeddedNotes()));
    infoOpen_ = false;
    resumeHeldDialogs(); // A save under the popup may have queued an error or the storage question.
}

// The first time Refarter's own window is in front after an update (from a hidden sign-in start, the first restore),
// with nothing else open; never over a game. Otherwise it stays due for the next activation.
void App::showNotesIfDue() {
    if (!notesDue_ || closing_ || startupTray_ || dialogOpen() || showingErrors_ || assigning_ || fullscreen_.on ||
        quietDialogs() || !ownWindowForeground()) return;
    showNotes();
}

std::wstring App::footerUpdateText() const {
    return updateView().footer;
}

// Logical pixels the footer's right end takes: the version text's room, plus the update text and its gap.
int App::footerReserve() const {
    const auto text = footerUpdateText();
    return footerVersionReserve + (text.empty() ? 0 : smallTextWidth(text) + updateGap);
}

// The update text sits just left of the version text; empty (all zero) when there is no update to show.
RECT App::footerUpdateBounds() const {
    const auto text = footerUpdateText();
    if (text.empty()) return {};
    RECT client{};
    GetClientRect(window_, &client);
    const int width = MulDiv(client.right, 96, static_cast<int>(dpi_));
    const int right = width - 16 - smallTextWidth(footerVersion()) - updateGap;
    return {px(right - smallTextWidth(text) - 2), client.bottom - px(footerHeight), px(right), client.bottom};
}

} // namespace refarter
