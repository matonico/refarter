#pragma once

#include <windows.h>
#include <filesystem>
#include <optional>
#include <cstddef>
#include <string>
#include <string_view>

namespace refarter::capture {

// "Videos\Refarter" in the user's profile, following a relocated Videos folder.
std::filesystem::path defaultClipFolder();

// The longest file name Refarter makes in a clip folder, from clipFileName's limits: the game name (at most 80
// characters, 81 with the "_" a device name gets), the 20-character time stamp " 2026-10-12 21-04-05", the " (999)" of
// the last free name, ".mp4" (or ".png") and the writer's ".partial". A folder whose path plus a separator plus this
// reaches MAX_PATH (260) would make every clip fail. The selected root also reserves the screenshots subfolder.
inline constexpr std::size_t maxGameNameLength = 80;
inline constexpr std::size_t clipStampLength = 20;
inline constexpr std::size_t longestClipFileName = (maxGameNameLength + 1) + clipStampLength + 6 + 4 + 8;
inline constexpr std::size_t maxOutputFolderLength = 260 - 1 - longestClipFileName - 1;
inline constexpr std::size_t maxClipFolderLength = maxOutputFolderLength - std::wstring_view(L"\\screenshots").size();
enum class ClipFolderKind { Clips, Screenshots };

// Creates the folder if needed and proves a file can be written there, so a
// bad location is reported when it is chosen, not when a clip is lost.
std::optional<std::wstring> prepareClipFolder(const std::filesystem::path& folder, ClipFolderKind kind = ClipFolderKind::Clips);

// "<game> 2026-10-12 21-04-05.mp4" with characters Windows rejects replaced.
std::wstring clipFileName(std::wstring_view game, const SYSTEMTIME& localTime);

// `path` if nothing is there, else the first free " (2)", " (3)"... of its name (a " (N)" it already carries is
// replaced, not stacked). A path that cannot be checked counts as taken. Empty when every number up to 999 is taken.
std::filesystem::path nextFreePath(const std::filesystem::path& path);

// clipFileName in `folder` through nextFreePath (empty when no free name was found: the save must fail, not reuse
// one). `extension` replaces ".mp4" (".png" for a screenshot).
std::filesystem::path nextClipPath(const std::filesystem::path& folder, std::wstring_view game, std::wstring_view extension = L".mp4");

// Drops a trailing separator ("D:\Clips\" becomes "D:\Clips") and tidies "." and ".." parts; a drive root
// keeps its separator. Clip lists name their folder this way, so a folder must be compared in this form.
std::filesystem::path normalisedFolder(const std::filesystem::path& folder);

} // namespace refarter::capture
