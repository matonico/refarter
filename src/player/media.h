#pragma once

#include <d3d11.h>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct AVBufferRef;
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVIOContext;
struct AVPacket;
struct SwrContext;

namespace refarter::player {

struct FrameDeleter { void operator()(AVFrame* frame) const; };
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;

// A decoded picture and when it shows, in seconds from the start of the clip.
struct Picture {
    Frame frame;
    double time = 0;
    explicit operator bool() const { return frame != nullptr; }
};

// One clip opened with FFmpeg: the demuxer, a video decoder (Direct3D 11 on the
// GPU when it can, software otherwise; dav1d for AV1 without GPU support) and
// the chosen audio track, resampled to 48 kHz stereo float. Not thread-safe:
// each MediaFile belongs to one thread. The file is shared for deleting, so
// it can be renamed (or binned) while open; reading carries on as before.
class MediaFile {
public:
    struct Options {
        ID3D11Device* device = nullptr; // Null decodes in software.
        bool audio = true;
        bool software = false;          // Force software decoding (testing, fallback).
    };

    MediaFile();
    ~MediaFile();
    MediaFile(const MediaFile&) = delete;
    MediaFile& operator=(const MediaFile&) = delete;

    // Opens the file and decodes the first picture. On failure error() says why.
    bool open(const std::filesystem::path& path, const Options& options);
    const std::wstring& error() const { return error_; }

    double duration() const { return duration_; }
    double frameDuration() const { return frameDuration_; }
    bool hardware() const { return hardware_; }
    const std::wstring& decoderName() const { return decoderName_; }
    // Keyframe times from the file's index, ascending; a cut starts at one of these.
    const std::vector<double>& keyframes() const { return keyframes_; }

    int audioTracks() const { return static_cast<int>(audioStreams_.size()); }
    // The one decoded track; -1 with none or several.
    int audioTrack() const { return selection_.size() == 1 ? selection_.front() : -1; }
    std::wstring audioTrackName(int track) const;
    // Switches the decoded audio tracks, played summed (one track plays exactly as
    // stored, with no mixing pass); call seek() afterwards.
    bool selectAudio(const std::vector<int>& tracks);
    bool selectAudio(int track) { return selectAudio(std::vector<int>{track}); }

    // Positions both decoders on `time` and returns the picture showing then.
    Picture seek(double time);
    // Strictly preceding decoded picture; stays on the first picture at the start.
    // Uses actual presentation times, not the nominal frame duration.
    Picture previous(double time);
    // The next picture in display order; empty at the end of the clip.
    Picture next();
    // Up to `frames` of interleaved stereo float at 48 kHz, continuing from the
    // last seek without gaps; 0 when nothing more can be decoded right now.
    std::size_t audio(float* out, std::size_t frames);
    bool audioEnded() const;
    // The keyframe at or before `time`, decoded on its own (timeline thumbnails).
    Picture keyframe(double time);

private:
    struct AudioDecoder;
    Picture seekPicture(double time, bool previous);
    bool openVideo(bool hardware);
    void closeAudio();
    bool readPacket();
    double seconds(long long pts, int stream) const;
    bool decodeAudioFrame(AudioDecoder& decoder);

    std::wstring error_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    AVIOContext* io_ = nullptr; // Reads file_ for the demuxer.
    AVFormatContext* format_ = nullptr;
    AVCodecContext* video_ = nullptr;
    AVBufferRef* device_ = nullptr;
    AVFrame* scratch_ = nullptr;
    int videoStream_ = -1;
    std::vector<int> audioStreams_;
    std::vector<int> selection_; // Decoded audio tracks.
    std::vector<std::unique_ptr<AudioDecoder>> decoders_; // One per selected track.
    Options options_;
    bool hardware_ = false;
    std::wstring decoderName_;
    double origin_ = 0, duration_ = 0, frameDuration_ = 1.0 / 60;
    std::vector<double> keyframes_;
    double keyframeDelay_ = -1; // Presentation minus decoding time of the first keyframe; -1 until read.
    std::deque<AVPacket*> videoPackets_;
    bool demuxEnded_ = false, videoDraining_ = false;
    Picture peeked_;
    double lastTime_ = -1;
    // Audio after a seek starts exactly at alignTo_: earlier samples are dropped, a gap is filled with silence.
    // audioTime_ is the position of the next sample audio() hands out.
    double alignTo_ = 0, audioTime_ = 0;
};

} // namespace refarter::player
