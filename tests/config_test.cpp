#include "config.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <iterator>

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    const auto values = refarter::parseConfig(
        L"; comment\r\n"
        L"orphan = ignored\r\n"
        L"[Shortcuts]\r\n"
        L"  Save_Replay =  ctrl + shift + f10  \r\n"
        L"# another comment\n"
        L"quoted = \"alt + f9\"\n"
        L"no equals sign\n"
        L"empty =\n"
        L"[ video ]\n"
        L"fps=60");
    check(values.size() == 4, "entry count");
    check(values.at(L"shortcuts/save_replay") == L"ctrl + shift + f10", "trimmed, case-insensitive names");
    check(values.at(L"shortcuts/quoted") == L"alt + f9", "quotes dropped");
    check(values.at(L"shortcuts/empty").empty(), "empty value kept");
    check(values.at(L"video/fps") == L"60", "last line without newline");
    check(!values.contains(L"/orphan"), "keys before a section ignored");
    check(refarter::parseConfig(L"").empty(), "empty file");

    // Writing to a hand-written file: only temporary files, never the real settings.
    const auto folder = std::filesystem::temp_directory_path() / (L"refarter-config-test-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(folder);
    const auto write = [](const std::filesystem::path& file, const std::string& bytes) {
        std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
    };
    const auto read = [](const std::filesystem::path& file) {
        std::ifstream in(file, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };

    // An ANSI file keeps its non-ASCII text through the conversion to UTF-16 (the system code page is Windows-1252 here).
    {
        const auto ansi = folder / L"ansi.ini";
        write(ansi, "; Jos\xE9\r\n[saving]\r\nclip_folder = C:\\Clips\\Jos\xE9\r\n");
        refarter::Config config;
        config.loadFrom(ansi);
        if (GetACP() == 1252) check(config.get(L"saving", L"clip_folder") == L"C:\\Clips\\Jos\u00E9", "ANSI text is read with the system code page");
        check(config.set(L"saving", L"other", L"Z\u00FCrich"), "set converts an ANSI file");
        refarter::Config again;
        again.loadFrom(ansi);
        if (GetACP() == 1252) check(again.get(L"saving", L"clip_folder") == L"C:\\Clips\\Jos\u00E9", "ANSI text survives the write");
        check(again.get(L"saving", L"other") == L"Z\u00FCrich", "the new value round trips");
        const auto bytes = read(ansi);
        check(bytes.size() > 2 && bytes[0] == '\xFF' && bytes[1] == '\xFE', "the file is now UTF-16");
        bool onlyOurFile = true;
        for (const auto& entry : std::filesystem::directory_iterator(folder))
            if (entry.path().filename() != L"ansi.ini" && entry.path().filename() != L"utf8.ini" && entry.path().filename() != L"locked.ini") onlyOurFile = false;
        check(onlyOurFile, "no temporary file is left beside the settings");
    }
    // Valid UTF-8 is decoded as UTF-8 (no U+FFFD, no code page guess), with or without a mark.
    {
        const auto utf8 = folder / L"utf8.ini";
        write(utf8, "\xEF\xBB\xBF[saving]\r\nname = Jos\xC3\xA9\r\n");
        refarter::Config config;
        config.loadFrom(utf8);
        check(config.get(L"saving", L"name") == L"Jos\u00E9", "UTF-8 is decoded strictly");
        check(config.set(L"saving", L"name", L"Zo\u00EB") && config.get(L"saving", L"name") == L"Zo\u00EB", "set on a UTF-8 file");
        refarter::Config again;
        again.loadFrom(utf8);
        check(again.get(L"saving", L"name") == L"Zo\u00EB", "UTF-8 file round trips");
    }
    // A file that cannot be read at that moment is left exactly as it was, and the write is reported as failed.
    {
        const auto locked = folder / L"locked.ini";
        const std::string original = "[saving]\r\nname = Jos\xE9\r\nvolume = 80\r\n";
        write(locked, original);
        refarter::Config config;
        config.loadFrom(locked);
        const HANDLE hold = CreateFileW(locked.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(hold != INVALID_HANDLE_VALUE, "test holds the file exclusively");
        if (hold != INVALID_HANDLE_VALUE) {
            check(!config.set(L"saving", L"volume", L"50"), "a failed read fails the write");
            CloseHandle(hold);
            check(read(locked) == original, "a failed read leaves the file untouched");
            check(config.set(L"saving", L"volume", L"50"), "the write works once the file can be read");
        }
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    return failures ? 1 : 0;
}
