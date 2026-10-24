#include "player/renderer.h"
#include "capture/gpu.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cstring>

namespace refarter::player {

namespace {
winrt::com_ptr<ID3D11Texture2D> createTexture(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format,
                                              D3D11_USAGE usage, UINT bind, UINT cpu) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = usage;
    desc.BindFlags = bind;
    desc.CPUAccessFlags = cpu;
    winrt::com_ptr<ID3D11Texture2D> texture;
    return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, texture.put())) ? texture : nullptr;
}

// The graphics device was removed, reset or its drawing target must be recreated.
bool deviceError(HRESULT result) {
    return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET || result == D2DERR_RECREATE_TARGET;
}
} // namespace

winrt::com_ptr<ID3D11Texture2D> createPictureTexture(ID3D11Device* device, UINT width, UINT height) {
    return createTexture(device, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0);
}

VideoConverter::VideoConverter(ID3D11Device* device) {
    device_.copy_from(device);
    device->GetImmediateContext(context_.put());
    video_ = device_.try_as<ID3D11VideoDevice>();
    videoContext_ = context_.try_as<ID3D11VideoContext>();
}

bool VideoConverter::prepare(UINT inWidth, UINT inHeight, UINT outWidth, UINT outHeight) {
    const UINT sizes[] = {inWidth, inHeight, outWidth, outHeight};
    if (processor_ && std::equal(std::begin(sizes), std::end(sizes), sizes_)) return true;
    processor_ = nullptr;
    enumerator_ = nullptr;
    if (!video_) return false;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate = {60, 1};
    desc.InputWidth = inWidth;
    desc.InputHeight = inHeight;
    desc.OutputFrameRate = {60, 1};
    desc.OutputWidth = outWidth;
    desc.OutputHeight = outHeight;
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(video_->CreateVideoProcessorEnumerator(&desc, enumerator_.put())) ||
        FAILED(video_->CreateVideoProcessor(enumerator_.get(), 0, processor_.put()))) {
        processor_ = nullptr;
        return false;
    }
    std::copy(std::begin(sizes), std::end(sizes), sizes_);
    // Fixed processing only: no driver "enhancements" that would differ by GPU.
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(processor_.get(), 0, FALSE);
    videoContext_->VideoProcessorSetStreamFrameFormat(processor_.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE output{};
    output.RGB_Range = 0; // Full-range RGB for the screen.
    videoContext_->VideoProcessorSetOutputColorSpace(processor_.get(), &output);
    return true;
}

ID3D11Texture2D* VideoConverter::upload(const AVFrame* frame) {
    const auto format = static_cast<AVPixelFormat>(frame->format);
    const bool tenBit = format == AV_PIX_FMT_YUV420P10LE || format == AV_PIX_FMT_P010LE;
    const bool planar = format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUV420P10LE;
    if (!planar && format != AV_PIX_FMT_NV12 && format != AV_PIX_FMT_P010LE) return nullptr;
    const DXGI_FORMAT dxgi = tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    const UINT width = (static_cast<UINT>(frame->width) + 1) & ~1u, height = (static_cast<UINT>(frame->height) + 1) & ~1u;
    if (!uploaded_ || uploadFormat_ != dxgi || uploadWidth_ != width || uploadHeight_ != height) {
        staging_ = createTexture(device_.get(), width, height, dxgi, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_WRITE);
        // No bind flags: the video processor rejects a shader-bindable NV12 upload on some GPUs
        // (E_INVALIDARG), which left every software-decoded picture black.
        uploaded_ = createTexture(device_.get(), width, height, dxgi, D3D11_USAGE_DEFAULT, uploadBindable_ ? D3D11_BIND_SHADER_RESOURCE : 0, 0);
        if (!uploaded_) uploaded_ = createTexture(device_.get(), width, height, dxgi, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0);
        if (!staging_ || !uploaded_) { staging_ = uploaded_ = nullptr; return nullptr; }
        uploadFormat_ = dxgi;
        uploadWidth_ = width;
        uploadHeight_ = height;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging_.get(), 0, D3D11_MAP_WRITE, 0, &mapped))) return nullptr;
    auto* luma = static_cast<std::uint8_t*>(mapped.pData);
    auto* chroma = luma + static_cast<std::size_t>(mapped.RowPitch) * height;
    const int w = frame->width, h = frame->height, sample = tenBit ? 2 : 1;
    for (int y = 0; y < h; ++y) {
        auto* row = luma + static_cast<std::size_t>(y) * mapped.RowPitch;
        const auto* source = frame->data[0] + static_cast<std::ptrdiff_t>(y) * frame->linesize[0];
        if (format == AV_PIX_FMT_YUV420P10LE) {
            // 10 bits in the low bits become P010's high bits.
            for (int x = 0; x < w; ++x)
                reinterpret_cast<std::uint16_t*>(row)[x] = static_cast<std::uint16_t>(reinterpret_cast<const std::uint16_t*>(source)[x] << 6);
        } else std::memcpy(row, source, static_cast<std::size_t>(w) * sample);
    }
    for (int y = 0; y < (h + 1) / 2; ++y) {
        auto* row = chroma + static_cast<std::size_t>(y) * mapped.RowPitch;
        if (!planar) {
            std::memcpy(row, frame->data[1] + static_cast<std::ptrdiff_t>(y) * frame->linesize[1], static_cast<std::size_t>((w + 1) / 2) * 2 * sample);
            continue;
        }
        const auto* u = frame->data[1] + static_cast<std::ptrdiff_t>(y) * frame->linesize[1];
        const auto* v = frame->data[2] + static_cast<std::ptrdiff_t>(y) * frame->linesize[2];
        for (int x = 0; x < (w + 1) / 2; ++x) {
            if (tenBit) {
                auto* out = reinterpret_cast<std::uint16_t*>(row);
                out[x * 2] = static_cast<std::uint16_t>(reinterpret_cast<const std::uint16_t*>(u)[x] << 6);
                out[x * 2 + 1] = static_cast<std::uint16_t>(reinterpret_cast<const std::uint16_t*>(v)[x] << 6);
            } else {
                row[x * 2] = u[x];
                row[x * 2 + 1] = v[x];
            }
        }
    }
    context_->Unmap(staging_.get(), 0);
    context_->CopyResource(uploaded_.get(), staging_.get());
    return uploaded_.get();
}

