#pragma once

#include "background_task.h"

#include <windows.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace refarter {

// How a download ended. Ready carries the checked setup; the rest say what the Settings note should.
struct DownloadResult {
    // NoChecksum: the release has no SHA256SUMS.txt, or it does not list this setup, so nothing can be checked.
    enum class Outcome { Ready, Network, NoChecksum, Mismatch, Disk } outcome = Outcome::Network;
    std::filesystem::path setup; // Ready only: the installer, its SHA-256 matching the release's SHA256SUMS.txt.
    // Ready only: the installer, open for reading and shared for reading only, so nothing can change, replace or
    // delete it between its check and the moment setup runs. Closed with the last copy.
    std::shared_ptr<void> lock;
    std::wstring tag;
};

// Downloads one release's installer from GitHub over WinHTTP, on a short-lived worker thread at background priority,
// only when the user asked to update (never by itself). It reads SHA256SUMS.txt from the same release, streams the
// setup into a `.partial` file of its own and checks it, renames it, then opens it locked (see DownloadResult::lock) and
// hashes it again through that handle; an installer already in the folder is checked the same way and used as it is
// (or removed when it differs). Other files in the folder (older installers, broken downloads) are removed first. The
// hash proves the file arrived whole, not who made it: SHA256SUMS.txt comes from the same release (see decisions.md).
// Like UpdateClient, the worker owns shared state only (BackgroundTask), so cancelling or destroying this never waits
// for the network; a cancelled worker deletes its partial file and posts nothing more.
class UpdateDownload {
public:
    ~UpdateDownload() { cancel(); }
    // `done` is posted once with the result (take()); `progress` (wParam: percent, lParam: the download's id, see
    // current()) whenever the whole percent changes.
    void start(HWND window, UINT done, UINT progress, std::wstring tag, std::wstring userAgent, std::filesystem::path folder);
    void cancel() { task_.cancel(); }
    bool running() const { return task_.running(); } // Started and not yet taken.
    // A progress message from this download, not one cancelled before it.
    bool current(LPARAM id) const { return task_.running() && id_ == id; }
    std::optional<DownloadResult> take() { return task_.take(); }

private:
    BackgroundTask<DownloadResult> task_;
    LPARAM id_ = 0; // This download's, while running().
};

} // namespace refarter
