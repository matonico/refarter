#include "chime.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
// A minimal mono 16-bit PCM WAV, with an odd-sized chunk before the data.
std::vector<std::uint8_t> wav(std::vector<std::int16_t> samples, std::uint16_t format = 1, std::uint16_t bits = 16) {
    std::vector<std::uint8_t> bytes;
    const auto add = [&](const void* data, std::size_t size) {
        const auto* at = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), at, at + size);
    };
    const auto add32 = [&](std::uint32_t value) { add(&value, 4); };
    const auto add16 = [&](std::uint16_t value) { add(&value, 2); };
    add("RIFF", 4); add32(0); add("WAVE", 4);
    add("fmt ", 4); add32(16); add16(format); add16(1); add32(24'000); add32(48'000); add16(2); add16(bits);
    add("LIST", 4); add32(3); add("abc", 3); bytes.push_back(0);
    add("data", 4); add32(static_cast<std::uint32_t>(samples.size() * 2)); add(samples.data(), samples.size() * 2);
    return bytes;
}

std::vector<std::int16_t> samples(const std::vector<std::uint8_t>& bytes, std::size_t count) {
    std::vector<std::int16_t> out(count);
    std::memcpy(out.data(), bytes.data() + bytes.size() - count * 2, count * 2);
    return out;
}
} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    using refarter::scaleWavVolume;
    const std::vector<std::int16_t> original{1000, -1000, 32767, -32768};

    auto full = wav(original);
    check(scaleWavVolume(full, 100) && samples(full, 4) == original, "100% leaves samples unchanged");
    auto half = wav(original);
    check(scaleWavVolume(half, 50) && samples(half, 4) == std::vector<std::int16_t>{500, -500, 16383, -16384}, "50% halves samples");
    auto muted = wav(original);
    check(scaleWavVolume(muted, 0) && samples(muted, 4) == std::vector<std::int16_t>{0, 0, 0, 0}, "0% silences");
    auto clamped = wav(original);
    check(scaleWavVolume(clamped, 150) && samples(clamped, 4) == original, "above 100% is clamped, never boosted");

    auto floats = wav(original, 3, 32);
    const auto before = floats;
    check(!scaleWavVolume(floats, 50) && floats == before, "non-PCM16 data is left alone");
    std::vector<std::uint8_t> junk{'n', 'o', 'p', 'e'};
    check(!scaleWavVolume(junk, 50), "not a WAV");
    auto truncated = wav(original);
    truncated.resize(truncated.size() - 3);
    check(!scaleWavVolume(truncated, 50), "truncated data chunk rejected");

    // Both shipped sounds are mono 16-bit PCM at 24 kHz, at most half a second, and scale like any other WAV.
    for (const char* name : {"save-chime.wav", "failed-chime.wav"}) {
        std::ifstream file(std::string(REFARTER_ASSETS_DIR) + "/" + name, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        check(bytes.size() > 44, name);
        if (bytes.size() <= 44) continue;
        std::uint16_t channels = 0, bits = 0;
        std::uint32_t rate = 0;
        std::memcpy(&channels, bytes.data() + 22, 2);
        std::memcpy(&rate, bytes.data() + 24, 4);
        std::memcpy(&bits, bytes.data() + 34, 2);
        check(channels == 1 && rate == 24'000 && bits == 16, "shipped sound is mono 16-bit 24 kHz");
        check(bytes.size() <= 44 + 24'000 + 64, "shipped sound is at most half a second"); // 24,000 bytes; some room for chunks.
        check(scaleWavVolume(bytes, 50), "shipped sound scales");
    }
    return failures ? 1 : 0;
}
