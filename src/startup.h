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
// The jump list's tasks (and Stream Deck, AutoHotkey and the like) start Refarter.exe with one of these flags; the new
// instance hands the command to the running one in this message's WPARAM and exits. No polling in Refarter.
inline constexpr wchar_t commandMessageName[] = L"Refarter.Command";
enum class LaunchCommand : WPARAM { None, SaveReplay, Screenshot, Library };
inline constexpr struct { LaunchCommand command; const wchar_t* flag; const wchar_t* title; } launchCommands[] = {
    {LaunchCommand::SaveReplay, L"--save-replay", L"save replay"},
    {LaunchCommand::Screenshot, L"--screenshot", L"screenshot"},
    {LaunchCommand::Library, L"--library", L"open library"},
};
struct StartupRegistration { std::wstring command; LSTATUS error = ERROR_SUCCESS; };
std::filesystem::path executablePath();
std::optional<std::wstring> startupCommand(const std::filesystem::path& executable);
StartupRegistration readStartup(const wchar_t* key = startupKey, const wchar_t* value = startupValue);
LSTATUS writeStartup(const std::optional<std::wstring>& command,
                     const wchar_t* key = startupKey, const wchar_t* value = startupValue);
bool startupLaunch(std::wstring_view commandLine);
bool quitLaunch(std::wstring_view commandLine);
LaunchCommand launchCommand(std::wstring_view commandLine); // One of launchCommands' flags, alone; otherwise None.
} // namespace refarter
