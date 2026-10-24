#include "auto_capture.h"
#include "local_file.h"
#include "text.h"

#include <shlobj.h>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace refarter {
namespace {
constexpr const wchar_t* header = L"refarter auto-capture 1";
constexpr std::uintmax_t maxBytes = 1024 * 1024;

bool same(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

bool clean(const std::wstring& text) { return text.find_first_of(L"\r\n\0", 0, 3) == std::wstring::npos; }
} // namespace

bool exitedAutoCapture(bool automatic, HANDLE process) {
    return automatic && process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
}

const AutoCaptureGame* AutoCaptureList::find(const std::wstring& path) const {
    const auto found = std::find_if(games.begin(), games.end(), [&](const auto& game) { return same(game.path, path); });
    return found == games.end() ? nullptr : &*found;
}

bool AutoCaptureList::isDeclined(const std::wstring& path) const {
    return std::any_of(declined.begin(), declined.end(), [&](const auto& entry) { return same(entry, path); });
}

void AutoCaptureList::add(std::wstring path, std::wstring label) {
    std::erase_if(declined, [&](const auto& entry) { return same(entry, path); });
    for (auto& c : label) if (c == L'\r' || c == L'\n') c = L' ';
    for (auto& game : games) {
        if (!same(game.path, path)) continue;
        game.label = std::move(label);
        return;
    }
    games.push_back({std::move(path), std::move(label)});
}

void AutoCaptureList::remove(const std::wstring& path) {
    std::erase_if(games, [&](const auto& game) { return same(game.path, path); });
}

void AutoCaptureList::decline(std::wstring path) {
    remove(path);
    if (!isDeclined(path)) declined.push_back(std::move(path));
}

std::filesystem::path autoCapturePath() { return localAppDataFile(L"auto_capture.dat"); }

std::wstring formatAutoCapture(const AutoCaptureList& list) {
    std::wostringstream text;
    text << header << L'\n';
    for (const auto& game : list.games) text << L"game " << std::quoted(game.path) << L' ' << std::quoted(game.label) << L'\n';
    for (const auto& path : list.declined) text << L"declined " << std::quoted(path) << L'\n';
    return text.str();
}

std::optional<AutoCaptureList> parseAutoCapture(const std::wstring& text) {
    std::wistringstream lines(text);
    std::wstring line;
    if (!std::getline(lines, line) || line != header) return std::nullopt;
    AutoCaptureList list;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        std::wistringstream record(line);
        std::wstring kind, path, label;
        if (!(record >> kind >> std::quoted(path))) return std::nullopt;
        if (kind == L"game" && !(record >> std::quoted(label))) return std::nullopt;
        if (kind != L"game" && kind != L"declined") return std::nullopt;
        record >> std::ws;
        if (!record.eof() || !std::filesystem::path(path).is_absolute() || !clean(path) || !clean(label)) return std::nullopt;
        if (kind == L"game") list.add(std::move(path), std::move(label));
        else list.decline(std::move(path));
    }
    return list;
}

std::optional<AutoCaptureList> readAutoCapture(const std::filesystem::path& file) {
    std::wstring text;
    switch (readUtf16File(file, maxBytes, text)) {
    case Utf16Read::Missing: return AutoCaptureList{};
    case Utf16Read::Ok: return parseAutoCapture(text);
    default: return std::nullopt;
    }
}

bool writeAutoCapture(const std::filesystem::path& file, const AutoCaptureList& list) {
    return writeUtf16FileAtomic(file, formatAutoCapture(list), maxBytes, L".auto-capture-");
}

bool offerable(const std::filesystem::path& executable) {
    // Browsers and players go fullscreen for videos, screenshot tools and overlays
    // cover the screen for a moment; none of them are games. Refarter never captures itself.
    static const wchar_t* const never[] = {
        L"refarter.exe", L"refarter-record.exe",
        L"chrome.exe", L"msedge.exe", L"firefox.exe", L"opera.exe", L"brave.exe", L"vivaldi.exe", L"iexplore.exe",
        L"arc.exe", L"librewolf.exe", L"waterfox.exe", L"floorp.exe", L"zen.exe", L"thorium.exe",
        L"vlc.exe", L"mpv.exe", L"mpc-hc.exe", L"mpc-hc64.exe", L"mpc-be64.exe", L"potplayermini64.exe",
        L"wmplayer.exe", L"microsoft.media.player.exe", L"video.ui.exe", L"powerpnt.exe",
        L"snippingtool.exe", L"screenclippinghost.exe", L"screensketch.exe", L"gamebar.exe", L"gamebarftserver.exe",
        L"sharex.exe", L"greenshot.exe", L"lightshot.exe", L"obs64.exe", L"obs32.exe",
        L"steam.exe", L"steamwebhelper.exe", L"discord.exe", L"nvidia overlay.exe", L"nvidia share.exe"};
    const auto name = lower(executable.filename().wstring());
    if (std::any_of(std::begin(never), std::end(never), [&](const wchar_t* entry) { return name == entry; })) return false;
    if (name.starts_with(L"powertoys")) return false; // PowerToys.<Module>.exe overlays and pickers.
    // Windows' own programs (Explorer, lock screen, Start, overlays) live under the Windows folder.
    wchar_t windows[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return true;
    const auto folder = lower(std::filesystem::path(windows).lexically_normal().wstring()) + L"\\";
    return !lower(executable.lexically_normal().wstring()).starts_with(folder);
}

bool appWindowStyle(LONG_PTR extendedStyle) {
    return (extendedStyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) == 0;
}

bool coversMonitor(const RECT& window, const RECT& monitor) {
    return window.left <= monitor.left && window.top <= monitor.top &&
           window.right >= monitor.right && window.bottom >= monitor.bottom;
}

} // namespace refarter
