#pragma once

#include "downscaler.h"
#include "video_settings.h"
#include "view_cache.h"

#include <d3d11_1.h>
#include <winrt/base.h>
#include <memory>

namespace refarter::capture {

// Converts captured BGRA frames into NV12 (or, for 10-bit encodes, P010) encoder input on the GPU; nothing touches
// the CPU. A frame that shrinks is first scaled by the Downscaler's shaders; the D3D11 video processor then does
// the colour conversion and letterboxing, and itself scales frames that keep their size or grow.
class FrameConverter {
public:
    // With `tenBit`, throws when the video processor cannot write P010 (E_NOTIMPL) or a P010 processor or output view
    // cannot be made, checked for `source` (the captured texture's size; the output size when empty) to `output`.
    FrameConverter(ID3D11Device* device, ID3D11DeviceContext* context, Size output, int fps, bool tenBit = false,
                   Size source = {});

    winrt::com_ptr<ID3D11Texture2D> createTarget() const;
    // Returns false when the region lies outside the captured texture.
    bool convert(ID3D11Texture2D* source, RECT region, ID3D11Texture2D* target);
    // A P010 step (processor, input/output view or conversion) failed where the same frame converted to NV12: the
    // recording must stop.
    bool tenBitRefused() const { return tenBitRefused_; }

private:
    void prepare(Size sourceTexture);
    void configure(ID3D11VideoProcessor* processor) const;
    void blit(ID3D11VideoProcessor* processor, ID3D11VideoProcessorInputView* input, ID3D11VideoProcessorOutputView* output,
              const RECT& sourceRect, const RECT& destRect) const;
    winrt::com_ptr<ID3D11Texture2D> createTarget(DXGI_FORMAT format) const;
    ID3D11VideoProcessorInputView* inputView(ID3D11Texture2D* texture);
    // Shrinks `from` into the downscaler's target at `to` and readies the processor to read it at 1:1. Returns its
    // input view, or null, from then on, once any step fails other than through a lost device, which throws.
    ID3D11VideoProcessorInputView* downscale(ID3D11Texture2D* source, const RECT& from, const RECT& to);
    void dropDownscaler(HRESULT code, int reason);
    // Marks tenBitRefused() when a P010 step failed but the whole frame converts to NV12. Never throws, so the original
    // error reaches the caller. Called only for the original-size path, never the downscaler's 1:1.
    void noteFailure(HRESULT error, ID3D11Texture2D* source, Size sourceTexture, const RECT& sourceRect, const RECT& destRect);

    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<ID3D11VideoDevice> videoDevice_;
    winrt::com_ptr<ID3D11VideoContext1> videoContext_;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> enumerator_;
    winrt::com_ptr<ID3D11VideoProcessor> processor_;
    ViewCache<ID3D11VideoProcessorInputView> inputs_;
    ViewCache<ID3D11VideoProcessorOutputView> outputs_;
    std::unique_ptr<Downscaler> downscaler_; // Made on the first frame that shrinks.
    bool downscalerFailed_ = false;
    bool oneToOneRefused_ = false; // The processor refused P010 at output to output, so the downscaler can't be used.
    DXGI_FORMAT scaledFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    Size output_;
    Size processorInput_;
    int fps_;
    DXGI_FORMAT format_;
    bool tenBitRefused_ = false;
};

} // namespace refarter::capture
