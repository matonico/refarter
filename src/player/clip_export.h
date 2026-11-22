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
    std::wstring cleanupWarning; // A valid result exists, but its owned intermediate could not be removed (or, for a
                                 // trim that already fits, renamed as compressed).
    bool overLimit = false;      // Combine only: saved past the folder's storage limit, as the setting allows.
};
// Synchronous worker entry point. Never call on the UI or recording worker.
// One stereo AAC mix, hardware H.264, original timestamps; above 63 fps, the first picture in each 1/60 s.
ExportResult compressClip(const std::filesystem::path& source, std::atomic<float>& progress,
                          const std::atomic<bool>& cancel);
} // namespace refarter::player
