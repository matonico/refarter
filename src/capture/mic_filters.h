#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace refarter::capture {

// Microphone clean-up, applied when a clip is saved (never while playing), to
// the raw 48 kHz stereo track: noise suppression first, then the gate.
// Both are off until the user turns them on; gate values default to OBS's
// own. Quiet headset microphones may need lower levels (e.g. -45/-50 dB).
struct MicFilters {
    bool noiseSuppression = false; // RNNoise: removes breathing, fans, keyboard hiss.
    bool noiseGate = false;        // Silences the mic between words.
    float gateOpenDb = -26.f;      // Opens above this level (dBFS)...
    float gateCloseDb = -32.f;     // ...and closes below this one.
    int gateAttackMs = 25;         // Fade-in when opening.
    int gateHoldMs = 200;          // Stays open this long after the level drops.
    int gateReleaseMs = 150;       // Fade-out when closing.
};

std::optional<std::wstring> validate(const MicFilters& filters);

void applyNoiseSuppression(std::vector<std::int16_t>& stereo);
void applyNoiseGate(std::vector<std::int16_t>& stereo, const MicFilters& filters);
void applyMicFilters(std::vector<std::int16_t>& stereo, const MicFilters& filters);

} // namespace refarter::capture
