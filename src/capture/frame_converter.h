#pragma once

#include "video_settings.h"

#include <d3d11_1.h>
#include <winrt/base.h>
#include <utility>
#include <vector>

namespace refarter::capture {

// Converts captured BGRA frames into NV12 encoder input in a single
// D3D11 video-processor pass: colour conversion, scaling and letterboxing.
// The GPU's fixed-function video path does the work; nothing touches the CPU.
class FrameConverter {
public:
    FrameConverter(ID3D11Device* device, ID3D11DeviceContext* context, Size output, int fps);

    winrt::com_ptr<ID3D11Texture2D> createTarget() const;
    // Returns false when the region lies outside the captured texture.
    bool convert(ID3D11Texture2D* source, RECT region, ID3D11Texture2D* target);

private:
    void prepare(Size sourceTexture);
    template <typename View, typename Create>
    View* cached(std::vector<std::pair<ID3D11Texture2D*, winrt::com_ptr<View>>>& cache, ID3D11Texture2D* texture, Create create);

    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11VideoDevice> videoDevice_;
    winrt::com_ptr<ID3D11VideoContext1> videoContext_;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> enumerator_;
    winrt::com_ptr<ID3D11VideoProcessor> processor_;
    // Views keep their textures alive, so cached raw keys cannot be reused.
    std::vector<std::pair<ID3D11Texture2D*, winrt::com_ptr<ID3D11VideoProcessorInputView>>> inputs_;
    std::vector<std::pair<ID3D11Texture2D*, winrt::com_ptr<ID3D11VideoProcessorOutputView>>> outputs_;
    Size output_;
    Size processorInput_;
    int fps_;
};

} // namespace refarter::capture
