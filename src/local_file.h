#pragma once

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

// Files kept under %LOCALAPPDATA%\Refarter, and the safe way to read and replace the UTF-16 ones.
namespace refarter {

// %LOCALAPPDATA%\Refarter\<name>; empty when the folder cannot be found.
inline std::filesystem::path localAppDataFile(const wchar_t* name) {
    PWSTR local = nullptr;
    std::filesystem::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local)))
        path = std::filesystem::path(local) / L"Refarter" / name;
    CoTaskMemFree(local);
    return path;
}

enum class Utf16Read { Missing, Ok, Unreadable };

// Reads a UTF-16 file that starts with a byte-order mark. A missing or zero-byte file is not an error (Missing: it
// holds nothing); a file that is too large, odd-sized or lacks the mark is Unreadable, and callers must then never overwrite it.
inline Utf16Read readUtf16File(const std::filesystem::path& file, std::uintmax_t maxBytes, std::wstring& text) {
    if (file.empty()) return Utf16Read::Unreadable;
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error) return Utf16Read::Unreadable;
    if (!exists) return Utf16Read::Missing;
    const auto bytes = std::filesystem::file_size(file, error);
    if (!error && bytes == 0) return Utf16Read::Missing; // Empty (an interrupted first write, a truncating tool): nothing to lose, so it is rewritten.
    if (error || bytes < 2 || bytes > maxBytes || bytes % 2) return Utf16Read::Unreadable;
    std::ifstream input(file, std::ios::binary);
    unsigned char bom[2]{};
    input.read(reinterpret_cast<char*>(bom), 2);
    if (bom[0] != 0xff || bom[1] != 0xfe) return Utf16Read::Unreadable;
    text.assign(static_cast<std::size_t>((bytes - 2) / sizeof(wchar_t)), L'\0');
    input.read(reinterpret_cast<char*>(text.data()), static_cast<std::streamsize>(bytes - 2));
    return input ? Utf16Read::Ok : Utf16Read::Unreadable;
}

// Writes `text` as UTF-16 with a mark to a temporary file beside `file`, then swaps it in, so a crash never
// leaves a half-written file. `tempPrefix` names the temporary file (".favourites-").
inline bool writeUtf16FileAtomic(const std::filesystem::path& file, const std::wstring& text, std::uintmax_t maxBytes,
                                 const wchar_t* tempPrefix) {
    if (file.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error) return false;
    if (text.size() * sizeof(wchar_t) + 2 > maxBytes) return false;
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) return false;
    wchar_t unique[40]{};
    StringFromGUID2(id, unique, static_cast<int>(std::size(unique)));
    const auto temporary = file.parent_path() / (std::wstring(tempPrefix) + unique);
    const HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BYTE bom[] = {0xff, 0xfe};
    const DWORD bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    const bool saved = WriteFile(output, bom, 2, &written, nullptr) && written == 2 &&
        WriteFile(output, text.data(), bytes, &written, nullptr) && written == bytes && FlushFileBuffers(output);
    CloseHandle(output);
    const bool replaced = saved && MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!replaced) DeleteFileW(temporary.c_str());
    return replaced;
}

} // namespace refarter
