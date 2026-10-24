#include "startup.h"
#include <iostream>

int main() {
    using namespace refarter;
    int failures = 0;
    const auto check = [&](bool value, const char* message) { if (!value) { ++failures; std::cerr << message << '\n'; } };
    const auto command = startupCommand(L"C:\\Clips ü\\Refarter.exe");
    check(command && *command == L"\"C:\\Clips ü\\Refarter.exe\" --startup", "quoted Unicode executable path");
    check(!startupCommand(L"relative\\Refarter.exe"), "relative path rejected");
    check(!startupCommand(L"C:\\bad\"name.exe"), "quote injection rejected");
    check(!startupCommand(std::wstring(L"C:\\") + std::wstring(260, L'a')), "Run command length limited");
    check(command && startupLaunch(*command), "startup argument parsed");
    check(!startupLaunch(L"Refarter.exe") && !startupLaunch(L"Refarter.exe --startup extra") &&
          !startupLaunch(L"Refarter.exe --startup-other"), "ordinary and unknown launches stay visible");
    check(quitLaunch(L"Refarter.exe --quit") && !quitLaunch(L"Refarter.exe --startup"), "quit distinct from startup");

    // Exercise real registry operations in an isolated key, never the live Run key.
    const auto key = L"Software\\RefarterTests\\startup-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HKEY opened = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &opened, &disposition) != ERROR_SUCCESS)
        return 1;
    RegCloseKey(opened);
    if (disposition != REG_CREATED_NEW_KEY) return 1;
    check(readStartup(key.c_str()).command.empty(), "fresh registration off");
    check(writeStartup(command, key.c_str()) == ERROR_SUCCESS, "registration succeeds");
    check(readStartup(key.c_str()).command == *command, "registration read back exactly");
    check(writeStartup(startupCommand(L"D:\\Refarter.exe"), key.c_str()) == ERROR_SUCCESS &&
          readStartup(key.c_str()).command == L"\"D:\\Refarter.exe\" --startup", "path replaced without duplicate entry");
    check(writeStartup(std::wstring(261, L'a'), key.c_str()) == ERROR_INVALID_PARAMETER, "invalid update rejected");
    check(writeStartup(std::nullopt, key.c_str()) == ERROR_SUCCESS && readStartup(key.c_str()).command.empty(), "disable removes registration");
    check(writeStartup(std::nullopt, key.c_str()) == ERROR_SUCCESS, "disable idempotent");
    RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str());
    return failures ? 1 : 0;
}
