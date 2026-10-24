#include "leftover_files.h"
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>

namespace {
void touch(const std::filesystem::path& path, std::chrono::seconds age) {
    std::ofstream(path) << "x";
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() - age);
}
} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    using refarter::isLeftoverName;

    // What Refarter leaves behind.
    check(isLeftoverName(L"Apex Legends 2026-10-16 20-00-00.mp4.partial"), "save partial");
    check(isLeftoverName(L"clip 2026-10-16 20-00-00 (2).mp4.partial"), "save partial after a name collision");
    check(isLeftoverName(L"CLIP 2026-10-16 20-00-00.MP4.PARTIAL"), "match ignores case");
    check(isLeftoverName(L"League of Legends (TM) Client 2026-01-02 03-04-05.mp4.partial"), "game names may contain brackets");
    check(isLeftoverName(L"Apex Legends 2026-10-16 20-00-00.png.partial"), "screenshot partial");
    check(isLeftoverName(L"clip 2026-10-16 20-00-00 (2).PNG.PARTIAL"), "screenshot partial after a name collision, other case");
    check(isLeftoverName(L"refarter-export-{0DB50FB9-6D2E-4E0C-9C3F-4C3B7F0A1B2D}.part"), "export partial");
    check(isLeftoverName(L"refarter-export-{0db50fb9-6d2e-4e0c-9c3f-4c3b7f0a1b2d}.PART"), "export partial, other case");
    check(isLeftoverName(L"game 2026-10-16 20-00-00 trim.mp4.part"), "trim partial");
    check(isLeftoverName(L"game 2026-10-16 20-00-00 trim (2).mp4.part"), "trim partial after a name collision");
    check(isLeftoverName(L"game 2026-10-16 20-00-00 (2) trim (12).mp4.part"), "trim of a numbered clip, two digits");
    check(isLeftoverName(L"game 2026-10-16 20-00-00 trim trim.mp4.part"), "a trim of a trim");
    check(isLeftoverName(L"20mb_game 2026-10-16 20-00-00 (2) (3) trim.mp4.part"), "repeated collision suffixes before a trim");
    check(isLeftoverName(L"game 2026-10-16 20-00-00 trim (2) (3).mp4.part"), "repeated collision suffixes after a trim");
    check(isLeftoverName(L"game 2026-10-16 20-00-00 (2) (3).mp4.partial"), "repeated collision suffixes on a partial clip");
    check(!isLeftoverName(L"game (2) (3).mp4.partial"), "repeated suffixes do not replace the stamp");
    check(isLeftoverName(L"20mb_game 2026-10-16 20-00-00 trim.mp4.part"), "trim of a compressed copy");

    // What it must never touch.
    for (const auto* other : {L".mp4.partial", L"clip.mp4", L"clip.mp4.partial.bak", L"clip.partial", L"clip.mkv.partial",
             L"movie.mp4.part", L"download.part", L"notes.txt", L"refarter-export-.part",
             L"refarter-export-{0DB50FB9-6D2E-4E0C-9C3F-4C3B7F0A1B2D}.mp4", L"refarter-export-{0DB50FB9-6D2E-4E0C-9C3F-4C3B7F0A1B2}.part",
             L"refarter-export-{0DB50FB9-6D2E-4E0C-9C3F-4C3B7F0A1B2G}.part", L"refarter-export-0DB50FB9-6D2E-4E0C-9C3F-4C3B7F0A1B2D00.part",
             L"my refarter-export-{0DB50FB9-6D2E-4E0C-9C3F-4C3B7F0A1B2D}.part", L" trim.mp4.part", L"trim.mp4.part", L"game trim (x).mp4.part",
             L"game trim ().mp4.part", L"game (2).mp4.part", L"game trimmed.mp4.part", L"20mb_clip.mp4", L"clip.mp4.part.partial.txt", L""}) {
        check(!isLeftoverName(other), "unrelated name matched");
    }
    // Look-alikes from other programs: partial names without Refarter's stamp, or trims of something that is not a clip.
    for (const auto* other : {L"video trim.mp4.part", L"video.mp4.part", L"x.mp4.partial", L"old.mp4.partial", L"clip (2).mp4.partial",
             L"a trim.mp4.part", L"a trim (2).mp4.part", L"holiday trim.mp4.part", L"movie 2026-10-16 trim.mp4.part",
             L"game 2026-10-16 20-00-00.mp4.part", L"game 2026-10-16 20-00-00 trimmed.mp4.part",
             L" 2026-10-16 20-00-00.mp4.partial", L" 2026-10-16 20-00-00 trim.mp4.part",
             L"game 2026-13-16 20-00-00.mp4.partial", L"game 2026-10-32 20-00-00.mp4.partial", L"game 2026-10-16 24-00-00.mp4.partial",
             L"game 2026-10-16 20-60-00.mp4.partial", L"game 2026-10-16 20-00-00x.mp4.partial", L"game2026-10-16 20-00-00.mp4.partial",
             L"game 2026-10-16 20-00-00 (x).mp4.partial", L"game 26-10-16 20-00-00.mp4.partial", L"game 2026-10-16T20-00-00.mp4.partial",
             L"game 2026-10-16 20-00-00 trim (a).mp4.part", L"game 2026-10-16 20-00-00 copy trim.mp4.part",
             L"photo.png.partial", L"screenshot.png.partial", L"game 2026-10-16.png.partial", L" 2026-10-16 20-00-00.png.partial",
             L"game 2026-10-16 20-00-00 trim.png.part", L"game 2026-10-16 20-00-00.png.part", L"game 2026-10-16 20-00-00.png"}) {
        check(!isLeftoverName(other), "look-alike matched");
    }

    // Scanning: only matching, regular, old-enough files directly in the folder.
    const auto folder = std::filesystem::temp_directory_path() / (L"refarter-leftover-test-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(folder / L"nested");
    using std::chrono::seconds;
    touch(folder / L"old 2026-10-16 20-00-00.mp4.partial", seconds(600));
    touch(folder / L"fresh 2026-10-16 20-00-01.mp4.partial", seconds(5));
    touch(folder / L"refarter-export-{0DB50FB9-6D2E-4E0C-9C3F-4C3B7F0A1B2D}.part", seconds(3600));
    touch(folder / L"a 2026-10-16 20-00-02 trim.mp4.part", seconds(61));
    touch(folder / L"keep.mp4", seconds(9999));
    touch(folder / L"movie.mp4.part", seconds(9999));
    touch(folder / L"video trim.mp4.part", seconds(9999));
    touch(folder / L"x.mp4.partial", seconds(9999));
    touch(folder / L"nested" / L"inner 2026-10-16 20-00-03.mp4.partial", seconds(9999));
    std::filesystem::create_directories(folder / L"dir 2026-10-16 20-00-04.mp4.partial"); // A directory with a matching name is not a file.
    auto found = refarter::findLeftovers(folder);
    std::vector<std::wstring> names;
    for (const auto& path : found) names.push_back(path.filename().wstring());
    std::sort(names.begin(), names.end());
    check(names.size() == 3, "three old leftovers found");
    check(names.size() == 3 && names[0] == L"a 2026-10-16 20-00-02 trim.mp4.part" && names[1] == L"old 2026-10-16 20-00-00.mp4.partial" && names[2].rfind(L"refarter-export-", 0) == 0, "the right three");
    check(refarter::findLeftovers(folder, seconds(0)).size() == 4, "age limit applies");
    check(refarter::findLeftovers(folder / L"missing").empty(), "missing folder");

    // Actual production recovery moves, using only this test's disposable directory.
    const auto recovery = folder / refarter::leftoverRecoveryFolder;
    std::filesystem::create_directory(recovery);
    const auto collision = folder / L"collision 2026-10-16 20-00-05.mp4.partial";
    touch(collision, seconds(600));
    std::ofstream(recovery / collision.filename()) << "do not overwrite";
    const auto locked = folder / L"locked 2026-10-16 20-00-06.mp4.partial";
    touch(locked, seconds(600));
    const HANDLE lock = CreateFileW(locked.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(lock != INVALID_HANDLE_VALUE, "test writer opens its file");
    auto candidates = refarter::findLeftovers(folder);
    check(refarter::recoverLeftovers(candidates) == 3, "three old leftovers actually move to recovery");
    check(std::filesystem::exists(recovery / L"old 2026-10-16 20-00-00.mp4.partial"), "original filename retained");
    check(!std::filesystem::exists(folder / L"old 2026-10-16 20-00-00.mp4.partial"), "recovery is a move, not a second copy");
    check(std::filesystem::exists(collision), "colliding source stays in place");
    std::string content;
    { std::ifstream kept(recovery / collision.filename()); std::getline(kept, content); }
    check(content == "do not overwrite", "existing recovery file unchanged");
    check(std::filesystem::exists(locked), "active writer stays untouched");
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    const auto fresh = folder / L"fresh 2026-10-16 20-00-01.mp4.partial";
    check(refarter::recoverLeftovers({fresh}) == 0 && std::filesystem::exists(fresh), "age rechecked even for a previously selected path");
    check(refarter::recoverLeftovers({folder / L"keep.mp4"}) == 0, "unrelated file rejected by move itself");
    check(refarter::recoverLeftovers({locked}) == 1, "unlocked old file can be recovered later");
    check(refarter::recoverLeftovers({locked}) == 0, "repeated recovery is harmless");

    // A regular file at the destination folder name must never be replaced.
    const auto blocked = folder / L"blocked";
    std::filesystem::create_directory(blocked);
    std::ofstream(blocked / refarter::leftoverRecoveryFolder) << "keep folder-name collision";
    const auto blockedSource = blocked / L"blocked 2026-10-16 20-00-07.mp4.partial";
    touch(blockedSource, seconds(600));
    check(refarter::recoverLeftovers({blockedSource}) == 0 && std::filesystem::exists(blockedSource), "non-directory recovery target leaves source intact");
    const auto redirect = folder / L"redirect";
    const auto elsewhere = folder / L"elsewhere";
    std::filesystem::create_directory(redirect);
    std::filesystem::create_directory(elsewhere);
    std::error_code linkError;
    std::filesystem::create_directory_symlink(elsewhere, redirect / refarter::leftoverRecoveryFolder, linkError);
    if (!linkError) {
        const auto source = redirect / L"redirect 2026-10-16 20-00-08.mp4.partial";
        touch(source, seconds(600));
        check(refarter::recoverLeftovers({source}) == 0 && std::filesystem::exists(source), "reparse recovery directory rejected");
        check(std::filesystem::is_empty(elsewhere), "recovery link target untouched");
    } else std::cout << "SKIP: directory symlink privilege unavailable\n";

    // Screenshots are written in <clip folder>\screenshots: its leftovers are found with the clip folder's and move to
    // the recovery folder of their own folder; a look-alike there stays.
    const auto clips = folder / L"clips";
    const auto shots = clips / refarter::leftoverScreenshotsFolder;
    std::filesystem::create_directories(shots);
    touch(clips / L"save 2026-10-16 20-00-09.mp4.partial", seconds(600));
    touch(shots / L"shot 2026-10-16 20-00-10.png.partial", seconds(600));
    touch(shots / L"fresh 2026-10-16 20-00-11.png.partial", seconds(5));
    touch(shots / L"photo.png.partial", seconds(600));
    check(refarter::findLeftovers(clips).size() == 1, "the clip folder scan alone does not look into screenshots");
    const auto all = refarter::findAllLeftovers(clips);
    check(all.size() == 2, "clip folder and screenshots subfolder both scanned, old files only");
    check(refarter::recoverLeftovers(all) == 2, "both recovered");
    check(std::filesystem::exists(clips / refarter::leftoverRecoveryFolder / L"save 2026-10-16 20-00-09.mp4.partial"), "clip leftover in the clip recovery folder");
    check(std::filesystem::exists(shots / refarter::leftoverRecoveryFolder / L"shot 2026-10-16 20-00-10.png.partial"), "screenshot leftover in the screenshots recovery folder");
    check(std::filesystem::exists(shots / L"photo.png.partial") && std::filesystem::exists(shots / L"fresh 2026-10-16 20-00-11.png.partial"), "look-alike and fresh screenshot partial stay");

    std::filesystem::remove_all(folder);
    return failures ? 1 : 0;
}
