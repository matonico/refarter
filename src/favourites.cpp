#include "favourites.h"
#include "local_file.h"
#include "text.h"
#include <iomanip>
#include <sstream>

namespace refarter {
namespace {
constexpr std::uintmax_t maxBytes = 16 * 1024 * 1024;
}

std::filesystem::path favouritesPath() { return localAppDataFile(L"favourites.dat"); }

std::wstring favouriteKey(const ClipFile& clip) {
    auto path = clip.path.lexically_normal().wstring();
    path = lower(path);
    std::wostringstream record;
    record << std::quoted(path) << L' ' << clip.bytes << L' ' << clip.written.dwLowDateTime << L' ' << clip.written.dwHighDateTime;
    return record.str();
}

std::optional<Favourites> readFavourites(const std::filesystem::path& file) {
    std::wstring text;
    switch (readUtf16File(file, maxBytes, text)) {
    case Utf16Read::Missing: return Favourites{};
    case Utf16Read::Ok: break;
    default: return std::nullopt;
    }
    std::wistringstream lines(text);
    std::wstring line;
    if (!std::getline(lines, line) || line != L"refarter favourites 1") return std::nullopt;
    Favourites favourites;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        std::wistringstream record(line);
        std::wstring path;
        ClipFile clip;
        if (!(record >> std::quoted(path) >> clip.bytes >> clip.written.dwLowDateTime >> clip.written.dwHighDateTime)) return std::nullopt;
        record >> std::ws;
        clip.path = path;
        if (!record.eof() || !clip.path.is_absolute() || !isClipFile(clip.path) || path.find_first_of(L"\r\n\0", 0, 3) != std::wstring::npos)
            return std::nullopt;
        favourites.insert(favouriteKey(clip));
    }
    return favourites;
}

bool writeFavourites(const std::filesystem::path& file, const Favourites& favourites) {
    std::wstring text = L"refarter favourites 1\n";
    for (const auto& record : favourites) text += record + L'\n';
    return writeUtf16FileAtomic(file, text, maxBytes, L".favourites-");
}
} // namespace refarter
