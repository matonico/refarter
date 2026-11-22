#include "frame_converter.h"
#include "diagnostic_log.h"
#include "processor_step.h"

#include <algorithm>
#include <limits>
#include <new>

namespace refarter::capture {

namespace {
// WGC recreates its two buffers on resize; a few entries cover the transition.
constexpr std::size_t maxCachedInputs = 4;
// Output views follow the encoder's fixed surface pool.
constexpr std::size_t unlimited = std::numeric_limits<std::size_t>::max();

winrt::com_ptr<ID3D11VideoProcessorEnumerator> createEnumerator(ID3D11VideoDevice* video, Size input, Size output, int fps,
                                                                DXGI_FORMAT format) {
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate = {static_cast<UINT>(fps), 1};
    desc.InputWidth = static_cast<UINT>(input.width);
    desc.InputHeight = static_cast<UINT>(input.height);
    desc.OutputFrameRate = {static_cast<UINT>(fps), 1};
    desc.OutputWidth = static_cast<UINT>(output.width);
    desc.OutputHeight = static_cast<UINT>(output.height);
    desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> enumerator;
    winrt::check_hresult(video->CreateVideoProcessorEnumerator(&desc, enumerator.put()));
    UINT support = 0;
    winrt::check_hresult(enumerator->CheckVideoProcessorFormat(format, &support));
    if (!(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) winrt::throw_hresult(E_NOTIMPL);
    return enumerator;
}
} // namespace

FrameConverter::FrameConverter(ID3D11Device* device, ID3D11DeviceContext* context, Size output, int fps, bool tenBit,
                               Size source)
    : output_(output), fps_(fps), format_(tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12) {
    device_.copy_from(device);
    videoDevice_ = device_.as<ID3D11VideoDevice>();
    winrt::check_hresult(context->QueryInterface(IID_PPV_ARGS(videoContext_.put())));
    context_.copy_from(context);
    if (tenBit) {
        // Refuse P010 now, at the captured size, while the caller can still fall back to NV12; the real processor
        // waits for the first frame's size.
        const Size checked = source.width > 0 && source.height > 0 ? source : output;
        auto enumerator = createEnumerator(videoDevice_.get(), checked, output, fps, format_);
        winrt::com_ptr<ID3D11VideoProcessor> processor;
        winrt::check_hresult(videoDevice_->CreateVideoProcessor(enumerator.get(), 0, processor.put()));
        const auto target = createTarget();
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc{};
        desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        winrt::com_ptr<ID3D11VideoProcessorOutputView> view;
        winrt::check_hresult(videoDevice_->CreateVideoProcessorOutputView(target.get(), enumerator.get(), &desc, view.put()));
        // The downscaler's 1:1. A refusal there only keeps the video processor's scaler, so a session that never
        // shrinks is not refused 10-bit for it.
        if (!(checked == output)) {
            try {
                enumerator = createEnumerator(videoDevice_.get(), output, output, fps, format_);
            } catch (const winrt::hresult_error& e) {
                if (e.code() != E_NOTIMPL) throw;
                enumerator = nullptr;
                oneToOneRefused_ = true;
            }
        }
        // The downscaler's output, which the processor reads at the output size, keeps 10 bits where it may.
        UINT support = 0;
        if (enumerator && SUCCEEDED(enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_R10G10B10A2_UNORM, &support)) &&
            (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) scaledFormat_ = DXGI_FORMAT_R10G10B10A2_UNORM;
    }
}

void FrameConverter::dropDownscaler(HRESULT code, int reason) {
    diagnosticLog().write(DiagnosticEvent::DownscalerFallback, code, reason, 0);
    // For the rest of the session the video processor scales, as before the downscaler existed.
    downscaler_.reset();
    downscalerFailed_ = true;
}

ID3D11VideoProcessorInputView* FrameConverter::downscale(ID3D11Texture2D* source, const RECT& from, const RECT& to) {
    if (downscalerFailed_) return nullptr;
    if (oneToOneRefused_) {
        dropDownscaler(E_NOTIMPL, 3); // reason=3: the processor refused P010 at 1:1.
        return nullptr;
    }
    // The processor refusing P010 at 1:1 only drops the downscaler; noteFailure never runs here, so the source size
    // decides the refusal.
    int reason = 0; // reason=0: the shaders or their textures failed, with the HRESULT.
    try {
        if (!downscaler_) downscaler_ = std::make_unique<Downscaler>(device_.get(), context_.get(), output_, scaledFormat_);
        if (!downscaler_->scale(source, from, to)) {
            const HRESULT removed = device_->GetDeviceRemovedReason();
            if (FAILED(removed)) winrt::throw_hresult(removed);
            dropDownscaler(S_FALSE, 1); // reason=1: shaders cannot read the source.
            return nullptr;
        }
        reason = 2; // reason=2: the video processor refused the downscaler's output.
        prepare(output_);
        return inputView(downscaler_->target());
    } catch (const winrt::hresult_error& e) {
        // A lost device ends the session as itself, not as a fallback.
        if (FAILED(device_->GetDeviceRemovedReason())) throw;
        dropDownscaler(e.code(), reason);
    } catch (const std::bad_alloc&) {
        dropDownscaler(E_OUTOFMEMORY, reason);
    }
    return nullptr;
}

void FrameConverter::noteFailure(HRESULT error, ID3D11Texture2D* source, Size sourceTexture, const RECT& sourceRect,
                                 const RECT& destRect) {
    // Proof that P010 caused the failure: the same frame, through every step, converts to NV12. Runs only on a failed
    // P010 frame, so the steady-state path allocates nothing.
    tenBitRefused_ = tenBitRefused_ || tenBitRefusal(format_ == DXGI_FORMAT_P010, error,
        [&] { return FAILED(device_->GetDeviceRemovedReason()); },
        [&] {
            const auto enumerator = createEnumerator(videoDevice_.get(), sourceTexture, output_, fps_, DXGI_FORMAT_NV12);
            winrt::com_ptr<ID3D11VideoProcessor> processor;
            winrt::check_hresult(videoDevice_->CreateVideoProcessor(enumerator.get(), 0, processor.put()));
            configure(processor.get());
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
            inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            winrt::com_ptr<ID3D11VideoProcessorInputView> input;
            winrt::check_hresult(videoDevice_->CreateVideoProcessorInputView(source, enumerator.get(), &inputDesc, input.put()));
            const auto target = createTarget(DXGI_FORMAT_NV12);
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
            outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            winrt::com_ptr<ID3D11VideoProcessorOutputView> output;
            winrt::check_hresult(videoDevice_->CreateVideoProcessorOutputView(target.get(), enumerator.get(), &outputDesc,
                                                                               output.put()));
            blit(processor.get(), input.get(), output.get(), sourceRect, destRect);
        });
}

winrt::com_ptr<ID3D11Texture2D> FrameConverter::createTarget() const { return createTarget(format_); }

winrt::com_ptr<ID3D11Texture2D> FrameConverter::createTarget(DXGI_FORMAT format) const {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(output_.width);
    desc.Height = static_cast<UINT>(output_.height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(device_->CreateTexture2D(&desc, nullptr, texture.put()));
    return texture;
}

void FrameConverter::configure(ID3D11VideoProcessor* processor) const {
    // Driver "enhancements" (denoise, edge enhancement...) cost GPU time and alter the image.
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(processor, 0, FALSE);
    videoContext_->VideoProcessorSetStreamFrameFormat(processor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext_->VideoProcessorSetStreamOutputRate(processor, 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, FALSE, nullptr);
    // Full-range sRGB desktop frames to limited-range BT.709, as players expect.
    videoContext_->VideoProcessorSetStreamColorSpace1(processor, 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    videoContext_->VideoProcessorSetOutputColorSpace1(processor, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    D3D11_VIDEO_COLOR black{};
    black.RGBA = {0.f, 0.f, 0.f, 1.f};
    videoContext_->VideoProcessorSetOutputBackgroundColor(processor, FALSE, &black);
}

void FrameConverter::blit(ID3D11VideoProcessor* processor, ID3D11VideoProcessorInputView* input,
                          ID3D11VideoProcessorOutputView* output, const RECT& sourceRect, const RECT& destRect) const {
    videoContext_->VideoProcessorSetStreamSourceRect(processor, 0, TRUE, &sourceRect);
    videoContext_->VideoProcessorSetStreamDestRect(processor, 0, TRUE, &destRect);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input;
    winrt::check_hresult(videoContext_->VideoProcessorBlt(processor, output, 0, 1, &stream));
}

void FrameConverter::prepare(Size sourceTexture) {
    if (processor_ && sourceTexture == processorInput_) return;
    inputs_.clear();
    outputs_.clear(); // Output views belong to the enumerator being replaced.
    processor_ = nullptr;
    enumerator_ = nullptr;

    enumerator_ = createEnumerator(videoDevice_.get(), sourceTexture, output_, fps_, format_);
    winrt::check_hresult(videoDevice_->CreateVideoProcessor(enumerator_.get(), 0, processor_.put()));
    configure(processor_.get());
    processorInput_ = sourceTexture;
}

ID3D11VideoProcessorInputView* FrameConverter::inputView(ID3D11Texture2D* texture) {
    return cachedView(inputs_, texture, maxCachedInputs, [&](ID3D11VideoProcessorInputView** view) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};
        desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        return videoDevice_->CreateVideoProcessorInputView(texture, enumerator_.get(), &desc, view);
    });
}

bool FrameConverter::convert(ID3D11Texture2D* source, RECT region, ID3D11Texture2D* target) {
    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    const Size sourceSize{static_cast<int>(sourceDesc.Width), static_cast<int>(sourceDesc.Height)};

    // While a resize settles the region can extend past the capture buffer.
    const RECT bounds{0, 0, static_cast<LONG>(sourceDesc.Width), static_cast<LONG>(sourceDesc.Height)};
    RECT sourceRect{};
    if (!IntersectRect(&sourceRect, &region, &bounds)) return false;
    const int width = sourceRect.right - sourceRect.left;
    const int height = sourceRect.bottom - sourceRect.top;
    // Letterbox: the output size is fixed for the session, the window may not be.
    const double scale = std::min(static_cast<double>(output_.width) / width, static_cast<double>(output_.height) / height);
    const int destWidth = std::min(output_.width, static_cast<int>(width * scale + 0.5) & ~1);
    const int destHeight = std::min(output_.height, static_cast<int>(height * scale + 0.5) & ~1);
    const int left = ((output_.width - destWidth) / 2) & ~1;
    const int top = ((output_.height - destHeight) / 2) & ~1;
    const RECT destRect{left, top, left + destWidth, top + destHeight};

    // Shrinking goes through the downscaler, and the video processor then converts its output at 1:1. Native size,
    // upscaling and a source shaders cannot read keep the video processor's own scaling.
    // A window with an odd size loses a pixel to even rounding; an output that holds the even-rounded region is native
    // size, not shrinking.
    const bool native = output_.width >= (width & ~1) && output_.height >= (height & ~1);
    // Not native, so the letterbox scale is below 1 and neither axis grows.
    const bool shrinks = !native && destWidth > 0 && destHeight > 0;
    auto* scaledInput = shrinks ? downscale(source, sourceRect, destRect) : nullptr;
    const bool downscaled = scaledInput != nullptr;
    if (downscaled) sourceRect = destRect;
    return scaledVideoFrame(downscaled, [&] {
        return processVideoFrame([&] {
            if (downscaled) return scaledInput;
            prepare(sourceSize);
            return inputView(source);
        }, [&] {
            // Output views depend on the chosen enumerator too: refusal must fall back just like a failed blit.
            return cachedView(outputs_, target, unlimited, [&](ID3D11VideoProcessorOutputView** view) {
                D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc{};
                desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
                return videoDevice_->CreateVideoProcessorOutputView(target, enumerator_.get(), &desc, view);
            });
        }, [&](auto* input, auto* output) {
            blit(processor_.get(), input, output, sourceRect, destRect);
            return true;
        }, [&](HRESULT error) {
            // The 1:1 path may be refused while the original-size processor still supports P010; its failure retries
            // through the original path below, which classifies.
            if (!downscaled) noteFailure(error, source, sourceSize, sourceRect, destRect);
        });
    }, [&] { return SUCCEEDED(device_->GetDeviceRemovedReason()); }, [&](HRESULT error) {
        dropDownscaler(error, 2);
        return convert(source, region, target); // Disabled for this session: exactly one legacy retry.
    });
}

} // namespace refarter::capture
