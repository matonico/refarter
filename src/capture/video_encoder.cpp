#include "video_encoder.h"
#include "annexb.h"
#include "environment.h"

#include <mferror.h>
#include <strmif.h>
#include <initguid.h>
#include <codecapi.h>
#include <algorithm>

namespace refarter::capture {

namespace {
// Optional tuning: not every encoder supports every property.
HRESULT setCodecValue(ICodecAPI* codec, const GUID& key, UINT32 value) {
    VARIANT v{};
    v.vt = VT_UI4;
    v.ulVal = value;
    return codec->SetValue(&key, &v);
}

// Some encoders accept a value and keep their own; read it back where they report it.
bool holdsValue(ICodecAPI* codec, const GUID& key, UINT32 value) {
    VARIANT v{};
    if (FAILED(codec->GetValue(&key, &v))) return true;
    const bool held = v.vt == VT_UI4 && v.ulVal == value;
    VariantClear(&v);
    return held;
}

void setCodecFlag(ICodecAPI* codec, const GUID& key, bool value) {
    VARIANT v{};
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    codec->SetValue(&key, &v);
}

void setVideoFormat(IMFMediaType* type, const EncoderConfig& config) {
    winrt::check_hresult(MFSetAttributeSize(type, MF_MT_FRAME_SIZE, config.size.width, config.size.height));
    winrt::check_hresult(MFSetAttributeRatio(type, MF_MT_FRAME_RATE, config.fps, 1));
    winrt::check_hresult(MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    winrt::check_hresult(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    // Matches the converter output, and is signalled in the stream for players.
    type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
}

bool sameAdapter(IMFActivate* activate, LUID adapter) {
    LUID luid{};
    UINT32 size = 0;
    if (SUCCEEDED(activate->GetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<UINT8*>(&luid), sizeof(luid), &size)) && size == sizeof(luid)) {
        return luid.LowPart == adapter.LowPart && luid.HighPart == adapter.HighPart;
    }
    UINT64 packed = 0;
    if (SUCCEEDED(activate->GetUINT64(MFT_ENUM_ADAPTER_LUID, &packed))) {
        return packed == ((static_cast<UINT64>(static_cast<UINT32>(adapter.HighPart)) << 32) | adapter.LowPart);
    }
    return false;
}

// The codec data a media type carries, when it has any; `header` is left as it was otherwise.
void readSequenceHeader(IMFMediaType* type, std::vector<std::uint8_t>& header) {
    UINT32 size = 0;
    if (FAILED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) || !size) return;
    header.resize(size);
    type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header.data(), size, nullptr);
}

std::wstring friendlyName(IMFActivate* activate) {
    wchar_t* value = nullptr;
    UINT32 length = 0;
    if (FAILED(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &value, &length))) return L"hardware H.264 encoder";
    std::wstring name(value, length);
    CoTaskMemFree(value);
    return name;
}
} // namespace

const GUID& mediaSubtype(Codec codec) {
    return codec == Codec::AV1 ? MFVideoFormat_AV1 : codec == Codec::HEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264;
}

UINT32 mediaProfile(Codec codec) {
    // eAVEncAV1VProfile_Main_420_8; spelled out because older SDKs lack the enum.
    constexpr UINT32 av1Main8 = 1;
    return codec == Codec::AV1 ? av1Main8 :
        codec == Codec::HEVC ? eAVEncH265VProfile_Main_420_8 : eAVEncH264VProfile_High;
}

struct VideoEncoder::EventCallback : winrt::implements<EventCallback, IMFAsyncCallback> {
    std::mutex mutex;
    VideoEncoder* owner = nullptr;
    winrt::com_ptr<IMFMediaEventGenerator> events;

    STDMETHODIMP GetParameters(DWORD*, DWORD*) noexcept override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult* result) noexcept override {
        std::lock_guard lock(mutex);
        if (!owner) return S_OK; // Shutting down; do not re-arm.
        winrt::com_ptr<IMFMediaEvent> event;
        if (const HRESULT hr = events->EndGetEvent(result, event.put()); FAILED(hr)) {
            owner->fail(hr);
            return S_OK;
        }
        owner->onEvent(event.get());
        if (const HRESULT hr = events->BeginGetEvent(this, nullptr); FAILED(hr)) owner->fail(hr);
        return S_OK;
    }
};

