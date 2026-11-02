#include "player/clip_preview.h"
#include "capture/gpu.h"
#include "player/media.h"
#include "player/renderer.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <winrt/base.h>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace refarter::player {
namespace {
// A CPU copy of the converted picture as a premultiplied top-down DIB (the video is opaque).
HBITMAP readBack(ID3D11DeviceContext* context, ID3D11Texture2D* staging, UINT width, UINT height) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) return nullptr;
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), static_cast<LONG>(width), -static_cast<LONG>(height), 1, 32, BI_RGB};
    void* bits = nullptr;
    const HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap) {
        for (UINT y = 0; y < height; ++y) {
            auto* row = static_cast<std::uint8_t*>(bits) + static_cast<std::size_t>(y) * width * 4;
            std::memcpy(row, static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch, width * 4);
            for (UINT x = 0; x < width; ++x) row[x * 4 + 3] = 255;
        }
    }
    context->Unmap(staging, 0);
    return bitmap;
}
} // namespace

ClipPreview::~ClipPreview() {
    {
        std::lock_guard lock(mutex_);
        exit_ = true;
    }
    work_.notify_all();
    if (worker_.joinable()) worker_.join();
    for (auto& picture : results_) DeleteObject(picture.bitmap);
}

std::uint64_t ClipPreview::request(std::filesystem::path clip, int count, UINT width, UINT height, HMONITOR monitor) {
    std::lock_guard lock(mutex_);
    pending_ = Request{++newest_, std::move(clip), count, width, height, monitor};
    if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
    work_.notify_all();
    return newest_;
}

void ClipPreview::cancel() {
    std::lock_guard lock(mutex_);
    ++newest_; // The running request is no longer the newest: it stops at its next picture.
    pending_.reset();
}

std::vector<ClipPreview::Picture> ClipPreview::take() {
    std::lock_guard lock(mutex_);
    return std::exchange(results_, {});
}

bool ClipPreview::current(std::uint64_t number) {
    std::lock_guard lock(mutex_);
    return !exit_ && number == newest_;
}

void ClipPreview::run() {
    // Below-normal CPU and I/O priority, as the library worker, so a game or a save is not slowed.
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    {
        capture::Gpu gpu;
        HMONITOR gpuMonitor = nullptr;
        for (;;) {
            Request request;
            {
                std::unique_lock lock(mutex_);
                // The GPU device is let go after half a minute without a preview (one timed wait, no polling), so an
                // idle Refarter does not keep it; the next hover makes it again.
                if (!work_.wait_for(lock, std::chrono::seconds(30), [this] { return exit_ || pending_; })) {
                    lock.unlock();
                    gpu = {};
                    gpuMonitor = nullptr;
                    lock.lock();
                    work_.wait(lock, [this] { return exit_ || pending_; });
                }
                if (exit_) break;
                request = std::move(*pending_);
                pending_.reset();
            }
            if (request.count <= 0 || !request.width || !request.height) continue;
            // The GPU showing the window, kept between requests while it stays the same.
            // A device removed by a GPU reset or driver update is made again, not reused.
            if (!gpu.device || request.monitor != gpuMonitor || gpu.device->GetDeviceRemovedReason() != S_OK) {
                try { gpu = capture::createGpuForMonitor(request.monitor); } catch (...) { gpu = {}; }
                gpuMonitor = request.monitor;
            }
            if (!gpu.device || !current(request.number)) continue;
            // Software decoding uses one thread: its workers would not inherit this thread's background priority, and
            // isolated keyframes gain nothing from frame threading.
            MediaFile file; // Closed when this request ends.
            if (!file.open(request.clip, {gpu.device.get(), false, softwareOnly(), 1}) || file.duration() <= 0) continue;
            VideoConverter converter(gpu.device.get());
            const auto target = createPictureTexture(gpu.device.get(), request.width, request.height);
            D3D11_TEXTURE2D_DESC copy{};
            copy.Width = request.width;
            copy.Height = request.height;
            copy.MipLevels = copy.ArraySize = 1;
            copy.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            copy.SampleDesc.Count = 1;
            copy.Usage = D3D11_USAGE_STAGING;
            copy.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            winrt::com_ptr<ID3D11Texture2D> staging;
            if (!target || FAILED(gpu.device->CreateTexture2D(&copy, nullptr, staging.put()))) continue;
            for (const int index : ClipPreview::decodeOrder(request.count)) {
                if (!current(request.number)) break;
                const auto picture = file.keyframe(timeOf(index, request.count, file.duration()));
                if (!picture || !converter.convert(picture.frame.get(), target.get())) continue;
                gpu.context->CopyResource(staging.get(), target.get());
                const HBITMAP bitmap = readBack(gpu.context.get(), staging.get(), request.width, request.height);
                if (!bitmap) continue;
                bool notify = false;
                {
                    std::lock_guard lock(mutex_);
                    notify = results_.empty();
                    results_.push_back({request.number, index, bitmap});
                }
                if (notify && ready_) ready_();
            }
        }
    }
    winrt::uninit_apartment();
}

} // namespace refarter::player
