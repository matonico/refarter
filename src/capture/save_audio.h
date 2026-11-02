#pragma once

#include "audio_levels.h"
#include "audio_track.h"
#include "mic_filters.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace refarter::capture {

// The saved tracks of one clip. Each recorded source's span is copied out of its PCM ring when the save is pressed,
// before the save waits for an earlier one, so a save that runs slower than real time (or queues behind one) still
// has all its sound: the ring holds only the replay plus AudioTrack::leadSeconds, and the capture thread keeps
// writing into it. Only what the clip keeps is copied: a Discord or game span without sound is skipped, and the
// microphone, stored mono in its ring, stays mono. Only the filtering, levels and mixing are done a piece at a
// time while the clip is written, so a save holds the clip's PCM and a few pieces, not a mix and float copies of
// the microphone besides. The tracks are the same as reading each ring whole, filtering the microphone and
// calling prepareAudioTracks.
class SaveAudio {
public:
    // Sources in AudioSource order; null when not recorded. The rings are read here and never again. A recorded
    // source with no snapshot (silent Discord or game) reads as silence and sounds() says false.
    SaveAudio(const std::array<const AudioTrack*, audioSourceCount>& tracks, std::int64_t from, std::int64_t to,
              const MicFilters& filters, const AudioLevels& levels)
        : levels_(levels) {
        frames_ = static_cast<std::size_t>(AudioTrack::frameCount(from, to));
        for (int source = 0; source < audioSourceCount; ++source) {
            if (!tracks[source]) continue;
            recorded_[source] = true;
            const AudioTrack& track = *tracks[source];
            const auto span = track.spanOf(from, to);
            // Discord and the game are saved only for a clip where they made a sound, so a silent one is not copied.
            if ((source == DiscordSource || source == GameSource) && !track.sounds(span)) continue;
            copy(track, span, source);
        }
        if (recorded_[MicrophoneSource] && (filters.noiseSuppression || filters.noiseGate))
            mic_ = std::make_unique<MicFilterStream>(filters);
    }

    bool recorded(int source) const { return recorded_[source]; }
    // Any sound in that source's span (none when it was not copied).
    bool sounds(int source) const {
        const auto& pcm = snapshot_[source];
        return std::any_of(pcm.begin(), pcm.end(), [](std::int16_t s) { return s != 0; });
    }
    void drop(int source) { // Before layout(): a silent or doubled source.
        recorded_[source] = false;
        std::vector<std::int16_t>().swap(snapshot_[source]);
    }

    AudioLayout layout(bool gameInDesktop) const {
        std::array<bool, audioSourceCount> recorded{};
        for (int source = 0; source < audioSourceCount; ++source) recorded[source] = recorded_[source] && frames_ > 0;
        return audioLayout(recorded, gameInDesktop);
    }
    std::size_t frames() const { return frames_; } // Every track's length.

    // The next `frames` frames of every track of `layout`, in order.
    void next(const AudioLayout& layout, std::size_t frames, std::vector<std::vector<std::int16_t>>& tracks) {
        frames = std::min(frames, frames_ - done_);
        const std::size_t samples = frames * AudioTrack::channels;
        std::array<const std::int16_t*, audioSourceCount> pieces{};
        for (int source = 0; source < audioSourceCount; ++source) {
            if (!recorded_[source]) continue;
            auto& piece = pieces_[source];
            piece.resize(samples);
            const auto& pcm = snapshot_[source];
            if (source == MicrophoneSource && mic_) filtered(frames, piece.data());
            else if (pcm.empty()) std::fill(piece.begin(), piece.end(), std::int16_t{0});
            else if (channels_[source] == AudioTrack::channels) std::copy_n(pcm.data() + done_ * AudioTrack::channels, samples, piece.data());
            else for (std::size_t f = 0; f < frames; ++f) piece[2 * f] = piece[2 * f + 1] = pcm[done_ + f]; // Mono: both channels.
            applyAudioLevel(std::span(piece), sourcePercent(source, levels_));
            pieces[source] = piece.data();
        }
        mixPieces(layout, pieces, samples, tracks);
        done_ += frames;
    }

private:
    // The span of one ring into snapshot_, as many channels as the ring stores, a second at a time.
    void copy(const AudioTrack& track, const AudioTrack::Span& span, int source) {
        const auto frames = static_cast<std::size_t>(span.count);
        const auto stored = static_cast<std::size_t>(track.storedChannels());
        channels_[source] = static_cast<int>(stored);
        auto& pcm = snapshot_[source];
        pcm.resize(frames * stored);
        if (stored == AudioTrack::channels) {
            track.copy(span, 0, frames, pcm.data());
            return;
        }
        std::vector<std::int16_t> scratch(AudioTrack::chunkFrames * AudioTrack::channels);
        for (std::size_t at = 0; at < frames; at += AudioTrack::chunkFrames) {
            const auto part = std::min(AudioTrack::chunkFrames, frames - at);
            track.copy(span, at, part, scratch.data());
            for (std::size_t f = 0; f < part; ++f) pcm[at + f] = scratch[f * AudioTrack::channels]; // The channels are equal.
        }
    }

    // The microphone through its filters; RNNoise's lag means reading a little ahead of the piece.
    void filtered(std::size_t frames, std::int16_t* out) {
        const auto& pcm = snapshot_[MicrophoneSource];
        while (mic_->ready() < frames) {
            if (micRead_ == frames_) { mic_->finish(); break; }
            const std::size_t read = std::min<std::size_t>(AudioTrack::rate / 10, frames_ - micRead_);
            if (channels_[MicrophoneSource] == AudioTrack::channels) {
                mic_->push(pcm.data() + micRead_ * AudioTrack::channels, read);
            } else {
                micPiece_.resize(read * AudioTrack::channels);
                for (std::size_t f = 0; f < read; ++f) micPiece_[2 * f] = micPiece_[2 * f + 1] = pcm[micRead_ + f];
                mic_->push(micPiece_.data(), read);
            }
            micRead_ += read;
        }
        mic_->take(out, frames);
    }

    AudioLevels levels_;
    std::unique_ptr<MicFilterStream> mic_;
    std::size_t frames_ = 0, done_ = 0, micRead_ = 0;
    std::array<bool, audioSourceCount> recorded_{};
    std::array<std::vector<std::int16_t>, audioSourceCount> snapshot_; // The clip's span per source; empty if not copied.
    std::array<int, audioSourceCount> channels_{}; // Interleaved channels in each snapshot (2, or 1 for the microphone).
    std::vector<std::int16_t> micPiece_;           // A stereo piece of the mono microphone for the filters.
    std::array<std::vector<std::int16_t>, audioSourceCount> pieces_;
};

} // namespace refarter::capture
