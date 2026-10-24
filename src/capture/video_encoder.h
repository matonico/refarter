#pragma once

#include "replay_buffer.h"
#include "video_settings.h"

#include <d3d11.h>
#include <strmif.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <winrt/base.h>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace refarter::capture {

struct EncoderConfig {
    Size size;
    int fps = 60;
    Codec codec = Codec::H264;
    Bitrate bitrate{50'000, 50'000, true};
};

// Subtype and profile shared by the encoder output and the MP4 writer.
const GUID& mediaSubtype(Codec codec);
UINT32 mediaProfile(Codec codec);

// The GPU's hardware video encoder (NVENC, AMF or Quick Sync) through Media
// Foundation: H.264, HEVC or AV1. Input frames stay in GPU memory. The encoder
// is event-driven: Media Foundation calls back when it wants input or has
// output, so there is no polling thread and no CPU work between frames.
class VideoEncoder {
public:
    using PacketHandler = std::function<void(PacketPtr)>;
    using ErrorHandler = std::function<void(HRESULT)>;

    VideoEncoder(ID3D11Device* device, LUID adapter, const EncoderConfig& config,
                 PacketHandler onPacket, ErrorHandler onError);
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    // Accepts a frame for encoding. Returns false when it displaced an older
    // frame the encoder had not yet asked for (that frame is dropped).
    bool submit(winrt::com_ptr<IMFSample> sample);

    const std::wstring& name() const { return name_; }
    // What the encoder accepted: some ignore variable bitrate and run CBR at the average.
    bool variableBitrate() const { return variableBitrate_; }
    std::vector<std::uint8_t> sequenceHeader() const;

private:
    struct EventCallback;
    void onEvent(IMFMediaEvent* event);
    void processInput(IMFSample* sample);
    void processOutput();
    void fail(HRESULT hr);

    winrt::com_ptr<IMFDXGIDeviceManager> deviceManager_;
    winrt::com_ptr<IMFActivate> activate_;
    winrt::com_ptr<IMFTransform> transform_;
    winrt::com_ptr<EventCallback> callback_;
    PacketHandler onPacket_;
    ErrorHandler onError_;
    Codec codec_;
    std::wstring name_;
    bool variableBitrate_ = false;
    bool providesSamples_ = true;
    DWORD outputBufferSize_ = 0;

    winrt::com_ptr<ICodecAPI> codecApi_; // Null when the encoder exposes no codec properties.

    mutable std::mutex mutex_;
    std::int64_t lastKeyframe_ = -1; // Sample time of the newest keyframe output.
    std::int64_t forcedAt_ = -1; // Sample time of the newest ForceKeyFrame request (one per second at most).
    int inputRequests_ = 0;
    winrt::com_ptr<IMFSample> pending_;
    std::vector<std::uint8_t> sequenceHeader_;
    bool failed_ = false;
};

} // namespace refarter::capture
