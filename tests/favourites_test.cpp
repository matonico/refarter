#include "clip_sources.h"
#include "favourites.h"
#include <objbase.h>
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
    std::filesystem::remove(folder);
    return failures ? 1 : 0;
}
