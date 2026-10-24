#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace refarter::capture {

struct AudioLevels {
    int desktopPercent = 100;
    int microphonePercent = 100;
};

inline std::optional<std::wstring> validate(const AudioLevels& levels) {
    if (levels.desktopPercent < 0 || levels.desktopPercent > 200 ||
        levels.microphonePercent < 0 || levels.microphonePercent > 200)
        return L"audio volume must be 0-200%";
    return std::nullopt;
}

// Save-time gain, after mic filters and before mixing. 100% is bit-exact and
// costs no sample pass; mute keeps the track's duration and channel layout.
inline void applyAudioLevel(std::span<std::int16_t> samples, int percent) {
    percent = std::clamp(percent, 0, 200);
    if (percent == 100) return;
    if (percent == 0) { std::fill(samples.begin(), samples.end(), std::int16_t{0}); return; }
    for (auto& sample : samples) {
        const int scaled = static_cast<int>(sample) * percent / 100;
        sample = static_cast<std::int16_t>(std::clamp(scaled, -32768, 32767));
    }
}

// Live level meters. A capture thread feeds the packets it records into a
// LevelMeter (one relaxed atomic, no lock, no allocation); the interface takes
// the peak since it last looked, applies the volume setting (what is recorded)
// and smooths it with a MeterBar. The interface switches a meter on only while a
// bar is actually on screen (want); a capture thread then skips even the peak
// scan, so a hidden recording pays nothing for the meters.
class LevelMeter {
public:
    void want(bool wanted) noexcept { wanted_.store(wanted, std::memory_order_relaxed); }
    bool wanted() const noexcept { return wanted_.load(std::memory_order_relaxed); }
    // Interleaved 16-bit samples as captured, before the volume setting.
    void push(const std::int16_t* samples, std::size_t count) noexcept {
        int peak = 0;
        for (std::size_t i = 0; i < count; ++i) peak = std::max(peak, std::abs(static_cast<int>(samples[i])));
        int seen = peak_.load(std::memory_order_relaxed);
        while (peak > seen && !peak_.compare_exchange_weak(seen, peak, std::memory_order_relaxed)) {}
    }
    // The largest sample (0..32768) since the previous call; 0 after silence.
    int take() noexcept { return peak_.exchange(0, std::memory_order_relaxed); }

private:
    std::atomic<int> peak_ = 0;
    std::atomic<bool> wanted_ = false;
};

inline constexpr float meterFloorDb = -60.f; // An empty bar.
inline constexpr float meterClipDb = -3.f;   // From here the bar turns to the warning colour.

// dBFS of a raw peak (0..32768) after the volume setting, as the saved clip
// will contain it (the clamp at full scale included). Silence reads the floor.
inline float peakToDb(int peak, int percent = 100) {
    percent = std::clamp(percent, 0, 200);
    const long long scaled = std::min<long long>(32767, static_cast<long long>(peak) * percent / 100);
    if (scaled <= 0) return meterFloorDb;
    return std::max(meterFloorDb, 20.f * std::log10(static_cast<float>(scaled) / 32768.f));
}

// What a bar shows: rises at once, holds a new peak briefly, then falls steadily.
struct MeterBar {
    static constexpr float holdSeconds = 0.25f, fallDbPerSecond = 30.f;
    float db = meterFloorDb;
    float held = 0.f; // Seconds since the shown peak was last matched.

    void update(float measuredDb, float seconds) {
        if (measuredDb >= db) { db = measuredDb; held = 0.f; return; }
        held += seconds;
        if (held > holdSeconds) db = std::max(measuredDb, db - fallDbPerSecond * seconds);
    }
    void reset() { *this = {}; }
    float fraction() const { return std::clamp((db - meterFloorDb) / -meterFloorDb, 0.f, 1.f); }
    bool clipping() const { return db >= meterClipDb; }
};

// Filled pixels of a bar `length` long; a repaint is needed only when this or the colour changes.
inline int meterLength(float fraction, int length) {
    return std::clamp(static_cast<int>(std::lround(fraction * static_cast<float>(std::max(length, 0)))), 0, std::max(length, 0));
}

// Saved tracks and the names written into the file for them (MP4 handler names,
// read by the built-in player to offer each source on its own).
struct PreparedAudio {
    std::vector<std::vector<std::int16_t>> tracks;
    std::vector<std::string> names;
};

// Inputs already have any microphone filters applied; an empty input was not
// recorded. Discord, split out of desktop audio, keeps the desktop volume. Keep
// muted tracks as silent alternates; every player gets the same adjusted levels
// in the mix, which comes first whenever two or more sources recorded. A game track (the game window's own
// process tree) comes last and, while `gameInDesktop` (desktop audio follows the system default, which the game plays
// on, or a chosen output the game plays on or may play on: gameInDesktopAudio), is not part of the mix: desktop audio
// already holds it. Only a chosen output the game verifiably does not play on makes it an independent source named
// "game output", summed into the mix like Discord.
inline PreparedAudio prepareAudioTracks(
        std::vector<std::int16_t> desktop, std::vector<std::int16_t> microphone,
        const AudioLevels& levels, std::vector<std::int16_t> discord = {}, std::vector<std::int16_t> game = {},
        bool gameInDesktop = true) {
    applyAudioLevel(desktop, levels.desktopPercent);
    applyAudioLevel(microphone, levels.microphonePercent);
    applyAudioLevel(discord, levels.desktopPercent);
    applyAudioLevel(game, levels.desktopPercent); // The game is part of desktop audio.
    std::vector<std::int16_t> none; // Stands in for the game when it is already inside desktop audio.
    const std::pair<const char*, std::vector<std::int16_t>*> sources[] = {
        {"desktop", &desktop}, {"microphone", &microphone}, {"discord", &discord}, {"game output", gameInDesktop ? &none : &game}};
    std::size_t recorded = 0, longest = 0;
    for (const auto& [name, samples] : sources) {
        if (samples->empty()) continue;
        ++recorded;
        longest = std::max(longest, samples->size());
    }
    PreparedAudio prepared;
    if (recorded > 1) {
        std::vector<std::int16_t> mixed(longest);
        for (std::size_t i = 0; i < mixed.size(); ++i) {
            int sum = 0;
            for (const auto& [name, samples] : sources) if (i < samples->size()) sum += (*samples)[i];
            mixed[i] = static_cast<std::int16_t>(std::clamp(sum, -32768, 32767));
        }
        prepared.tracks.push_back(std::move(mixed));
        prepared.names.push_back("mix");
    }
    for (const auto& [name, samples] : sources) {
        if (samples->empty()) continue;
        prepared.tracks.push_back(std::move(*samples));
        prepared.names.push_back(name);
    }
    // The game's own sound is already in desktop audio (and so in the mix): it is a track of its own, never summed.
    if (gameInDesktop && !game.empty()) {
        prepared.tracks.push_back(std::move(game));
        prepared.names.push_back("game");
    }
    return prepared;
}

} // namespace refarter::capture
