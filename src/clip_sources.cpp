#include "clip_sources.h"
#include "local_file.h"
#include "text.h"
#include <iomanip>
#include <sstream>

namespace refarter {
namespace {
constexpr std::uintmax_t maxBytes = 4 * 1024 * 1024;
constexpr wchar_t header[] = L"refarter clip sources 1";

std::uint64_t ticks(const FILETIME& time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
} // namespace

std::filesystem::path clipSourcesPath() { return localAppDataFile(L"sources.dat"); }

std::wstring sourceFolder(const std::filesystem::path& folder) {
    auto text = folder.lexically_normal().wstring();
    while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/')) text.pop_back();
    return lower(text);
}

SourceKey sourceKey(const ClipFile& clip) { return {sourceFolder(clip.path.parent_path()), clip.bytes, ticks(clip.written)}; }

std::optional<ClipSources> readClipSources(const std::filesystem::path& file) {
    std::wstring text;
    switch (readUtf16File(file, maxBytes, text)) {
    case Utf16Read::Missing: return ClipSources{};
    case Utf16Read::Ok: break;
    default: return std::nullopt;
    }
    std::wistringstream lines(text);
    std::wstring line;
    if (!std::getline(lines, line) || line != header) return std::nullopt;
    ClipSources sources;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        std::wistringstream record(line);
        SourceKey key;
        std::wstring source;
        if (!(record >> std::quoted(key.folder) >> key.bytes >> key.written >> std::quoted(source))) return std::nullopt;
        record >> std::ws;
        if (!record.eof() || key.folder.empty() || source.empty()) return std::nullopt;
        sources[std::move(key)] = std::move(source);
    }
    return sources;
}

bool writeClipSources(const std::filesystem::path& file, const ClipSources& sources) {
    std::wostringstream text;
    text << header << L'\n';
    for (const auto& [key, source] : sources)
        text << std::quoted(key.folder) << L' ' << key.bytes << L' ' << key.written << L' ' << std::quoted(source) << L'\n';
    return writeUtf16FileAtomic(file, text.str(), maxBytes, L".sources-");
}

} // namespace refarter
