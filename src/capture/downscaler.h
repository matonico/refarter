#pragma once

#include "video_settings.h"
#include "view_cache.h"

#include <d3d11_1.h>
#include <winrt/base.h>

namespace refarter::capture {

// Shrinks a captured BGRA region with a two-pass Lanczos-3 filter (shaders/scale_ps.hlsl) into an RGB texture at
// the output size, which the video processor then only converts to YUV. Its driver-chosen scaler lost 2-6 VMAF at
// 1080p and 4-11 at 720p against Lanczos, except on a static menu (issue 179). Holds no texture until the first frame it scales.
class Downscaler {
public:
    // Throws when the shaders cannot be created; the converter then keeps the video processor's scaler.
    Downscaler(ID3D11Device* device, ID3D11DeviceContext* context, Size output, DXGI_FORMAT format);

    // Scales `from` in `source` into `to` in target(). Both axes must shrink or keep their size. Returns false,
    // drawing nothing, when shaders cannot read `source`; throws when a texture or other view cannot be made. Either way
    // the caller should stop using this object.
    bool scale(ID3D11Texture2D* source, const RECT& from, const RECT& to);
    // Render-target only: some GPUs refuse a shader-bindable video processor input (decisions.md).
    ID3D11Texture2D* target() const { return target_.get(); }

private:
    // Matches cbuffer ScalePass in shaders/scale_ps.hlsl.
    struct PassConstants {
        float viewOrigin[2];
        float sourceStart;
        float scale;
        int acrossStart;
        int low;
        int high;
        UINT vertical;
    };
    static_assert(sizeof(PassConstants) % 16 == 0);

    winrt::com_ptr<ID3D11Texture2D> createTexture(Size size, DXGI_FORMAT format, UINT bindFlags) const;
    // Uploads `constants` to the pass's buffer only when they changed since the last frame.
    void upload(int pass, const PassConstants& constants);
    void draw(ID3D11ShaderResourceView* input, ID3D11RenderTargetView* output, const RECT& viewport, ID3D11Buffer* constants);

    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<ID3D10Multithread> lock_;
    winrt::com_ptr<ID3D11VertexShader> vertexShader_;
    winrt::com_ptr<ID3D11PixelShader> pixelShader_;
    winrt::com_ptr<ID3D11Buffer> constants_[2]; // One per pass, so the second update never waits for the first draw.
    PassConstants uploaded_[2]{};
    bool hasUploaded_[2]{};
    // The first pass's output, RGBA16F: the output's width by the source region's height. It only grows.
    winrt::com_ptr<ID3D11Texture2D> between_;
    winrt::com_ptr<ID3D11RenderTargetView> betweenTarget_;
    winrt::com_ptr<ID3D11ShaderResourceView> betweenView_;
    Size betweenSize_{};
    winrt::com_ptr<ID3D11Texture2D> target_;
    winrt::com_ptr<ID3D11RenderTargetView> targetView_;
    ViewCache<ID3D11ShaderResourceView> sources_;
    Size output_;
    DXGI_FORMAT format_;
};

} // namespace refarter::capture
