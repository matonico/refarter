#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace refarter::player {

// Refarter clips store a "mix" track followed by the sources it combines
// (desktop, microphone, discord), and a game window's clips add a "game" track:
// the game's own sound, which desktop audio already contains, so it is never
// summed with desktop. The player offers those sources as switches
// and plays the stored mix while all of them are on: one decoder, as before.
// A clip with the sources but no mix (a desktop and game clip, say) plays them summed instead.
// Other files offer a plain choice of one track.
struct AudioSources {
    int mix = -1;                     // Track index of the stored mix; -1 if none.
    std::vector<int> tracks;          // Track index of each source.
    std::vector<std::wstring> names;  // Their names, in file order.
    // Two or more known sources get switches, with or without a stored mix (without one, the sources are summed).
    bool switchable() const { return tracks.size() >= 2; }
};

inline AudioSources audioSources(const std::vector<std::wstring>& trackNames) {
    AudioSources found;
    for (std::size_t i = 0; i < trackNames.size(); ++i) {
        const auto& name = trackNames[i];
        if (name == L"mix" && found.mix < 0) found.mix = static_cast<int>(i);
        else if (name == L"desktop" || name == L"microphone" || name == L"discord" || name == L"game" || name == L"game output") {
            found.tracks.push_back(static_cast<int>(i));
            found.names.push_back(name);
        }
    }
    return found;
}

// The switches as they sound: the game is part of desktop audio, so while desktop is on the game is on too.
// Turn desktop off to hear the game on its own.
inline std::vector<bool> effectiveOn(const AudioSources& sources, std::vector<bool> on) {
    on.resize(sources.names.size(), false);
    const auto desktop = std::find(sources.names.begin(), sources.names.end(), L"desktop");
    const auto game = std::find(sources.names.begin(), sources.names.end(), L"game");
    if (desktop != sources.names.end() && game != sources.names.end() && on[static_cast<std::size_t>(desktop - sources.names.begin())])
        on[static_cast<std::size_t>(game - sources.names.begin())] = true;
    return on;
}

// The sources whose switch cannot be flipped: the game while desktop is on (desktop audio already contains it).
inline std::vector<bool> includedIn(const AudioSources& sources, std::vector<bool> on) {
    on.resize(sources.names.size(), false);
    const auto desktop = std::find(sources.names.begin(), sources.names.end(), L"desktop");
    std::vector<bool> included(sources.names.size(), false);
    if (desktop == sources.names.end() || !on[static_cast<std::size_t>(desktop - sources.names.begin())]) return included;
    for (std::size_t i = 0; i < sources.names.size(); ++i) included[i] = sources.names[i] == L"game";
    return included;
}

// Tracks to decode for the switched-on sources: the stored mix when all are on (without a mix, the sources summed). The game track is left out
// while desktop is on (it would play twice); a trim keeps it (trimKeepFor).
inline std::vector<int> tracksFor(const AudioSources& sources, const std::vector<bool>& switches) {
    const auto on = effectiveOn(sources, switches);
    const auto desktop = std::find(sources.names.begin(), sources.names.end(), L"desktop");
    const bool desktopOn = desktop != sources.names.end() && on[static_cast<std::size_t>(desktop - sources.names.begin())];
    std::vector<int> tracks;
    for (std::size_t i = 0; i < sources.tracks.size(); ++i) {
        if (!on[i] || (desktopOn && sources.names[i] == L"game")) continue;
        tracks.push_back(sources.tracks[i]);
    }
    if (std::find(on.begin(), on.end(), false) == on.end() && sources.mix >= 0) return {sources.mix};
    return tracks;
}

// Tracks a trim keeps: every switched-on source, the separate game track included even while desktop is on
// (only playback and the mix leave it out, because desktop audio already holds it).
inline std::vector<int> trimKeepFor(const AudioSources& sources, const std::vector<bool>& switches) {
    const auto on = effectiveOn(sources, switches);
    std::vector<int> tracks;
    for (std::size_t i = 0; i < sources.tracks.size(); ++i)
        if (on[i]) tracks.push_back(sources.tracks[i]);
    return tracks;
}

// Of those, the tracks a new mix must not sum: the game track while desktop is on.
inline std::vector<int> trimApartFor(const AudioSources& sources, const std::vector<bool>& switches) {
    const auto kept = trimKeepFor(sources, switches);
    const auto playing = tracksFor(sources, switches);
    std::vector<int> apart;
    for (const auto track : kept)
        if (std::find(playing.begin(), playing.end(), track) == playing.end()) apart.push_back(track);
    return apart;
}

// Every source is switched off: the clip plays silently and a trim keeps no audio track at all
// (an empty keep list would mean "every track").
inline bool allOff(const AudioSources& sources, const std::vector<bool>& switches) {
    const auto on = effectiveOn(sources, switches);
    return !on.empty() && std::find(on.begin(), on.end(), true) == on.end();
}

// Pill text: "mix", one name, "no discord", or the names joined with " + ".
inline std::wstring sourcesLabel(const AudioSources& sources, const std::vector<bool>& switches) {
    const auto on = effectiveOn(sources, switches);
    std::vector<std::wstring> playing, silent;
    for (std::size_t i = 0; i < sources.names.size(); ++i)
        (on[i] ? playing : silent).push_back(sources.names[i]);
    if (silent.empty()) return L"mix";
    if (playing.empty()) return L"off";
    if (playing.size() > 1 && silent.size() == 1) return L"no " + silent.front();
    std::wstring label;
    for (const auto& name : playing) label += (label.empty() ? L"" : L" + ") + name;
    return label;
}

} // namespace refarter::player
