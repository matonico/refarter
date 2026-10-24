#include "chime.h"
#include "resources.h"

#include <windows.h>
#include <mmsystem.h>

namespace refarter {
namespace {
constexpr int resources[] = {IDW_SAVE_CHIME, IDW_FAILED_CHIME}; // Indexed by Chimes::Kind.
} // namespace

void Chimes::prepare(int percent) {
    percent_ = percent;
    // A scaled copy may be playing; stop it before replacing its bytes.
    PlaySoundW(nullptr, nullptr, 0);
    for (auto& wav : wav_) wav.clear();
    if (percent_ == 0 || percent_ == 100) return; // Silent, or the resources as they are.
    const auto module = GetModuleHandleW(nullptr);
    for (std::size_t i = 0; i < wav_.size(); ++i) {
        const auto resource = FindResourceW(module, MAKEINTRESOURCEW(resources[i]), L"WAVE");
        const auto loaded = resource ? LoadResource(module, resource) : nullptr;
        if (!loaded) continue;
        const auto* bytes = static_cast<const std::uint8_t*>(LockResource(loaded));
        wav_[i].assign(bytes, bytes + SizeofResource(module, resource));
        if (!scaleWavVolume(wav_[i], percent_)) wav_[i].clear();
    }
}

void Chimes::play(Kind kind, bool preview) {
    const bool keepPlaying = !preview && kind == Kind::Saved && last_ == Kind::Saved;
    const DWORD flags = SND_ASYNC | SND_NODEFAULT | (keepPlaying ? SND_NOSTOP : 0);
    if (percent_ == 0) return;
    last_ = kind;
    const auto index = static_cast<std::size_t>(kind);
    if (!wav_[index].empty()) PlaySoundW(reinterpret_cast<LPCWSTR>(wav_[index].data()), nullptr, SND_MEMORY | flags);
    else PlaySoundW(MAKEINTRESOURCEW(resources[index]), GetModuleHandleW(nullptr), SND_RESOURCE | flags);
}

} // namespace refarter
