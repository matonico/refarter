#pragma once
#include "video_settings.h"

namespace refarter::capture {
// A restored window can report its full content before the capture pool has
// replaced its minimised-size texture. Wait for a surface that contains it.
inline bool usableStartupFrame(bool minimised, Size client, Size content, Size texture) {
    return !minimised && client.width >= 2 && client.height >= 2 &&
        content.width >= client.width && content.height >= client.height &&
        texture.width >= content.width && texture.height >= content.height;
}
} // namespace refarter::capture