bool VideoConverter::convert(const AVFrame* frame, ID3D11Texture2D* target) {
    if (!frame || !target || !videoContext_) return false;
    D3D11_TEXTURE2D_DESC out{};
    target->GetDesc(&out);
    ID3D11Texture2D* input = nullptr;
    UINT slice = 0;
    if (frame->format == AV_PIX_FMT_D3D11) {
        input = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
        slice = static_cast<UINT>(reinterpret_cast<std::intptr_t>(frame->data[1]));
    } else input = upload(frame);
    if (!input || !prepare(static_cast<UINT>(frame->width), static_cast<UINT>(frame->height), out.Width, out.Height)) return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputDesc.Texture2D.ArraySlice = slice;
    winrt::com_ptr<ID3D11VideoProcessorInputView> inputView;
    if (frame->format != AV_PIX_FMT_D3D11) {
        UINT support = 0; // Where the driver can say, an input format the processor cannot read is not tried.
        const auto format = input == uploaded_.get() ? uploadFormat_ : DXGI_FORMAT_UNKNOWN;
        if (format != DXGI_FORMAT_UNKNOWN && SUCCEEDED(enumerator_->CheckVideoProcessorFormat(format, &support)) &&
            !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) return false;
    }
    if (FAILED(video_->CreateVideoProcessorInputView(input, enumerator_.get(), &inputDesc, inputView.put()))) {
        if (frame->format == AV_PIX_FMT_D3D11 || input != uploaded_.get() || uploadBindable_) return false;
        // Some GPUs want the shader-bindable flag instead: recreate the upload texture with it and try once more.
        uploadBindable_ = true;
        uploaded_ = createTexture(device_.get(), uploadWidth_, uploadHeight_, uploadFormat_, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0);
        if (!uploaded_) return false;
        context_->CopyResource(uploaded_.get(), staging_.get());
        input = uploaded_.get();
        if (FAILED(video_->CreateVideoProcessorInputView(input, enumerator_.get(), &inputDesc, inputView.put()))) return false;
    }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    winrt::com_ptr<ID3D11VideoProcessorOutputView> outputView;
    if (FAILED(video_->CreateVideoProcessorOutputView(target, enumerator_.get(), &outputDesc, outputView.put()))) return false;

    // BT.709 unless the file says BT.601 (or is standard definition and silent).
    const bool bt601 = frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M ||
        (frame->colorspace == AVCOL_SPC_UNSPECIFIED && frame->height < 720);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE colour{};
    colour.YCbCr_Matrix = bt601 ? 0 : 1;
    colour.Nominal_Range = frame->color_range == AVCOL_RANGE_JPEG ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 :
        D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext_->VideoProcessorSetStreamColorSpace(processor_.get(), 0, &colour);
    const RECT source{0, 0, frame->width, frame->height};
    const RECT destination{0, 0, static_cast<LONG>(out.Width), static_cast<LONG>(out.Height)};
    videoContext_->VideoProcessorSetStreamSourceRect(processor_.get(), 0, TRUE, &source);
    videoContext_->VideoProcessorSetStreamDestRect(processor_.get(), 0, TRUE, &destination);
    videoContext_->VideoProcessorSetOutputTargetRect(processor_.get(), TRUE, &destination);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.get();
    return SUCCEEDED(videoContext_->VideoProcessorBlt(processor_.get(), outputView.get(), 0, 1, &stream));
}

