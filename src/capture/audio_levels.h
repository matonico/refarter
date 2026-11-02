#pragma once

#include <algorithm>
#include <array>
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

// Saved tracks (interleaved, stereo except the microphone's: see sourceChannels) and the names written into the
// file for them (MP4 handler names, read by the built-in player to offer each source on its own).
struct PreparedAudio {
    std::vector<std::vector<std::int16_t>> tracks;
    std::vector<std::string> names;
};

// The sources, in this order, as prepareAudioTracks and SaveAudio take them.
enum AudioSource : int { DesktopSource, MicrophoneSource, DiscordSource, GameSource, audioSourceCount };

// Channels a source is buffered and saved with: the microphone is mono (its two channels averaged), the others and
// the mix stereo. Everything in between (ring reads, filters, volume, the mix) stays stereo.
inline constexpr int sourceChannels(int source) { return source == MicrophoneSource ? 1 : 2; }

// Stereo frames to mono, the two channels averaged (the same as a mono ring stores them).
inline void downmix(const std::int16_t* stereo, std::size_t frames, std::int16_t* mono) {
    for (std::size_t f = 0; f < frames; ++f) mono[f] = static_cast<std::int16_t>((stereo[2 * f] + stereo[2 * f + 1]) / 2);
}

// The volume setting a source takes: Discord and the game are part of desktop audio.
inline int sourcePercent(int source, const AudioLevels& levels) {
    return source == MicrophoneSource ? levels.microphonePercent : levels.desktopPercent;
}

// Which tracks a clip saves (see prepareAudioTracks): their names and, per track, the sources summed into it and
// its channels (a track of one source has that source's, the mix is stereo).
struct AudioLayout {
    std::vector<std::string> names;
    std::vector<std::vector<int>> sources;
    std::vector<int> channels;
};

inline AudioLayout audioLayout(const std::array<bool, audioSourceCount>& recorded, bool gameInDesktop) {
    static constexpr const char* names[] = {"desktop", "microphone", "discord", "game output"};
    std::vector<int> summed; // The game stands out of the mix while desktop audio already holds it.
    for (int source = 0; source < audioSourceCount; ++source)
        if (recorded[source] && (source != GameSource || !gameInDesktop)) summed.push_back(source);
    AudioLayout layout;
    const auto add = [&](const char* name, std::vector<int> sources) {
        layout.names.push_back(name);
        layout.channels.push_back(sources.size() == 1 ? sourceChannels(sources.front()) : 2);
        layout.sources.push_back(std::move(sources));
    };
    if (summed.size() > 1) add("mix", summed);
    for (const int source : summed) add(names[source], {source});
    // The game's own sound is already in desktop audio (and so in the mix): it is a track of its own, never summed.
    if (gameInDesktop && recorded[GameSource]) add("game", {GameSource});
    return layout;
}

// One piece of every track of `layout` from one stereo piece of each source (volume applied), `samples` long each;
// a mono track comes out half as long.
inline void mixPieces(const AudioLayout& layout, const std::array<const std::int16_t*, audioSourceCount>& pieces,
                      std::size_t samples, std::vector<std::vector<std::int16_t>>& tracks) {
    tracks.resize(layout.sources.size());
    for (std::size_t t = 0; t < tracks.size(); ++t) {
        auto& track = tracks[t];
        const auto& sources = layout.sources[t];
        if (layout.channels[t] == 1) {
            track.resize(samples / 2);
            downmix(pieces[sources.front()], samples / 2, track.data());
            continue;
        }
        track.resize(samples);
        if (sources.size() == 1) { std::copy_n(pieces[sources.front()], samples, track.begin()); continue; }
        for (std::size_t i = 0; i < samples; ++i) {
            int sum = 0;
            for (const int source : sources) sum += pieces[source][i];
            track[i] = static_cast<std::int16_t>(std::clamp(sum, -32768, 32767));
        }
    }
}

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
    std::vector<std::int16_t>* sources[] = {&desktop, &microphone, &discord, &game};
    std::array<bool, audioSourceCount> recorded{};
    for (int source = 0; source < audioSourceCount; ++source) {
        applyAudioLevel(*sources[source], sourcePercent(source, levels));
        recorded[source] = !sources[source]->empty();
    }
    const auto layout = audioLayout(recorded, gameInDesktop);
    PreparedAudio prepared;
    prepared.names = layout.names;
    for (std::size_t t = 0; t < layout.sources.size(); ++t) {
        const auto& summed = layout.sources[t];
        if (layout.channels[t] == 1) { // The microphone, saved mono.
            const auto& stereo = *sources[summed.front()];
            std::vector<std::int16_t> mono(stereo.size() / 2);
            downmix(stereo.data(), mono.size(), mono.data());
            prepared.tracks.push_back(std::move(mono));
            continue;
        }
        if (summed.size() == 1) { prepared.tracks.push_back(std::move(*sources[summed.front()])); continue; }
        std::size_t longest = 0; // Whole tracks may differ in length; a missing sample is silence.
        for (const int source : summed) longest = std::max(longest, sources[source]->size());
        std::vector<std::int16_t> mixed(longest);
        for (std::size_t i = 0; i < mixed.size(); ++i) {
            int sum = 0;
            for (const int source : summed) if (i < sources[source]->size()) sum += (*sources[source])[i];
            mixed[i] = static_cast<std::int16_t>(std::clamp(sum, -32768, 32767));
        }
        prepared.tracks.push_back(std::move(mixed)); // The mix comes first, before its sources move.
    }
    return prepared;
}

} // namespace refarter::capture
