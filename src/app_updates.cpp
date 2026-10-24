// App: "is a newer version available". The first and only network request Refarter makes, so it is opt-in: a one-time
// question (only with Refarter's own window in front), a Settings switch and a manual button. The rules are pure
// (update_check.h) and the request runs on a worker thread (update_client.cpp). Nothing here polls or keeps a timer:
// an automatic check is one worker started at most once per start, and only while the choice is "on" and the last
// answer is more than a day old. A failed check is never a dialog: it leaves a note on the Settings row.
#include "app_internal.h"
#include "message_box.h"
#include "update_check.h"
#include "version.h"

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
    using Outcome = UpdateResult::Outcome;
    switch (result->outcome) {
    case Outcome::Newer:
        updateTag_ = result->tag;
        updateUrl_ = result->url;
        updateNote_ = L"version " + update::displayVersion(result->tag) + L" is available";
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
    RECT client{};
    GetClientRect(window_, &client);
    const RECT footer{0, client.bottom - px(footerHeight), client.right, client.bottom};
    InvalidateRect(window_, &footer, FALSE);
}

// The opt-in question. It opens only where a dialog is welcome: Refarter's own window visible and in front, nothing
// else open. Otherwise it waits for the next activation (WM_ACTIVATEAPP posts updateQuestion) and never appears over a
// game or at a hidden or tray start.
void App::askUpdateQuestion() {
    if (closing_ || startupTray_ || state_.updateCheck != UpdateChoice::Ask || asking_ || showingErrors_ || assigning_ || fullscreen_.on ||
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

std::wstring App::footerUpdateText() const {
    return updateTag_.empty() ? std::wstring{} : L"update available · v" + update::displayVersion(updateTag_);
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
