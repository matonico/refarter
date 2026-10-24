#include "frame_converter.h"

#include <algorithm>

namespace refarter::capture {

namespace {
// WGC recreates its two buffers on resize; a few entries cover the transition.
constexpr std::size_t maxCachedInputs = 4;
} // namespace

FrameConverter::FrameConverter(ID3D11Device* device, ID3D11DeviceContext* context, Size output, int fps)
    : output_(output), fps_(fps) {
    device_.copy_from(device);
    videoDevice_ = device_.as<ID3D11VideoDevice>();
    winrt::check_hresult(context->QueryInterface(IID_PPV_ARGS(videoContext_.put())));
}

winrt::com_ptr<ID3D11Texture2D> FrameConverter::createTarget() const {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(output_.width);
    desc.Height = static_cast<UINT>(output_.height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(device_->CreateTexture2D(&desc, nullptr, texture.put()));
    return texture;
}

void FrameConverter::prepare(Size sourceTexture) {
    if (processor_ && sourceTexture == processorInput_) return;
    inputs_.clear();
    outputs_.clear(); // Output views belong to the enumerator being replaced.
    processor_ = nullptr;
    enumerator_ = nullptr;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate = {static_cast<UINT>(fps_), 1};
    desc.InputWidth = static_cast<UINT>(sourceTexture.width);
    desc.InputHeight = static_cast<UINT>(sourceTexture.height);
    desc.OutputFrameRate = {static_cast<UINT>(fps_), 1};
    desc.OutputWidth = static_cast<UINT>(output_.width);
    desc.OutputHeight = static_cast<UINT>(output_.height);
    desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    winrt::check_hresult(videoDevice_->CreateVideoProcessorEnumerator(&desc, enumerator_.put()));
    UINT support = 0;
    winrt::check_hresult(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support));
    if (!(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) winrt::throw_hresult(E_NOTIMPL);
    winrt::check_hresult(videoDevice_->CreateVideoProcessor(enumerator_.get(), 0, processor_.put()));

    auto* processor = processor_.get();
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
    processorInput_ = sourceTexture;
}

template <typename View, typename Create>
View* FrameConverter::cached(std::vector<std::pair<ID3D11Texture2D*, winrt::com_ptr<View>>>& cache,
                             ID3D11Texture2D* texture, Create create) {
    for (auto& [key, view] : cache) if (key == texture) return view.get();
    winrt::com_ptr<View> view;
    winrt::check_hresult(create(view.put()));
    cache.emplace_back(texture, view);
    return view.get();
}

bool FrameConverter::convert(ID3D11Texture2D* source, RECT region, ID3D11Texture2D* target) {
    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    prepare({static_cast<int>(sourceDesc.Width), static_cast<int>(sourceDesc.Height)});

    if (inputs_.size() >= maxCachedInputs) inputs_.erase(inputs_.begin());
    auto* input = cached(inputs_, source, [&](ID3D11VideoProcessorInputView** view) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};
        desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        return videoDevice_->CreateVideoProcessorInputView(source, enumerator_.get(), &desc, view);
    });
    auto* output = cached(outputs_, target, [&](ID3D11VideoProcessorOutputView** view) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc{};
        desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        return videoDevice_->CreateVideoProcessorOutputView(target, enumerator_.get(), &desc, view);
    });

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

    auto* processor = processor_.get();
    videoContext_->VideoProcessorSetStreamSourceRect(processor, 0, TRUE, &sourceRect);
    videoContext_->VideoProcessorSetStreamDestRect(processor, 0, TRUE, &destRect);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input;
    winrt::check_hresult(videoContext_->VideoProcessorBlt(processor, output, 0, 1, &stream));
    return true;
}

} // namespace refarter::capture
