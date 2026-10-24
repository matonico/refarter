#pragma once
#include <atomic>
#include <filesystem>
#include <string>

namespace refarter::player {
struct ExportResult {
    std::filesystem::path path;
    std::wstring error;
    bool cancelled = false;
    bool alreadyFits = false;
    std::wstring cleanupWarning; // A valid export exists, but its owned intermediate could not be removed.
};
// Synchronous worker entry point. Never call on the UI or recording worker.
// One stereo AAC mix, hardware H.264, original timestamps unless reducing to ~30 fps.
ExportResult compressClip(const std::filesystem::path& source, std::atomic<float>& progress,
                          const std::atomic<bool>& cancel);
} // namespace refarter::player
