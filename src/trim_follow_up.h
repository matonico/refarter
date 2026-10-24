#pragma once

#include "player/clip_export.h"
#include "player/toast_kind.h"

#include <filesystem>
#include <cstdint>
#include <string>

namespace refarter {

// "compress & copy" in the player's trim bar: what happens after a trim is saved. Pure decisions only; the
// App starts the compression job and puts the file on the clipboard (clip_selection.h).

// The box is unavailable while a compression runs, because only one job runs at a time.
inline bool compressCopyAvailable(bool compressionRunning) { return !compressionRunning; }
inline constexpr const wchar_t* compressionBusyHint = L"a compression is already running";

enum class AfterTrim { Nothing, Compress };

// `ticked`: the box as it was when the trim was saved. `trimmed`: the trim produced a clip (a failed or
// cancelled trim never gets a follow-up). `compressionRunning`: a job started meanwhile, e.g. from the library
// while the trim was still being written.
inline AfterTrim afterTrim(bool ticked, bool trimmed, bool compressionRunning) {
    return ticked && trimmed && !compressionRunning ? AfterTrim::Compress : AfterTrim::Nothing;
}

enum class AfterCompression { Nothing, CopyFile };

struct FollowUpResult {
    AfterCompression action = AfterCompression::Nothing;
    std::filesystem::path file; // What to copy to the clipboard.
    std::wstring note;          // Shown when nothing is copied (empty: say nothing).
    player::ToastKind kind = player::ToastKind::Notice; // The note's icon.
};

// The trimmed clip a compression was started for. It decides, from the finished export, whether and what to
// copy: the compressed file, or the trimmed clip itself when it already fits. A cancelled or failed export,
// or one cancelled just as it ended, copies nothing.
class TrimFollowUp {
public:
    void arm(const std::filesystem::path& trimmed, const std::filesystem::path& source = {}, bool replace = false) {
        clip_ = trimmed; source_ = source; replace_ = replace; cancelled_ = false;
    }
    bool pending() const { return !clip_.empty(); }
    const std::filesystem::path& clip() const { return clip_; }
    const std::filesystem::path& source() const { return source_; }
    bool replacesSource() const { return replace_; }
    // The user cancelled the compression (clip menu).
    void cancel() { if (pending()) cancelled_ = true; }
    // The compression could not start.
    void drop() { clip_.clear(); source_.clear(); replace_ = false; cancelled_ = false; }

    // `finished`: the clip the job worked on. A result for any other clip is not ours and changes nothing.
    FollowUpResult finish(const std::filesystem::path& finished, const player::ExportResult& result) {
        FollowUpResult plan;
        if (!pending() || finished != clip_) return plan;
        const auto trimmed = clip_;
        const bool wasCancelled = cancelled_ || result.cancelled;
        drop();
        if (wasCancelled) {
            plan.note = L"compression cancelled";
            plan.kind = player::ToastKind::Notice;
        } else if (!result.error.empty()) {
            plan.note = L"compression failed";
            plan.kind = player::ToastKind::Error;
        } else if (result.alreadyFits) {
            plan = {AfterCompression::CopyFile, trimmed, {}};
        } else if (!result.path.empty()) {
            plan = {AfterCompression::CopyFile, result.path, {}};
        } else {
            plan.note = L"compression failed";
            plan.kind = player::ToastKind::Error;
        }
        return plan;
    }

private:
    std::filesystem::path clip_;
    std::filesystem::path source_;
    bool replace_ = false;
    bool cancelled_ = false;
};

// The player's toast line while it runs: "compressing · 42%".
inline std::wstring compressingNote(float progress) {
    const int percent = progress <= 0 ? 0 : progress >= 1 ? 100 : static_cast<int>(progress * 100);
    return L"compressing · " + std::to_wstring(percent) + L"%";
}
inline constexpr const wchar_t* copiedToClipboardNote = L"copied to clipboard";
inline constexpr const wchar_t* copyFailedNote = L"could not copy · the clipboard is busy";
inline constexpr const wchar_t* copyMissingNote = L"copy failed · file moved or deleted";

} // namespace refarter
