#pragma once

#include "video_settings.h"

#include <windows.h>
#include <string>
#include <vector>

namespace refarter::capture {

struct CodecSupport {
    Codec codec = Codec::H264;
    bool records = false;  // The GPU encoded a short clip and Windows wrote and reopened it.
    bool plays = false;    // Media Foundation decoded a test frame; not a test of third-party players.
    std::wstring encoder;  // Hardware encoder name, when one opened.
    std::wstring problem;  // Why recording is unavailable.
};

// Checks each codec end to end on the GPU driving `monitor`: opens the hardware
// encoder exactly as recording would, encodes a few blank frames, writes them
// as an MP4 and reads it back. Takes up to a second or so; call it from a
// worker, only while replay is off, and cache the result.
std::vector<CodecSupport> probeCodecs(HMONITOR monitor);

} // namespace refarter::capture
