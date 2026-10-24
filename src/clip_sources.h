#pragma once
#include "clip_list.h"
#include <compare>
#include <map>
#include <optional>

namespace refarter {

// The game of a clip renamed in Refarter, whose new name no longer says it (the library's folders read the game
// from the name otherwise). Keyed by folder, size and write time, which a rename keeps: no file name, so the entry
// follows the clip to its new name. Kept in %LOCALAPPDATA%\Refarter beside the favourites.
struct SourceKey {
    std::wstring folder; // Lower case, normalised.
    std::uint64_t bytes = 0, written = 0;
    auto operator<=>(const SourceKey&) const = default;
};
using ClipSources = std::map<SourceKey, std::wstring>;

std::filesystem::path clipSourcesPath();
SourceKey sourceKey(const ClipFile& clip);
std::wstring sourceFolder(const std::filesystem::path& folder); // A key's folder text for a clip folder.
// Missing file is an empty map; unreadable/malformed data is an error, never overwritten.
std::optional<ClipSources> readClipSources(const std::filesystem::path& file);
bool writeClipSources(const std::filesystem::path& file, const ClipSources& sources);

} // namespace refarter
