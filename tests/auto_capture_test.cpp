#include "auto_capture.h"
#include <iostream>

using namespace refarter;

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--exit-target") return 0;
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };

    check(!exitedAutoCapture(true, nullptr), "missing process cannot clear a replay");
    check(!exitedAutoCapture(true, GetCurrentProcess()), "live automatic game retains replay");
    check(!exitedAutoCapture(false, GetCurrentProcess()), "live manual game retains replay");
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" --exit-target";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &child)) return 1;
    check(WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0, "test target exits");
    check(exitedAutoCapture(true, child.hProcess), "any automatic game clears on confirmed process exit");
    check(!exitedAutoCapture(false, child.hProcess), "exited manual game retains replay");
    check(!exitedAutoCapture(true, GetCurrentProcess()), "new live instance is independent of the exited instance");
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);

    AutoCaptureList list;
    list.add(L"C:\\Games\\Apex\\r5apex.exe", L"apex legends");
    list.add(L"c:\\games\\apex\\R5APEX.exe", L"apex"); // Same game: label replaced, no duplicate.
    check(list.games.size() == 1 && list.games[0].label == L"apex", "paths compare case-insensitively");
    check(list.find(L"C:\\GAMES\\APEX\\r5apex.exe") != nullptr, "find ignores case");
    list.decline(L"C:\\Tools\\Viewer\\viewer.exe");
    check(list.isDeclined(L"c:\\tools\\viewer\\viewer.exe"), "declines are remembered");
    list.add(L"C:\\Tools\\Viewer\\viewer.exe", L"viewer");
    check(!list.isDeclined(L"C:\\Tools\\Viewer\\viewer.exe") && list.games.size() == 2, "adding forgets an earlier decline");
    list.decline(L"C:\\Tools\\Viewer\\viewer.exe");
    check(list.games.size() == 1, "declining removes it from the list");
    list.add(L"D:\\Steam\\Weird \"Quoted\" Game\\game.exe", L"label with \"quotes\"\nand a newline");
    check(list.games.back().label.find(L'\n') == std::wstring::npos, "labels stay on one line");

    const auto text = formatAutoCapture(list);
    const auto parsed = parseAutoCapture(text);
    check(parsed && parsed->games.size() == list.games.size() && parsed->declined == list.declined &&
          parsed->games.back().path == list.games.back().path && parsed->games.back().label == list.games.back().label,
          "round trip keeps quotes, labels and declines");
    check(!parseAutoCapture(L"something else\n"), "wrong header is an error");
    check(!parseAutoCapture(L"refarter auto-capture 1\ngame \"relative\\game.exe\" \"x\"\n"), "relative paths are an error");
    check(!parseAutoCapture(L"refarter auto-capture 1\nplay \"C:\\a.exe\"\n"), "unknown records are an error");
    check(!parseAutoCapture(L"refarter auto-capture 1\ngame \"C:\\a.exe\"\n"), "a game needs a label");
    check(parseAutoCapture(L"refarter auto-capture 1\n") && parseAutoCapture(L"refarter auto-capture 1\n")->games.empty(), "empty list");

    wchar_t folder[MAX_PATH]{}, temporary[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, folder) || !GetTempFileNameW(folder, L"rac", 0, temporary)) return 1;
    std::filesystem::remove(temporary);
    check(readAutoCapture(temporary) && readAutoCapture(temporary)->games.empty(), "missing file is an empty list");
    check(writeAutoCapture(temporary, list), "list is written");
    const auto read = readAutoCapture(temporary);
    check(read && formatAutoCapture(*read) == text, "file round trip");
    std::filesystem::remove(temporary);

    check(!offerable(L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe"), "browsers are never offered");
    check(!offerable(L"D:\\Repos\\Refarter\\build\\Release\\Refarter.exe"), "refarter is never offered");
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    check(!offerable(std::filesystem::path(windows) / L"explorer.exe"), "windows programs are never offered");
    check(!offerable(L"C:\\Program Files\\WindowsApps\\Microsoft.ScreenSketch_11.2409.25.0_x64__8wekyb3d8bbwe\\SnippingTool\\SnippingTool.exe"),
          "the snipping tool overlay is never offered");
    check(!offerable(L"C:\\Program Files\\PowerToys\\PowerToys.PowerOCR.exe"), "powertoys overlays are never offered");
    check(!offerable(L"C:\\Program Files (x86)\\Steam\\steamwebhelper.exe"), "steam big picture is not a game");
    check(offerable(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Apex Legends\\r5apex.exe"), "games are offered");
    check(!appWindowStyle(WS_EX_TOOLWINDOW | WS_EX_TOPMOST) && !appWindowStyle(WS_EX_NOACTIVATE | WS_EX_LAYERED),
          "tool and non-activating overlay windows are not offered");
    check(appWindowStyle(WS_EX_TOPMOST | WS_EX_APPWINDOW), "topmost fullscreen game windows are");
    check(offerable(L"C:\\WindowsGames\\game.exe"), "only the windows folder itself is excluded");

    const RECT monitor{0, 0, 2560, 1440};
    check(coversMonitor({0, 0, 2560, 1440}, monitor), "borderless fullscreen covers the monitor");
    check(coversMonitor({-8, -8, 2568, 1448}, monitor), "a frame beyond the edges still covers it");
    check(!coversMonitor({-8, -8, 2568, 1400}, monitor), "maximised above the taskbar does not");
    check(!coversMonitor({0, 0, 1920, 1080}, monitor), "a smaller window does not");
    check(!coversMonitor({2560, 0, 5120, 1440}, monitor), "a window on another monitor does not");
    return failures ? 1 : 0;
}
