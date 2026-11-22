#include "game_names.h"
#include "local_file.h"
#include "path_list.h"
#include "text.h"

#include <windows.h>
#include <cwctype>
#include <iomanip>
#include <sstream>

namespace refarter {
namespace {
constexpr const wchar_t* header = L"refarter game names 1";
constexpr std::uintmax_t maxBytes = 1024 * 1024;
} // namespace

const CustomGameName* GameNames::find(const std::wstring& path) const { return findByPath(games, path); }

void GameNames::set(std::wstring path, std::wstring name) {
    name = customGameName(std::move(name));
    if (name.empty()) return remove(path);
    if (auto* game = findByPath(games, path)) {
        game->name = std::move(name);
        return;
    }
    games.push_back({std::move(path), std::move(name)});
}

void GameNames::remove(const std::wstring& path) { eraseByPath(games, path); }

std::wstring customGameName(std::wstring typed) {
    typed = oneLineLabel(std::move(typed));
    std::erase(typed, L'\0');
    const auto space = [](wchar_t c) { return std::iswspace(c) != 0; };
    while (!typed.empty() && space(typed.back())) typed.pop_back();
    size_t first = 0;
    while (first < typed.size() && space(typed[first])) ++first;
    typed.erase(0, first);
    return typed.size() > maxCustomGameName ? cutText(typed, maxCustomGameName) : typed;
}

std::filesystem::path gameNamesPath() { return localAppDataFile(L"game_names.dat"); }

std::wstring formatGameNames(const GameNames& names) {
    std::wostringstream text;
    text << header << L'\n';
    for (const auto& game : names.games) text << L"game " << std::quoted(game.path) << L' ' << std::quoted(game.name) << L'\n';
    return text.str();
}

std::optional<GameNames> parseGameNames(const std::wstring& text) {
    std::wistringstream lines(text);
    std::wstring line;
    // A hand edit may have saved CRLF: every line, the header and blank ones too, loses its '\r'.
    const auto next = [&] {
        if (!std::getline(lines, line)) return false;
        if (line.ends_with(L'\r')) line.pop_back();
        return true;
    };
    if (!next() || line != header) return std::nullopt;
    GameNames names;
    while (next()) {
        if (line.empty()) continue;
        std::wistringstream record(line);
        std::wstring kind, path, name;
        if (!(record >> kind >> std::quoted(path) >> std::quoted(name)) || kind != L"game") return std::nullopt;
        record >> std::ws;
        if (!record.eof() || !std::filesystem::path(path).is_absolute() || !singleLine(path) || !singleLine(name)) return std::nullopt;
        names.set(std::move(path), std::move(name));
    }
    return names;
}

std::optional<GameNames> readGameNames(const std::filesystem::path& file) {
    std::wstring text;
    switch (readUtf16File(file, maxBytes, text)) {
    case Utf16Read::Missing: return GameNames{};
    case Utf16Read::Ok: return parseGameNames(text);
    default: return std::nullopt;
    }
}

bool writeGameNames(const std::filesystem::path& file, const GameNames& names) {
    return writeUtf16FileAtomic(file, formatGameNames(names), maxBytes, L".game-names-");
}

} // namespace refarter
