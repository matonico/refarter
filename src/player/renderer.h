#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <winrt/base.h>
#include <cstdint>
#include <string>

struct AVFrame;

namespace refarter::player {

// A BGRA texture the video processor can draw into and Direct2D can draw from.
winrt::com_ptr<ID3D11Texture2D> createPictureTexture(ID3D11Device* device, UINT width, UINT height);

// Turns decoded pictures (GPU surfaces or software planes) into BGRA textures
// of any size with the fixed-function video processor: colour conversion and
// scaling on the GPU. One converter per thread; the device is shared.
class VideoConverter {
public:
    explicit VideoConverter(ID3D11Device* device);
    // Draws `frame` scaled into all of `target` (BGRA, render-target bindable).
    bool convert(const AVFrame* frame, ID3D11Texture2D* target);

private:
    bool prepare(UINT inWidth, UINT inHeight, UINT outWidth, UINT outHeight);
    ID3D11Texture2D* upload(const AVFrame* frame);

    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<ID3D11VideoDevice> video_;
    winrt::com_ptr<ID3D11VideoContext> videoContext_;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> enumerator_;
    winrt::com_ptr<ID3D11VideoProcessor> processor_;
    UINT sizes_[4]{};
    // Software pictures: staged on the CPU, then copied to a GPU texture.
    winrt::com_ptr<ID3D11Texture2D> staging_, uploaded_;
    DXGI_FORMAT uploadFormat_ = DXGI_FORMAT_UNKNOWN;
    UINT uploadWidth_ = 0, uploadHeight_ = 0;
    bool uploadBindable_ = false; // Set once the flagless upload texture was refused as a processor input.
};

// The player's surface: a composition swap chain on the player's child window,
// drawn with Direct2D (interface) and the video processor (pictures). Owned by
// the player's engine thread; nothing renders while no clip is open.
class Renderer {
public:
    bool create(HWND window, std::wstring& error);
    bool resize(UINT width, UINT height);

    ID3D11Device* device() const { return device_.get(); }
    IDWriteFactory* text() const { return write_.get(); }
    ID2D1Factory1* factory() const { return factory_.get(); }

    // The current picture as a bitmap of exactly width × height pixels.
    ID2D1Bitmap1* picture(const AVFrame* frame, UINT width, UINT height);
    // Wraps a BGRA texture (a thumbnail) as a bitmap for drawing.
    winrt::com_ptr<ID2D1Bitmap1> wrap(ID3D11Texture2D* texture);

    ID2D1DeviceContext* begin();
    bool end(); // Presents on the next vertical blank.
    // The graphics device was removed or reset (seen by resize() or end()): nothing more can be drawn. There is no rebuild.
    bool lost() const { return lost_; }

private:
    bool target();
    bool succeeded(HRESULT result); // Notes a lost device.
    void lose();
    bool lost_ = false;

    HWND window_ = nullptr;
    UINT width_ = 0, height_ = 0;
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<IDXGISwapChain1> swapChain_;
    winrt::com_ptr<IDCompositionDevice> composition_;
    winrt::com_ptr<IDCompositionTarget> compositionTarget_;
    winrt::com_ptr<IDCompositionVisual> visual_;
    winrt::com_ptr<ID2D1Factory1> factory_;
    winrt::com_ptr<ID2D1Device> d2d_;
    winrt::com_ptr<ID2D1DeviceContext> context_;
    winrt::com_ptr<ID2D1Bitmap1> backBuffer_;
    winrt::com_ptr<IDWriteFactory> write_;
    std::unique_ptr<VideoConverter> converter_;
    winrt::com_ptr<ID3D11Texture2D> pictureTexture_;
    winrt::com_ptr<ID2D1Bitmap1> pictureBitmap_;
    const AVFrame* pictureFrame_ = nullptr;
    std::int64_t pictureStamp_ = 0;
};

} // namespace refarter::player
