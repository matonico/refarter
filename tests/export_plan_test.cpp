#include "player/export_plan.h"
#include <cmath>
#include <iostream>
#include <limits>
int main() {
    using namespace refarter::player;
    int failures = 0;
    const auto check = [&](bool pass, const char* name) { if (!pass) { std::cerr << name << '\n'; ++failures; } };
    check(!planExport(2560, 1440, 0, true), "unknown duration rejected");
    check(!planExport(2560, 1440, std::numeric_limits<double>::infinity(), true), "invalid duration rejected");
    check(!planExport(2560, 1440, 3600, true), "long clips cannot bypass size budget with a bitrate floor");
    const auto shortClip = planExport(2560, 1440, 10, true);
    check(shortClip && shortClip->width == 2560 && shortClip->height == 1440, "short clip keeps native size");
    const auto midClip = planExport(2560, 1440, 38, true);
    check(midClip && midClip->width == 1920 && midClip->height == 1080, "mid-length clip drops to 1080p");
    const auto longClip = planExport(2560, 1440, 121, true);
    check(longClip && longClip->width == 1280 && longClip->height == 720, "long clip drops to 720p");
    const auto fullHd = planExport(1920, 1080, 121, true);
    check(fullHd && fullHd->width == 1280 && fullHd->height == 720, "1080p source reaches exactly 720p");
    const auto fourK = planExport(3840, 2160, 10, true);
    check(fourK && fourK->width == 2560 && fourK->height == 1440, "4K never stays native within 20 Mb/s");
    const auto ultrawide = planExport(3440, 1440, 30, true);
    check(ultrawide && ultrawide->width * ultrawide->height <= 1920 * 1080 && ultrawide->width > 1920 &&
          std::abs(static_cast<double>(ultrawide->width) / ultrawide->height - 3440.0 / 1440) < 0.01, "ultrawide keeps its shape at the 1080p pixel count");
    const auto fastSource = planExport(2560, 1440, 60, true, firstBudget, 144);
    check(fastSource && fastSource->fps == 60 && fastSource->width == 1280, "144 fps exports at 60 and keeps the 60 fps tier");
    const auto slowSource = planExport(2560, 1440, 60, true, firstBudget, 30);
    check(slowSource && slowSource->fps == 30 && slowSource->width == 1920, "30 fps gets the tier of twice its rate");
    check(fastSource && fastSource->reduceFps, "144 fps drops pictures");
    const auto jittery = planExport(2560, 1440, 60, true, firstBudget, 61);
    check(jittery && jittery->fps == 61 && !jittery->reduceFps && jittery->width == 1280, "a 60 fps capture averaging 61 keeps every picture");
    const auto at63 = planExport(2560, 1440, 60, true, firstBudget, 63), at64 = planExport(2560, 1440, 60, true, firstBudget, 64);
    check(at63 && at64 && !at63->reduceFps && at64->reduceFps, "frame rate is reduced only above 63");
    const auto misreported = planExport(2560, 1440, 60, true, firstBudget, 5);
    check(misreported && !misreported->reduceFps && misreported->width == 1920, "a misreported low rate counts as 30 for the tier");
    const auto veryLong = planExport(2560, 1440, 300, true);
    check(veryLong && veryLong->width == 852 && veryLong->height == 480 && veryLong->fps == 60, "very long clip drops to 480p at 60 fps");
    check(!planExport(2560, 1440, 950, true), "below 160 kb/s is refused");
    const auto small = planExport(640, 361, 121, false);
    check(small && small->width == 640 && small->height == 360, "even dimensions without upscaling");
    const auto noAudio = planExport(2560, 1440, 121, false);
    check(longClip && noAudio && noAudio->bitrate > longClip->bitrate, "no audio leaves more room for video");
    check(exportPath(L"C:/clips/apex.mp4").filename() == L"20mb_apex.mp4", "prefix naming");
    check(exportPath(L"C:/clips/apex.mp4", 2).filename() == L"20mb_apex (2).mp4", "collision suffix");
    return failures ? 1 : 0;
}
