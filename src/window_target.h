#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

#include "recording_controller.h"

namespace refarter {

// A visible, titled top-level window of another program, with its process held
// open so PID reuse cannot redirect capture. Shared by the picker, auto-capture and settings loading.
std::optional<RecordingTarget> windowTarget(HWND window);

std::vector<HMONITOR> displayMonitors();

// "\.\DISPLAY1": unlike HMONITOR, it names the same display after a restart.
std::wstring monitorDevice(HMONITOR monitor);

} // namespace refarter
