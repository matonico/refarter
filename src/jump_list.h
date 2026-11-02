#pragma once

#include <filesystem>

namespace refarter {

// The jump list of Refarter's taskbar button and Start entry: launchCommands (startup.h) as tasks, each starting
// `executable` with its flag. Written once per start; needs COM on the calling thread. False when Windows refused it.
bool registerJumpList(const std::filesystem::path& executable);

} // namespace refarter
