#include "trim_follow_up.h"
#include <iostream>

using namespace refarter;

namespace {
int failures = 0;
void check(bool passes, const char* name) {
    if (!passes) { std::cerr << name << '\n'; ++failures; }
}
player::ExportResult exported(const wchar_t* path, const wchar_t* error = L"", bool cancelled = false, bool fits = false) {
    player::ExportResult result;
    result.path = path;
    result.error = error;
    result.cancelled = cancelled;
    result.alreadyFits = fits;
    return result;
}
} // namespace

int main() {
    // What happens right after a trim.
    check(afterTrim(true, true, false) == AfterTrim::Compress, "ticked box and a saved trim compress");
    check(afterTrim(false, true, false) == AfterTrim::Nothing, "unticked box does nothing");
    check(afterTrim(true, false, false) == AfterTrim::Nothing, "a failed or cancelled trim never compresses");
    check(afterTrim(true, true, true) == AfterTrim::Nothing, "a running compression is not queued behind");
    check(!compressCopyAvailable(true) && compressCopyAvailable(false), "box disabled while a compression runs");

    const std::filesystem::path trimmed = L"d:\\clips\\game trim.mp4", small = L"d:\\clips\\20mb_game trim.mp4";
    const std::filesystem::path original = L"d:\\clips\\game.mp4";
    TrimFollowUp replacement;
    replacement.arm(trimmed, original, true);
    check(replacement.source() == original && replacement.replacesSource(), "replace source retained until compression finishes");
    replacement.finish(trimmed, exported(L"d:\\clips\\20mb_game trim.mp4"));
    check(replacement.source().empty() && !replacement.replacesSource(), "completion clears replacement intent");

    // Compressed copy: copy that file.
    TrimFollowUp follow;
    check(!follow.pending(), "idle at first");
    follow.arm(trimmed);
    check(follow.pending() && follow.clip() == trimmed, "armed for the trimmed clip");
    auto plan = follow.finish(trimmed, exported(L"d:\\clips\\20mb_game trim.mp4"));
    check(plan.action == AfterCompression::CopyFile && plan.file == small && plan.note.empty(), "copies the compressed file");
    check(!follow.pending(), "finished disarms");

    // Already under the limit: the trimmed clip itself is copied.
    follow.arm(trimmed);
    plan = follow.finish(trimmed, exported(L"", L"", false, true));
    check(plan.action == AfterCompression::CopyFile && plan.file == trimmed, "already fits copies the trimmed clip");

    // Replace uses a separate trim until compression succeeds; the original is not the export input.
    follow.arm(trimmed, original, true);
    plan = follow.finish(trimmed, exported(L"d:\\clips\\20mb_game.mp4"));
    check(plan.action == AfterCompression::CopyFile && plan.file == L"d:\\clips\\20mb_game.mp4", "replace flow copies the compressed new file");
    follow.arm(trimmed, original, true);
    plan = follow.finish(trimmed, exported(L"", L"", false, true));
    check(plan.file == trimmed, "replace flow already fits keeps and copies the trim");

    // Failure, cancellation and a cancel that raced the end copy nothing.
    follow.arm(trimmed);
    plan = follow.finish(trimmed, exported(L"", L"encoder failed"));
    check(plan.action == AfterCompression::Nothing && plan.file.empty() && !plan.note.empty(), "failure copies nothing");
    check(plan.kind == player::ToastKind::Error, "a failure is an error toast");
    follow.arm(trimmed);
    plan = follow.finish(trimmed, exported(L"", L"", true));
    check(plan.action == AfterCompression::Nothing && !plan.note.empty(), "cancelled export copies nothing");
    check(plan.kind == player::ToastKind::Notice, "a cancel is a notice");
    follow.arm(trimmed);
    follow.cancel();
    plan = follow.finish(trimmed, exported(L"d:\\clips\\20mb_game trim.mp4"));
    check(plan.action == AfterCompression::Nothing, "cancel from the menu wins over a result that completed");
    follow.arm(trimmed);
    follow.cancel();
    plan = follow.finish(trimmed, exported(L"", L"", false, true));
    check(plan.action == AfterCompression::Nothing, "cancel wins over already fits too");
    follow.arm(trimmed);
    plan = follow.finish(trimmed, exported(L""));
    check(plan.action == AfterCompression::Nothing && !plan.note.empty(), "no output file copies nothing");

    // A result for another clip (a manual compression) is not ours.
    follow.arm(trimmed);
    plan = follow.finish(L"d:\\clips\\other.mp4", exported(L"d:\\clips\\20mb_other.mp4"));
    check(plan.action == AfterCompression::Nothing && plan.note.empty() && follow.pending(), "other clips are ignored");
    follow.drop();
    check(!follow.pending(), "drop disarms");
    plan = follow.finish(trimmed, exported(L"d:\\clips\\20mb_game trim.mp4"));
    check(plan.action == AfterCompression::Nothing, "nothing armed, nothing copied");
    follow.cancel();
    follow.arm(trimmed);
    plan = follow.finish(trimmed, exported(L"d:\\clips\\20mb_game trim.mp4"));
    check(plan.action == AfterCompression::CopyFile, "a cancel while idle does not leak into the next one");

    check(compressingNote(0.424f) == L"compressing \u00b7 42%" && compressingNote(-1) == L"compressing \u00b7 0%" &&
          compressingNote(2) == L"compressing \u00b7 100%", "progress text");
    return failures ? 1 : 0;
}
