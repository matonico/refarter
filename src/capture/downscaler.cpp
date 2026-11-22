#include "downscaler.h"

#include "scale_ps.h"
#include "scale_vs.h"

#include <algorithm>
#include <cstring>

namespace refarter::capture {

namespace {
// WGC recreates its two buffers on resize; a few entries cover the transition.
constexpr std::size_t maxCachedSources = 4;

// The immediate context is shared with the encoder; one frame's state changes and draws go in one piece.
class DeviceLock {
public:
    explicit DeviceLock(ID3D10Multithread* lock) : lock_(lock) { if (lock_) lock_->Enter(); }
    ~DeviceLock() { if (lock_) lock_->Leave(); }
    DeviceLock(const DeviceLock&) = delete;
    DeviceLock& operator=(const DeviceLock&) = delete;

private:
    ID3D10Multithread* lock_;
};
} // namespace

Downscaler::Downscaler(ID3D11Device* device, ID3D11DeviceContext* context, Size output, DXGI_FORMAT format)
    : output_(output), format_(format) {
    device_.copy_from(device);
    context_.copy_from(context);
    lock_ = device_.try_as<ID3D10Multithread>();
    winrt::check_hresult(device->CreateVertexShader(scaleVertexShader, sizeof(scaleVertexShader), nullptr, vertexShader_.put()));
    winrt::check_hresult(device->CreatePixelShader(scalePixelShader, sizeof(scalePixelShader), nullptr, pixelShader_.put()));
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(PassConstants);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    for (auto& buffer : constants_) winrt::check_hresult(device->CreateBuffer(&desc, nullptr, buffer.put()));
}

winrt::com_ptr<ID3D11Texture2D> Downscaler::createTexture(Size size, DXGI_FORMAT format, UINT bindFlags) const {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(size.width);
    desc.Height = static_cast<UINT>(size.height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bindFlags;
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(device_->CreateTexture2D(&desc, nullptr, texture.put()));
    return texture;
}

void Downscaler::upload(int pass, const PassConstants& constants) {
    if (hasUploaded_[pass] && std::memcmp(&uploaded_[pass], &constants, sizeof(constants)) == 0) return;
    context_->UpdateSubresource(constants_[pass].get(), 0, nullptr, &constants, 0, 0);
    uploaded_[pass] = constants;
    hasUploaded_[pass] = true;
}

void Downscaler::draw(ID3D11ShaderResourceView* input, ID3D11RenderTargetView* output, const RECT& area,
                      ID3D11Buffer* constants) {
    // The target first: binding it unbinds it as an input, which the second pass's input was in the first.
    context_->OMSetRenderTargets(1, &output, nullptr);
    context_->PSSetShaderResources(0, 1, &input);
    context_->PSSetConstantBuffers(0, 1, &constants);
    const D3D11_VIEWPORT viewport{static_cast<float>(area.left), static_cast<float>(area.top),
                                  static_cast<float>(area.right - area.left), static_cast<float>(area.bottom - area.top),
                                  0.f, 1.f};
    context_->RSSetViewports(1, &viewport);
    context_->Draw(3, 0);
}

bool Downscaler::scale(ID3D11Texture2D* source, const RECT& from, const RECT& to) {
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (!(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return false;
    ID3D11ShaderResourceView* input = nullptr;
    try {
        input = cachedView(sources_, source, maxCachedSources, [&](ID3D11ShaderResourceView** view) {
            return device_->CreateShaderResourceView(source, nullptr, view);
        });
    } catch (const winrt::hresult_error&) {
        return false;
    }

    const int width = from.right - from.left;
    const int height = from.bottom - from.top;
    const int scaledWidth = to.right - to.left;
    const int scaledHeight = to.bottom - to.top;
    // A failure here throws; the converter then drops this downscaler, so half-made members are never used.
    if (!target_) {
        // The video processor's input, so no shader-resource binding (see target()).
        target_ = createTexture(output_, format_, D3D11_BIND_RENDER_TARGET);
        winrt::check_hresult(device_->CreateRenderTargetView(target_.get(), nullptr, targetView_.put()));
    }
    if (betweenSize_.width < scaledWidth || betweenSize_.height < height) {
        const Size size{std::max(betweenSize_.width, scaledWidth), std::max(betweenSize_.height, height)};
        betweenTarget_ = nullptr;
        betweenView_ = nullptr;
        betweenSize_ = {};
        // Float, so the first pass's overshoot and fractions survive to the second.
        between_ = createTexture(size, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        winrt::check_hresult(device_->CreateRenderTargetView(between_.get(), nullptr, betweenTarget_.put()));
        winrt::check_hresult(device_->CreateShaderResourceView(between_.get(), nullptr, betweenView_.put()));
        betweenSize_ = size;
    }

    // Across: source rows into `between_` at the output's width. Down: `between_` columns into `to`.
    const PassConstants across{{0.f, 0.f}, static_cast<float>(from.left), static_cast<float>(width) / scaledWidth,
                               static_cast<int>(from.top), static_cast<int>(from.left), static_cast<int>(from.right) - 1, 0};
    const PassConstants down{{static_cast<float>(to.left), static_cast<float>(to.top)}, 0.f,
                             static_cast<float>(height) / scaledHeight, 0, 0, height - 1, 1};

    DeviceLock lock(lock_.get());
    upload(0, across);
    upload(1, down);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.get(), nullptr, 0);
    context_->RSSetState(nullptr);
    context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context_->OMSetDepthStencilState(nullptr, 0);
    draw(input, betweenTarget_.get(), {0, 0, scaledWidth, height}, constants_[0].get());
    draw(betweenView_.get(), targetView_.get(), to, constants_[1].get());
    // Unbound, so the video processor may read the target, WGC may reuse its buffer and nothing else on this context
    // draws with these shaders. The topology and viewport stay set; anything else drawing here must set its own.
    ID3D11ShaderResourceView* noInput = nullptr;
    ID3D11Buffer* noConstants = nullptr;
    context_->PSSetShaderResources(0, 1, &noInput);
    context_->PSSetConstantBuffers(0, 1, &noConstants);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    context_->VSSetShader(nullptr, nullptr, 0);
    context_->PSSetShader(nullptr, nullptr, 0);
    return true;
}

} // namespace refarter::capture
