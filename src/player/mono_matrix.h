#pragma once
// Which mono tracks play at their own level on both channels. Pure, so it is tested without FFmpeg.
#include <string_view>

namespace refarter::player {
// Refarter's own microphone track is the only mono track it writes, stored mono and heard in its stereo mix at the
// level it was recorded. It is told by the handler name Refarter gives it, never by the channel count alone: any
// other mono track (an imported or older clip) keeps swresample's default mapping, 3 dB lower on each side.
inline bool unityMonoMatrix(std::string_view handlerName, int channels) {
    return channels == 1 && handlerName == "microphone";
}
} // namespace refarter::player
