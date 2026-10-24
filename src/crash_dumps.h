#pragma once

#include <filesystem>
#include <string>

namespace refarter {

// At most this many dumps stay in the folder: each start trims it to one fewer, so the next crash fits.
inline constexpr int keptCrashDumps = 10;

// On a crash, writes a minidump to `folder` as "<name> <date time>.dmp" (local time), then lets
// Windows handle the crash as before (Windows Error Reporting, the Application event log). Covers
// unhandled exceptions, uncaught C++ exceptions, abort and std::terminate, pure virtual calls and
// invalid CRT parameters; fail-fast ends (buffer overruns, heap corruption) skip it. Nothing is
// uploaded. One idle thread waits for a crash; dbghelp is loaded only when one happens. Call once,
// early, on the main thread. False when the folder's path is too long or the thread could not start (the
// process then crashes as before).
bool installCrashDumps(const std::filesystem::path& folder, const std::wstring& name);

} // namespace refarter
