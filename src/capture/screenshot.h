#pragma once

#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace refarter::capture {

// The part of a recorded region that lies inside a width x height texture (while a resize settles the region can extend
// past the capture buffer), or nothing when no part does.
inline std::optional<RECT> clampToTexture(const RECT& region, UINT width, UINT height) {
    const RECT bounds{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    RECT clamped{};
    if (!IntersectRect(&clamped, &region, &bounds)) return std::nullopt;
    return clamped;
}

// Writes a 32-bit BGRA picture (alpha ignored) as a 24-bit PNG through WIC, which is part of Windows. The file is
// written as "<path>.partial" first and renamed without replacing anything, as clips are; a failure removes the
// partial file. Returns what went wrong, or nothing. Needs COM on the calling thread (the recorder's MTA is enough).
std::optional<std::wstring> writePng(const std::filesystem::path& path, const std::uint8_t* pixels, UINT width, UINT height, UINT pitch);

} // namespace refarter::capture
