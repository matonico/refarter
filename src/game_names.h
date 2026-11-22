#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace refarter {

// Names the user gave games, display only (App::displayGameName): shown instead of the short or stored name.
// Stored labels, clip names, library folders and icons never change. Kept by executable path in game_names.dat,
// apart from the auto-capture and per-game settings files, so older versions read those as before.
struct CustomGameName {
    std::wstring path; // Compared case-insensitively, as auto-capture and per-game settings compare it.
    std::wstring name;
};

inline constexpr std::size_t maxCustomGameName = 80; // As long as a clip's game name may be.

struct GameNames {
    std::vector<CustomGameName> games;

    const CustomGameName* find(const std::wstring& path) const;
    // Keeps `name` cleaned (customGameName); a name that is empty once cleaned removes the game's name instead.
    void set(std::wstring path, std::wstring name);
    void remove(const std::wstring& path);
};

// A typed name as it is kept: on one line, without spaces at either end, at most maxCustomGameName characters
// (never cut inside a surrogate pair).
std::wstring customGameName(std::wstring typed);

std::filesystem::path gameNamesPath();
std::wstring formatGameNames(const GameNames& names);
// Malformed text is an error rather than a partial list, so it is never overwritten.
std::optional<GameNames> parseGameNames(const std::wstring& text);
// A missing file is no names; an unreadable or malformed one is an error.
std::optional<GameNames> readGameNames(const std::filesystem::path& file);
bool writeGameNames(const std::filesystem::path& file, const GameNames& names);

} // namespace refarter
