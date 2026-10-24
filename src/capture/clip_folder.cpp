#include "clip_folder.h"
#include "save_errors.h"

#include <shlobj.h>
#include <algorithm>
#include <cwchar>
#include <system_error>

namespace refarter::capture {

std::filesystem::path defaultClipFolder() {
    PWSTR videos = nullptr;
    std::filesystem::path folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_DEFAULT, nullptr, &videos))) folder = videos;
    CoTaskMemFree(videos);
    return folder.empty() ? folder : folder / L"Refarter";
}

std::optional<std::wstring> prepareClipFolder(const std::filesystem::path& folder, ClipFolderKind kind) {
    if (folder.empty() || !folder.is_absolute()) return L"choose a full folder path, such as D:\\Clips";
    // The probe below is short; a real clip name is not. Count the separator Refarter adds, never a trailing one.
    auto length = folder.native().size();
    while (length && (folder.native()[length - 1] == L'\\' || folder.native()[length - 1] == L'/')) --length;
    const auto limit = kind == ClipFolderKind::Screenshots ? maxOutputFolderLength : maxClipFolderLength;
    if (length > limit)
        return L"this folder's path is too long for clip names · choose one of at most " + std::to_wstring(limit) + L" characters";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error || !std::filesystem::is_directory(folder, error)) return folderFailureText(error, L"the clip folder could not be created");
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) return L"could not check the clip folder";
    wchar_t unique[40]{};
    StringFromGUID2(id, unique, static_cast<int>(std::size(unique)));
    const auto probe = folder / (std::wstring(L".refarter-write-test-") + unique);
    const HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return folderFailureText(std::error_code(GetLastError(), std::system_category()), L"clips cannot be written to this folder");
    DWORD written = 0;
    const char byte = 0;
    const bool wrote = WriteFile(file, &byte, 1, &written, nullptr) != FALSE;
    const auto writeError = wrote ? ERROR_WRITE_FAULT : GetLastError(); // Capture before CloseHandle changes it.
    const bool writable = wrote && written == 1;
    CloseHandle(file);
    if (!writable)
        return folderFailureText(std::error_code(writeError, std::system_category()), L"clips cannot be written to this folder");
    return std::nullopt;
}

std::wstring clipFileName(std::wstring_view game, const SYSTEMTIME& t) {
    std::wstring name(game);
    if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) name.resize(name.size() - 4);
    for (auto& c : name) {
        if (c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) c = L'_';
    }
    if (name.size() > maxGameNameLength) name.resize(maxGameNameLength);
    // Do not split a UTF-16 surrogate pair when truncating a window title.
    if (!name.empty() && name.back() >= 0xd800 && name.back() <= 0xdbff) name.pop_back();
    while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
    if (name.empty()) name = L"clip";
    // Windows device names remain reserved before an extension (e.g. CON.demo).
    auto stem = name.substr(0, name.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    const bool numberedDevice = stem.size() == 4 &&
        (_wcsnicmp(stem.c_str(), L"COM", 3) == 0 || _wcsnicmp(stem.c_str(), L"LPT", 3) == 0) &&
        std::wstring_view(L"123456789¹²³").find(stem.back()) != std::wstring_view::npos;
    if (_wcsicmp(stem.c_str(), L"CON") == 0 || _wcsicmp(stem.c_str(), L"PRN") == 0 ||
        _wcsicmp(stem.c_str(), L"AUX") == 0 || _wcsicmp(stem.c_str(), L"NUL") == 0 || numberedDevice)
        name.insert(name.begin(), L'_');
    wchar_t stamp[32];
    swprintf_s(stamp, L" %04u-%02u-%02u %02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return name + stamp + L".mp4";
}

std::filesystem::path nextFreePath(const std::filesystem::path& path) {
    // A failed check (error set) must not look like "free": exists() returns false then.
    const auto taken = [](const std::filesystem::path& candidate) {
        std::error_code error;
        const bool exists = std::filesystem::exists(candidate, error);
        return exists || error;
    };
    if (!taken(path)) return path;
    auto stem = path.stem().wstring();
    // Our own " (N)" is replaced, so a retry never stacks suffixes.
    if (stem.size() >= 4 && stem.back() == L')') {
        const auto open = stem.rfind(L" (");
        if (open != std::wstring::npos && open + 3 < stem.size() &&
            std::all_of(stem.begin() + open + 2, stem.end() - 1, [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
            stem.resize(open);
    }
    const auto extension = path.extension().wstring();
    for (int n = 2; n < 1000; ++n) {
        auto candidate = path.parent_path() / (stem + L" (" + std::to_wstring(n) + L")" + extension);
        if (!taken(candidate)) return candidate;
    }
    return {};
}

std::filesystem::path nextClipPath(const std::filesystem::path& folder, std::wstring_view game, std::wstring_view extension) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const auto base = std::filesystem::path(clipFileName(game, now)).replace_extension(extension);
    return nextFreePath(folder / base);
}

std::filesystem::path normalisedFolder(const std::filesystem::path& folder) {
    auto normal = folder.lexically_normal();
    // "D:\Clips\" normalises to itself; only a root keeps its trailing separator.
    if (!normal.empty() && !normal.has_filename() && normal != normal.root_path()) normal = normal.parent_path();
    return normal;
}

} // namespace refarter::capture
