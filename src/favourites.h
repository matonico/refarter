#pragma once
#include "clip_list.h"
#include <optional>
#include <set>

namespace refarter {
using Favourites = std::set<std::wstring>;
std::filesystem::path favouritesPath();
std::wstring favouriteKey(const ClipFile& clip);
// Missing file is an empty set; unreadable/malformed data is an error, never overwritten.
std::optional<Favourites> readFavourites(const std::filesystem::path& file);
bool writeFavourites(const std::filesystem::path& file, const Favourites& favourites);
} // namespace refarter
