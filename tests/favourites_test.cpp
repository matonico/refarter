#include "clip_sources.h"
#include "favourites.h"
#include "game_icons.h"
#include <objbase.h>
#include <winrt/base.h>
#include <fstream>
#include <iostream>

int main() {
    using namespace refarter;
    int failures = 0;
    const auto check = [&](bool value, const char* message) { if (!value) { ++failures; std::cerr << message << '\n'; } };
    ClipFile first{L"C:\\Clips ü\\apex legends.mp4", {12, 34}, 567};
    ClipFile same{L"c:\\clips ü\\apex legends.mp4", {12, 34}, 567};
    check(favouriteKey(first) == favouriteKey(same), "Windows path case preserves identity");
    auto replaced = first;
    ++replaced.bytes;
    check(favouriteKey(first) != favouriteKey(replaced), "replacement size does not inherit mark");
    replaced = first;
    ++replaced.written.dwLowDateTime;
    check(favouriteKey(first) != favouriteKey(replaced), "replacement timestamp does not inherit mark");
    replaced = first;
    replaced.path = L"D:\\Clips ü\\apex legends.mp4";
    check(favouriteKey(first) != favouriteKey(replaced), "different folder stays independent");
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temporary)) return 1;
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) return 1;
    wchar_t unique[40]{};
    StringFromGUID2(id, unique, static_cast<int>(std::size(unique)));
    const auto folder = std::filesystem::path(temporary) / (L"refarter-favourites-" + std::wstring(unique));
    const auto file = folder / L"favourites.dat";
    check(readFavourites(file) == std::optional(Favourites{}), "missing metadata starts empty");
    const Favourites original{favouriteKey(first), favouriteKey(replaced)};
    check(writeFavourites(file, original) && readFavourites(file) == std::optional(original), "Unicode identities round trip atomically");
    auto removed = original;
    removed.erase(favouriteKey(first));
    check(writeFavourites(file, removed) && readFavourites(file) == std::optional(removed), "unmark persists without losing other folders");
    check(writeFavourites(file, {}) && readFavourites(file) == std::optional(Favourites{}), "empty marks survive relaunch");
    std::ofstream corrupt(file, std::ios::binary | std::ios::trunc);
    corrupt << "not a favourites file";
    check(static_cast<bool>(corrupt), "corrupt fixture written successfully");
    corrupt.close();
    check(!readFavourites(file), "corruption is reported rather than treated as empty");
    // A zero-byte file holds nothing to protect: it reads as empty and is rewritten.
    { std::ofstream empty(file, std::ios::binary | std::ios::trunc); }
    check(readFavourites(file) == std::optional(Favourites{}), "a zero-byte favourites file reads as empty");
    check(writeFavourites(file, original) && readFavourites(file) == std::optional(original), "a zero-byte favourites file is rewritten");
    std::filesystem::remove(file);
    // A directory at the destination forces replacement failure: no success or leaked temp file.
    std::filesystem::remove(file);
    std::filesystem::create_directory(file);
    check(!writeFavourites(file, original), "replacement failure reported");
    int entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) { (void)entry; ++entries; }
    check(entries == 1, "failed atomic write removes temporary file");
    std::filesystem::remove(file);

    // Remembered games: keyed without the file name, so a rename in the same folder keeps the entry.
    auto renamedClip = first;
    renamedClip.path = L"c:\\clips \u00fc\\sick clutch.mp4";
    check(sourceKey(first) == sourceKey(renamedClip), "a rename keeps the remembered game");
    check(sourceKey(first) != sourceKey(replaced), "another folder is another clip");
    auto rewritten = first;
    ++rewritten.bytes;
    check(sourceKey(first) != sourceKey(rewritten), "a replaced file is another clip");
    const auto sourcesFile = folder / L"sources.dat";
    check(readClipSources(sourcesFile) == std::optional(ClipSources{}), "missing sources start empty");
    const ClipSources games{{sourceKey(first), L"Apex \"Legends\" \u00fc"}, {sourceKey(replaced), L"cs2"}};
    check(writeClipSources(sourcesFile, games) && readClipSources(sourcesFile) == std::optional(games), "games round trip");
    std::ofstream broken(sourcesFile, std::ios::binary | std::ios::trunc);
    broken << "not a sources file";
    broken.close();
    check(!readClipSources(sourcesFile), "corrupt sources are reported rather than treated as empty");
    { std::ofstream empty(sourcesFile, std::ios::binary | std::ios::trunc); }
    check(readClipSources(sourcesFile) == std::optional(ClipSources{}), "a zero-byte sources file reads as empty");
    check(writeClipSources(sourcesFile, games) && readClipSources(sourcesFile) == std::optional(games), "a zero-byte sources file is rewritten");
    std::filesystem::remove(sourcesFile);

    // The executable behind each game folder, and its cached icon.
    const auto gamesFile = folder / L"games.dat";
    check(readGameExecutables(gamesFile) == std::optional(GameExecutables{}), "missing game executables start empty");
    const GameExecutables executables{{L"apex legends", L"C:\\Games\\Apex \"Legends\"\\r5apex.exe"}, {L"caf\u00e9 \U0001F430", L"D:\\x.exe"}};
    check(writeGameExecutables(gamesFile, executables) && readGameExecutables(gamesFile) == std::optional(executables), "game executables round trip");
    { std::ofstream junk(gamesFile, std::ios::binary | std::ios::trunc); junk << "nonsense"; }
    check(!readGameExecutables(gamesFile), "corrupt game executables are reported rather than treated as empty");
    std::filesystem::remove(gamesFile);
    const auto icons = folder / L"icons";
    check(gameIconCachePath(icons, L"C:\\Games\\A.exe", 22) == gameIconCachePath(icons, L"c:\\games\\a.EXE", 22) &&
          gameIconCachePath(icons, L"C:\\Games\\A.exe", 22) != gameIconCachePath(icons, L"C:\\Games\\A.exe", 33) &&
          gameIconCachePath(icons, L"C:\\Games\\A.exe", 22) != gameIconCachePath(icons, L"C:\\Games\\B.exe", 22), "one cache file per executable and size");
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        winrt::com_ptr<IWICImagingFactory> wic;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()));
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        const HBITMAP icon = gameIcon(wic.get(), self, 22, icons);
        const auto cached = gameIconCachePath(icons, self, 22);
        BITMAP shape{};
        check(icon && GetObjectW(icon, sizeof(shape), &shape) && shape.bmWidth == 22 && shape.bmBitsPixel == 32, "an executable's icon at the size asked");
        check(std::filesystem::exists(cached), "the icon is cached as a PNG");
        std::error_code error;
        const auto made = std::filesystem::last_write_time(cached, error);
        if (icon) DeleteObject(icon);
        const HBITMAP again = gameIcon(wic.get(), self, 22, icons);
        check(again && std::filesystem::last_write_time(cached, error) == made, "a cached icon is not extracted again");
        if (again) DeleteObject(again);
        check(!gameIcon(wic.get(), folder / L"gone.exe", 22, icons), "a missing executable has no icon");
        wic = nullptr;
        CoUninitialize();
    }
    std::filesystem::remove_all(icons);
    std::filesystem::remove(folder);
    return failures ? 1 : 0;
}
