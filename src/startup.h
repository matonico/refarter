#pragma once
#include <windows.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace refarter {
inline constexpr wchar_t startupKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t startupValue[] = L"Refarter";
inline constexpr wchar_t restoreMessageName[] = L"Refarter.RestoreWindow";
inline constexpr wchar_t quitMessageName[] = L"Refarter.Quit";
struct StartupRegistration { std::wstring command; LSTATUS error = ERROR_SUCCESS; };
std::filesystem::path executablePath();
std::optional<std::wstring> startupCommand(const std::filesystem::path& executable);
StartupRegistration readStartup(const wchar_t* key = startupKey, const wchar_t* value = startupValue);
LSTATUS writeStartup(const std::optional<std::wstring>& command,
                     const wchar_t* key = startupKey, const wchar_t* value = startupValue);
bool startupLaunch(std::wstring_view commandLine);
bool quitLaunch(std::wstring_view commandLine);
} // namespace refarter