VideoEncoder::VideoEncoder(ID3D11Device* device, LUID adapter, const EncoderConfig& config,
                         PacketHandler onPacket, ErrorHandler onError)
    : onPacket_(std::move(onPacket)), onError_(std::move(onError)), codec_(config.codec) {
    UINT token = 0;
    winrt::check_hresult(MFCreateDXGIDeviceManager(&token, deviceManager_.put()));
    winrt::check_hresult(deviceManager_->ResetDevice(device, token));

    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, mediaSubtype(config.codec)};
    IMFActivate** found = nullptr;
    UINT32 count = 0;
    winrt::check_hresult(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                   &input, &output, &found, &count));
    std::vector<winrt::com_ptr<IMFActivate>> candidates(count);
    for (UINT32 i = 0; i < count; ++i) candidates[i].attach(found[i]);
    CoTaskMemFree(found);
    // Prefer the encoder on the capture GPU; others would force a cross-adapter copy.
    std::stable_partition(candidates.begin(), candidates.end(), [&](const auto& a) { return sameAdapter(a.get(), adapter); });

    HRESULT lastError = MF_E_TOPO_CODEC_NOT_FOUND;
    for (auto& candidate : candidates) {
        winrt::com_ptr<IMFTransform> transform;
        if (FAILED(lastError = candidate->ActivateObject(IID_PPV_ARGS(transform.put())))) continue;
        winrt::com_ptr<IMFAttributes> attributes;
        UINT32 async = FALSE;
        UINT32 d3d11Aware = FALSE;
        if (SUCCEEDED(transform->GetAttributes(attributes.put()))) {
            attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async);
            attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d11Aware);
        }
        if (!async || !d3d11Aware ||
            FAILED(lastError = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE)) ||
            FAILED(lastError = transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(deviceManager_.get())))) {
            candidate->ShutdownObject();
            continue;
        }
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        activate_ = candidate;
        transform_ = transform;
        name_ = friendlyName(candidate.get());
        break;
    }
    if (!transform_) winrt::throw_hresult(lastError);

    try {
        const bool diagnostics = environmentIs(L"REFARTER_CAPTURE_DIAGNOSTICS", L"1");
        const auto checkType = [diagnostics](HRESULT result, const wchar_t* stage) {
            if (diagnostics && FAILED(result))
                throw winrt::hresult_error(result, winrt::hstring(std::wstring(stage) + L": " + std::wstring(winrt::hresult_error(result).message())));
            winrt::check_hresult(result);
        };
        // Properties first: some encoders lock them once media types are set.
        const UINT32 average = static_cast<UINT32>(config.bitrate.averageKbps) * 1000u;
        codecApi_ = transform_.try_as<ICodecAPI>();
        if (auto codec = codecApi_) {
            // Presets: variable bitrate saves space in calm scenes; the peak cap
            // keeps fast motion sharp and replay memory bounded. Custom: CBR
            // holds the chosen bitrate exactly. Encoders without peak-constrained
            // VBR run CBR at the average, which is still the smaller size.
            const UINT32 vbr = eAVEncCommonRateControlMode_PeakConstrainedVBR;
            variableBitrate_ = !config.bitrate.constant &&
                SUCCEEDED(setCodecValue(codec.get(), CODECAPI_AVEncCommonRateControlMode, vbr)) &&
                holdsValue(codec.get(), CODECAPI_AVEncCommonRateControlMode, vbr) &&
                SUCCEEDED(setCodecValue(codec.get(), CODECAPI_AVEncCommonMaxBitRate, static_cast<UINT32>(config.bitrate.peakKbps) * 1000u));
            if (!variableBitrate_) setCodecValue(codec.get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
            setCodecValue(codec.get(), CODECAPI_AVEncCommonMeanBitRate, average);
            // One keyframe per second: saved clips start at most a second early.
            setCodecValue(codec.get(), CODECAPI_AVEncMPVGOPSize, static_cast<UINT32>(config.fps));
            // No B-frames and low-latency mode: one frame in, one frame out, so
            // input surfaces are returned promptly and the pool stays small.
            setCodecValue(codec.get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
            setCodecFlag(codec.get(), CODECAPI_AVLowLatencyMode, true);
        }

        winrt::com_ptr<IMFMediaType> outputType;
        winrt::check_hresult(MFCreateMediaType(outputType.put()));
        winrt::check_hresult(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        winrt::check_hresult(outputType->SetGUID(MF_MT_SUBTYPE, mediaSubtype(config.codec)));
        winrt::check_hresult(outputType->SetUINT32(MF_MT_AVG_BITRATE, average));
        winrt::check_hresult(outputType->SetUINT32(MF_MT_MPEG2_PROFILE, mediaProfile(config.codec)));
        setVideoFormat(outputType.get(), config);
        checkType(transform_->SetOutputType(0, outputType.get(), 0), L"encoder output type");

        winrt::com_ptr<IMFMediaType> inputType;
        for (DWORD i = 0;; ++i) {
            winrt::com_ptr<IMFMediaType> candidate;
            if (FAILED(transform_->GetInputAvailableType(0, i, candidate.put()))) break;
            GUID subtype{};
            if (SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12) {
                inputType = candidate;
                break;
            }
        }
        if (!inputType) {
            winrt::check_hresult(MFCreateMediaType(inputType.put()));
            winrt::check_hresult(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
            winrt::check_hresult(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
        }
        setVideoFormat(inputType.get(), config);
        checkType(transform_->SetInputType(0, inputType.get(), 0), L"encoder input type");

        winrt::com_ptr<IMFMediaType> current;
        if (SUCCEEDED(transform_->GetOutputCurrentType(0, current.put()))) readSequenceHeader(current.get(), sequenceHeader_);

        MFT_OUTPUT_STREAM_INFO info{};
        winrt::check_hresult(transform_->GetOutputStreamInfo(0, &info));
        providesSamples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        outputBufferSize_ = std::max<DWORD>(info.cbSize, 1024 * 1024);

        callback_ = winrt::make_self<EventCallback>();
        callback_->owner = this;
        callback_->events = transform_.as<IMFMediaEventGenerator>();
        winrt::check_hresult(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
        winrt::check_hresult(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
        winrt::check_hresult(callback_->events->BeginGetEvent(callback_.get(), nullptr));
    } catch (...) {
        if (callback_) {
            std::lock_guard lock(callback_->mutex);
            callback_->owner = nullptr;
        }
        if (auto shutdown = transform_.try_as<IMFShutdown>()) shutdown->Shutdown();
        transform_ = nullptr;
        activate_->ShutdownObject();
        throw;
    }
}

VideoEncoder::~VideoEncoder() {
    {
        // Waits for an in-flight event, then stops further callbacks reaching us.
        std::lock_guard lock(callback_->mutex);
        callback_->owner = nullptr;
    }
    {
        std::lock_guard lock(mutex_);
        pending_ = nullptr;
    }
    // Shutdown cancels the outstanding event request and releases held input surfaces.
    if (auto shutdown = transform_.try_as<IMFShutdown>()) shutdown->Shutdown();
    transform_ = nullptr;
    activate_->ShutdownObject();
}

bool VideoEncoder::submit(winrt::com_ptr<IMFSample> sample) {
    std::lock_guard lock(mutex_);
    if (failed_) return false;
    if (inputRequests_ > 0) {
        --inputRequests_;
        processInput(sample.get());
        return true;
    }
    const bool displaced = pending_ != nullptr;
    pending_ = std::move(sample);
    return !displaced;
}

void VideoEncoder::onEvent(IMFMediaEvent* event) {
    MediaEventType type = MEUnknown;
    event->GetType(&type);
    HRESULT status = S_OK;
    event->GetStatus(&status);
    if (FAILED(status)) {
        fail(status);
        return;
    }
    std::lock_guard lock(mutex_);
    if (failed_) return;
    try {
        if (type == METransformNeedInput) {
            if (pending_) {
                const auto sample = std::move(pending_);
                processInput(sample.get());
            } else {
                ++inputRequests_;
            }
        } else if (type == METransformHaveOutput) {
            processOutput();
        } else if (type == MEError) {
            winrt::throw_hresult(E_FAIL);
        }
    } catch (...) {
        // Runs inside a Media Foundation callback: report, never unwind into it.
        failed_ = true;
        if (onError_) onError_(winrt::to_hresult());
    }
}

void VideoEncoder::processInput(IMFSample* sample) {
    // The GOP counts frames, so a static or paused source would go minutes
    // without a keyframe and replays could only start at the oldest one.
    // Also start a GOP once a second of capture time has passed. The request is made once and repeated only after
    // another second without a keyframe coming out, not on every input frame while the encoder catches up.
    LONGLONG time = 0;
    if (codecApi_ && lastKeyframe_ >= 0 && SUCCEEDED(sample->GetSampleTime(&time)) && time - lastKeyframe_ >= 10'000'000 &&
        (forcedAt_ < 0 || time - forcedAt_ >= 10'000'000)) {
        forcedAt_ = time;
        setCodecValue(codecApi_.get(), CODECAPI_AVEncVideoForceKeyFrame, 1); // Unsupported: GOP size alone, as before.
    }
    if (const HRESULT hr = transform_->ProcessInput(0, sample, 0); FAILED(hr)) {
        failed_ = true;
        if (onError_) onError_(hr);
    }
}

void VideoEncoder::processOutput() {
    winrt::com_ptr<IMFSample> allocated;
    MFT_OUTPUT_DATA_BUFFER output{};
    if (!providesSamples_) {
        winrt::com_ptr<IMFMediaBuffer> buffer;
        winrt::check_hresult(MFCreateSample(allocated.put()));
        winrt::check_hresult(MFCreateMemoryBuffer(outputBufferSize_, buffer.put()));
        winrt::check_hresult(allocated->AddBuffer(buffer.get()));
        output.pSample = allocated.get();
    }
    DWORD status = 0;
    const HRESULT hr = transform_->ProcessOutput(0, 1, &output, &status);
    if (output.pEvents) output.pEvents->Release();
    winrt::com_ptr<IMFSample> sample;
    if (providesSamples_) sample.attach(output.pSample);
    else sample = allocated;

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        // The encoder revised its output format (often to add codec data).
        // Accept it; the frame arrives with the next output event.
        winrt::com_ptr<IMFMediaType> type;
        if (SUCCEEDED(transform_->GetOutputAvailableType(0, 0, type.put()))) {
            transform_->SetOutputType(0, type.get(), 0);
            // AV1 carries no in-band parameter sets for the writer; keep the codec data.
            readSequenceHeader(type.get(), sequenceHeader_);
        }
        return;
    }
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
    if (FAILED(hr) || !sample) {
        failed_ = true;
        if (onError_) onError_(FAILED(hr) ? hr : E_POINTER);
        return;
    }

    auto packet = std::make_shared<EncodedPacket>();
    LONGLONG time = 0;
    LONGLONG duration = 0;
    sample->GetSampleTime(&time);
    if (FAILED(sample->GetSampleDuration(&duration)) || duration <= 0) duration = 0;
    packet->time = time;
    packet->duration = duration;
    winrt::com_ptr<IMFMediaBuffer> buffer;
    winrt::check_hresult(sample->ConvertToContiguousBuffer(buffer.put()));
    BYTE* data = nullptr;
    DWORD length = 0;
    winrt::check_hresult(buffer->Lock(&data, nullptr, &length));
    packet->data.assign(data, data + length);
    buffer->Unlock();
    if (codec_ == Codec::AV1) {
        // Not Annex B: trust the encoder's clean-point flag.
        packet->keyframe = MFGetAttributeUINT32(sample.get(), MFSampleExtension_CleanPoint, FALSE) != FALSE;
    } else {
        packet->keyframe = annexb::containsKeyframe(codec_, packet->data);
        if (packet->keyframe) {
            if (auto header = annexb::parameterSets(codec_, packet->data); !header.empty()) sequenceHeader_ = std::move(header);
        }
    }
    if (packet->keyframe) lastKeyframe_ = packet->time;
    if (onPacket_) onPacket_(std::move(packet));
}

void VideoEncoder::fail(HRESULT hr) {
    std::lock_guard lock(mutex_);
    if (failed_) return;
    failed_ = true;
    if (onError_) onError_(hr);
}

std::vector<std::uint8_t> VideoEncoder::sequenceHeader() const {
    std::lock_guard lock(mutex_);
    return sequenceHeader_;
}

} // namespace refarter::capture
