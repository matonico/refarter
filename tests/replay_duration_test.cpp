#include "replay_duration.h"
#include <iostream>

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    using refarter::parseReplaySeconds;
    for (const auto invalid : {L"", L"0", L"1", L"4", L"-1", L"12.5", L"abc", L" 60", L"60s", L"3601", L"999999999999"}) {
        check(!parseReplaySeconds(invalid), "invalid duration accepted");
    }
    check(parseReplaySeconds(L"5") == 5, "lower boundary");
    check(parseReplaySeconds(L"3600") == 3600, "upper boundary");
    check(parseReplaySeconds(L"0225") == 225, "leading zero input");
    check(refarter::formatReplayDuration(1) == L"1 s", "sub-minute display");
    check(refarter::formatReplayDuration(45) == L"45 s", "seconds only");
    check(refarter::formatReplayDuration(60) == L"1 min", "minute rollover");
    check(refarter::formatReplayDuration(180) == L"3 min", "whole minutes");
    check(refarter::formatReplayDuration(90) == L"1 min 30 s", "minutes and seconds");
    check(refarter::formatReplayDuration(225) == L"3 min 45 s", "custom conversion");
    check(refarter::formatReplayDuration(3600) == L"60 min", "upper-bound display");
    return failures ? 1 : 0;
}
