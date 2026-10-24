#include "config.h"
#include "resources.h"
#include "local_file.h"
#include "text.h"

#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <iterator>
#include <system_error>

namespace refarter {
namespace {
std::wstring key(std::wstring_view section, std::wstring_view name) {
    std::wstring result;
    result += lower(section);
    result += L'/';
    result += lower(name);
    return result;
}

// UTF-16 when the bytes start with its mark (as WritePrivateProfileString
// keeps such a file), otherwise UTF-8 with or without a mark. Bytes that are not valid UTF-8 are a hand-written
// ANSI file, read with the system code page (never turned into U+FFFD).
std::wstring decode(std::string_view bytes, bool& utf16) {
    utf16 = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE;
    if (utf16) return std::wstring(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / sizeof(wchar_t));
    if (bytes.starts_with("\xEF\xBB\xBF")) bytes.remove_prefix(3);
    if (bytes.empty()) return {};
    const int length = static_cast<int>(bytes.size());
    for (const UINT page : {static_cast<UINT>(CP_UTF8), static_cast<UINT>(CP_ACP)}) {
        const DWORD flags = page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
        const int size = MultiByteToWideChar(page, flags, bytes.data(), length, nullptr, 0);
        if (size <= 0) continue;
        std::wstring text(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(page, flags, bytes.data(), length, text.data(), size);
        return text;
    }
    return {};
}

// The whole file as text, empty when it does not exist. False when it exists but could not be read: the caller must
// then leave the file alone (an unreadable moment is not an empty file).
bool readText(const std::filesystem::path& path, std::wstring& text, bool& utf16) {
    utf16 = false;
    text.clear();
    std::error_code error;
    if (!std::filesystem::exists(path, error)) return !error;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) return false;
    text = decode(bytes, utf16);
    return true;
}

// The system writer keeps an ANSI file ANSI, which would mangle a clip folder under a non-ASCII user name; in a
// UTF-16 file it writes UTF-16. A temporary file and a rename mean a crash never leaves a half-written settings file.
bool writeUtf16(const std::filesystem::path& path, const std::wstring& text) {
    constexpr std::uintmax_t largestSettingsFile = 1 << 20;
    return writeUtf16FileAtomic(path, text, largestSettingsFile, L".refarter-ini-");
}

// config/refarter.ini, embedded so a fresh file carries its comments.
std::wstring defaultText() {
    const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_CONFIG_TEMPLATE), RT_RCDATA);
    const auto loaded = resource ? LoadResource(nullptr, resource) : nullptr;
    if (!loaded) return {};
    bool utf16 = false;
    return decode({static_cast<const char*>(LockResource(loaded)), SizeofResource(nullptr, resource)}, utf16);
}
} // namespace

std::filesystem::path configPath() { return localAppDataFile(L"refarter.ini"); }

void Config::load() { loadFrom(configPath()); }

void Config::loadFrom(std::filesystem::path path) {
    path_ = std::move(path);
    values_.clear();
    utf16_ = false;
    std::error_code error;
    if (!path_.empty() && !std::filesystem::exists(path_, error) && !error) {
        // First launch: write the commented defaults. If that fails, defaults still apply.
        const auto text = defaultText();
        utf16_ = writeUtf16(path_, text);
        values_ = parseConfig(text);
        return;
    }
    // Unreadable now: nothing is known, and set() will not touch the file unless it can read it then.
    std::wstring text;
    if (readText(path_, text, utf16_)) values_ = parseConfig(text);
}

std::wstring Config::get(std::wstring_view section, std::wstring_view name, std::wstring fallback) const {
    const auto found = values_.find(key(section, name));
    return found == values_.end() ? fallback : found->second;
}

bool Config::set(std::wstring_view section, std::wstring_view name, const std::wstring& value) {
    values_[key(section, name)] = value;
    if (path_.empty()) return false;
    if (!utf16_) {
        // A hand-written UTF-8 or ANSI file: convert once before the first write, and only from a successful read.
        std::wstring text;
        bool ignored = false;
        if (!readText(path_, text, ignored) || !writeUtf16(path_, text)) return false;
        utf16_ = true;
    }
    // Rare and user-initiated; the system writer keeps comments and the other lines intact.
    return WritePrivateProfileStringW(std::wstring(section).c_str(), std::wstring(name).c_str(),
        value.c_str(), path_.c_str()) != FALSE;
}

} // namespace refarter
