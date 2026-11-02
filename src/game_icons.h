#pragma once

#include <windows.h>
#include <wincodec.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace refarter {

// The executable each game's clips were recorded from, so the library's folder of that game can show its icon.
// Keyed by the game as its folder knows it (lower case, the name a clip carries); remembered when a game window's
// clip is saved, or a monitor clip named after the listed game in front. Other monitor captures and clips without a game have no entry and keep the plain folder. Kept in
// %LOCALAPPDATA%\Refarter\games.dat beside the favourites.
using GameExecutables = std::map<std::wstring, std::wstring>;

std::filesystem::path gameExecutablesPath();
// Missing file is an empty map; unreadable or malformed data is an error, never overwritten.
std::optional<GameExecutables> readGameExecutables(const std::filesystem::path& file);
bool writeGameExecutables(const std::filesystem::path& file, const GameExecutables& games);

// Where an executable's icon of `size` pixels is cached as a PNG: %LOCALAPPDATA%\Refarter\icons\<hash>-<size>.png.
std::filesystem::path gameIconCachePath(const std::filesystem::path& folder, const std::filesystem::path& executable, int size);

// The icon of `executable` at `size` × `size` pixels as a 32-bit premultiplied top-down DIB for AlphaBlend (owned by
// the caller), or null when the file is gone or has none. Read from the cache while it is newer than the executable,
// otherwise extracted locally (IShellItemImageFactory) and cached. No network. For the library worker, never the UI.
HBITMAP gameIcon(IWICImagingFactory* wic, const std::filesystem::path& executable, int size, const std::filesystem::path& cacheFolder);

// The accent a game's icon gives its folder, tiles and the player (accent_colour.h), from a gameIcon bitmap; nothing
// for a grey or nearly colourless icon.
std::optional<COLORREF> gameIconAccent(HBITMAP icon);

} // namespace refarter
