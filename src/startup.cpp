#include "startup.h"
#include <shellapi.h>
#include <vector>

namespace refarter {
std::filesystem::path executablePath() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return length && length < path.size() ? std::filesystem::path(std::wstring(path.data(), length)) : std::filesystem::path{};
}

std::optional<std::wstring> startupCommand(const std::filesystem::path& executable) {
    if (!executable.is_absolute() || executable.wstring().find(L'"') != std::wstring::npos) return std::nullopt;
    auto command = L"\"" + executable.wstring() + L"\" --startup";
    // Documented limit for the Run key's command line, excluding its terminator.
    if (command.size() > 260) return std::nullopt;
    return command;
}

StartupRegistration readStartup(const wchar_t* key, const wchar_t* value) {
    wchar_t command[261]{};
    DWORD bytes = sizeof(command);
    const auto error = RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_SZ,
                                    nullptr, command, &bytes);
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return {};
    if (error != ERROR_SUCCESS) return {{}, error};
    return {command, ERROR_SUCCESS};
}

LSTATUS writeStartup(const std::optional<std::wstring>& command, const wchar_t* key, const wchar_t* value) {
    if (!command) {
        const auto error = RegDeleteKeyValueW(HKEY_CURRENT_USER, key, value);
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ERROR_SUCCESS : error;
    }
    if (command->empty() || command->size() > 260) return ERROR_INVALID_PARAMETER;
    HKEY opened = nullptr;
    auto error = RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &opened, nullptr);
    if (error != ERROR_SUCCESS) return error;
    error = RegSetValueExW(opened, value, 0, REG_SZ, reinterpret_cast<const BYTE*>(command->c_str()),
                          static_cast<DWORD>((command->size() + 1) * sizeof(wchar_t)));
    RegCloseKey(opened);
    return error;
}

static bool launchFlag(std::wstring_view commandLine, std::wstring_view flag) {
    int count = 0;
    const std::wstring owned(commandLine);
    auto arguments = CommandLineToArgvW(owned.c_str(), &count);
    const bool result = arguments && count == 2 && std::wstring_view(arguments[1]) == flag;
    LocalFree(arguments);
    return result;
}
bool startupLaunch(std::wstring_view commandLine) { return launchFlag(commandLine, L"--startup"); }
bool quitLaunch(std::wstring_view commandLine) { return launchFlag(commandLine, L"--quit"); }
} // namespace refarter
