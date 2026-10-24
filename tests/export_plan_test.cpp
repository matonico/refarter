#include "player/export_plan.h"
#include <iostream>
#include <limits>
int main() {
    using namespace refarter::player;
    int failures = 0;
    const auto check = [&](bool pass, const char* name) { if (!pass) { std::cerr << name << '\n'; ++failures; } };
    check(!planExport(2560, 1440, 0, true), "unknown duration rejected");
    check(!planExport(2560, 1440, std::numeric_limits<double>::infinity(), true), "invalid duration rejected");
    check(!planExport(2560, 1440, 3600, true), "long clips cannot bypass size budget with a bitrate floor");
    const auto shortClip = planExport(2560, 1440, 38, true);
    check(shortClip && shortClip->width == 2560 && !shortClip->thirtyFps, "short clip keeps native size and timestamps");
    const auto longClip = planExport(2560, 1440, 121, true);
    check(longClip && longClip->width == 854 && longClip->height == 480 && longClip->thirtyFps, "long clip trades resolution and fps for size");
    const auto small = planExport(640, 361, 121, false);
    check(small && small->width == 640 && small->height == 360, "even dimensions without upscaling");
    const auto noAudio = planExport(2560, 1440, 121, false);
    check(longClip && noAudio && noAudio->bitrate > longClip->bitrate, "no audio leaves more room for video");
    check(exportPath(L"C:/clips/apex.mp4").filename() == L"20mb_apex.mp4", "prefix naming");
    check(exportPath(L"C:/clips/apex.mp4", 2).filename() == L"20mb_apex (2).mp4", "collision suffix");
    return failures ? 1 : 0;
}
