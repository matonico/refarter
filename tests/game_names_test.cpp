#include "game_names.h"
#include <windows.h>
#include <iostream>

using namespace refarter;

int wmain() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };

    check(customGameName(L"  bo6 \t") == L"bo6", "a name is trimmed");
    check(customGameName(L"zelda\r\nemulator") == L"zelda  emulator", "a name stays on one line");
    check(customGameName(L"   ").empty(), "spaces alone are no name");
    check(customGameName(std::wstring(100, L'x')).size() == maxCustomGameName, "a name is cut to the limit");
    auto pair = std::wstring(maxCustomGameName - 1, L'x') + L"\U0001F3AE"; // The emoji's pair would be split by the limit.
    check(customGameName(pair) == std::wstring(maxCustomGameName - 1, L'x'), "the cut never splits a surrogate pair");

    GameNames names;
    names.set(L"C:\\Games\\CoD\\cod.exe", L" bo6 ");
    names.set(L"c:\\games\\cod\\COD.exe", L"black ops 6"); // Same game: renamed, no duplicate.
    check(names.games.size() == 1 && names.games[0].name == L"black ops 6", "paths compare case-insensitively");
    check(names.find(L"C:\\GAMES\\COD\\cod.exe") != nullptr, "find ignores case");
    names.set(L"D:\\Emu\\lichii.exe", L"\"switch\" games");
    names.set(L"C:\\Games\\CoD\\cod.exe", L"  ");
    check(!names.find(L"C:\\Games\\CoD\\cod.exe") && names.games.size() == 1, "an empty name resets the game");
    names.remove(L"d:\\emu\\LICHII.exe");
    check(names.games.empty(), "remove resets too");
    names.set(L"D:\\Emu\\lichii.exe", L"\"switch\" games");

    const auto text = formatGameNames(names);
    const auto parsed = parseGameNames(text);
    check(parsed && parsed->games.size() == 1 && parsed->games[0].name == L"\"switch\" games", "round trip keeps quotes");
    check(!parseGameNames(L"refarter auto-capture 1\n"), "a wrong header is an error");
    check(!parseGameNames(L"refarter game names 1\ngame \"relative.exe\" \"x\"\n"), "relative paths are an error");
    check(!parseGameNames(L"refarter game names 1\ngame \"C:\\a.exe\"\n"), "a game needs a name");
    check(!parseGameNames(L"refarter game names 1\nname \"C:\\a.exe\" \"x\"\n"), "unknown records are an error");
    check(parseGameNames(L"refarter game names 1\n") && parseGameNames(L"refarter game names 1\n")->games.empty(), "no names");
    const auto crlf = parseGameNames(L"refarter game names 1\r\n\r\ngame \"C:\\\\a.exe\" \"x\"\r\n\r\n");
    check(crlf && crlf->games.size() == 1 && crlf->games[0].name == L"x", "a hand edit saved with CRLF still reads");

    wchar_t folder[MAX_PATH]{}, temporary[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, folder) || !GetTempFileNameW(folder, L"rgn", 0, temporary)) return 1;
    std::filesystem::remove(temporary);
    check(readGameNames(temporary) && readGameNames(temporary)->games.empty(), "a missing file is no names");
    check(writeGameNames(temporary, names), "names are written");
    const auto read = readGameNames(temporary);
    check(read && formatGameNames(*read) == text, "file round trip");
    std::filesystem::remove(temporary);
    return failures ? 1 : 0;
}