bool Renderer::create(HWND window, std::wstring& error) {
    window_ = window;
    RECT client{};
    GetClientRect(window, &client);
    width_ = std::max<UINT>(1, client.right);
    height_ = std::max<UINT>(1, client.bottom);
    try {
        // The GPU showing the window decodes and presents; no copies between adapters.
        auto gpu = capture::createGpuForMonitor(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST));
        device_ = gpu.device;
        const auto dxgi = device_.as<IDXGIDevice>();
        winrt::com_ptr<IDXGIAdapter> adapter;
        winrt::check_hresult(dxgi->GetAdapter(adapter.put()));
        winrt::com_ptr<IDXGIFactory2> factory;
        winrt::check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width_;
        desc.Height = height_;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED; // Rounded corners blend with the window.
        winrt::check_hresult(factory->CreateSwapChainForComposition(device_.get(), &desc, nullptr, swapChain_.put()));
        winrt::check_hresult(DCompositionCreateDevice(dxgi.get(), __uuidof(IDCompositionDevice), composition_.put_void()));
        winrt::check_hresult(composition_->CreateTargetForHwnd(window, TRUE, compositionTarget_.put()));
        winrt::check_hresult(composition_->CreateVisual(visual_.put()));
        winrt::check_hresult(visual_->SetContent(swapChain_.get()));
        winrt::check_hresult(compositionTarget_->SetRoot(visual_.get()));
        D2D1_FACTORY_OPTIONS options{};
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options, factory_.put_void()));
        winrt::check_hresult(factory_->CreateDevice(dxgi.get(), d2d_.put()));
        winrt::check_hresult(d2d_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, context_.put()));
        winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(write_.put())));
        converter_ = std::make_unique<VideoConverter>(device_.get());
        if (!target()) winrt::throw_hresult(E_FAIL);
        winrt::check_hresult(composition_->Commit());
        return true;
    } catch (...) {
        error = L"the graphics card could not be prepared for playback.";
        return false;
    }
}

bool Renderer::succeeded(HRESULT result) {
    if (SUCCEEDED(result)) return true;
    if (deviceError(result)) lose();
    return false;
}

// No full device rebuild: the surface stops showing its last picture, so the clip's error text shows instead.
void Renderer::lose() {
    if (lost_) return;
    lost_ = true;
    if (visual_) visual_->SetContent(nullptr);
    if (composition_) composition_->Commit();
}

bool Renderer::target() {
    winrt::com_ptr<IDXGISurface> surface;
    if (!succeeded(swapChain_->GetBuffer(0, __uuidof(IDXGISurface), surface.put_void()))) return false;
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    backBuffer_ = nullptr;
    if (!succeeded(context_->CreateBitmapFromDxgiSurface(surface.get(), &properties, backBuffer_.put()))) return false;
    context_->SetTarget(backBuffer_.get());
    return true;
}

bool Renderer::resize(UINT width, UINT height) {
    width = std::max<UINT>(1, width);
    height = std::max<UINT>(1, height);
    if (width == width_ && height == height_) return true;
    context_->SetTarget(nullptr);
    backBuffer_ = nullptr;
    if (!succeeded(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) return false;
    width_ = width;
    height_ = height;
    return target();
}

winrt::com_ptr<ID2D1Bitmap1> Renderer::wrap(ID3D11Texture2D* texture) {
    winrt::com_ptr<IDXGISurface> surface;
    winrt::com_ptr<ID2D1Bitmap1> bitmap;
    if (!texture || FAILED(texture->QueryInterface(__uuidof(IDXGISurface), surface.put_void()))) return nullptr;
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    return SUCCEEDED(context_->CreateBitmapFromDxgiSurface(surface.get(), &properties, bitmap.put())) ? bitmap : nullptr;
}

ID2D1Bitmap1* Renderer::picture(const AVFrame* frame, UINT width, UINT height) {
    if (!frame || !width || !height) return nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    if (pictureTexture_) pictureTexture_->GetDesc(&desc);
    const bool resized = !pictureTexture_ || desc.Width != width || desc.Height != height;
    if (resized) {
        pictureTexture_ = createPictureTexture(device_.get(), width, height);
        pictureBitmap_ = wrap(pictureTexture_.get());
    }
    if (!pictureBitmap_) return nullptr;
    // Convert once per picture and size; redraws of the interface reuse it.
    if (resized || frame != pictureFrame_ || frame->best_effort_timestamp != pictureStamp_) {
        if (!converter_->convert(frame, pictureTexture_.get())) return nullptr;
        pictureFrame_ = frame;
        pictureStamp_ = frame->best_effort_timestamp;
    }
    return pictureBitmap_.get();
}

ID2D1DeviceContext* Renderer::begin() {
    context_->BeginDraw();
    context_->SetTransform(D2D1::Matrix3x2F::Identity());
    return context_.get();
}

bool Renderer::end() {
    const bool drawn = succeeded(context_->EndDraw());
    const bool presented = succeeded(swapChain_->Present(1, 0));
    // A removed device can also show only here, on the device itself.
    if (device_ && FAILED(device_->GetDeviceRemovedReason())) lose();
    return drawn && presented;
}

} // namespace refarter::player
