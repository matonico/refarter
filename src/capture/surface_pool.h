#pragma once

#include "frame_converter.h"

#include <mfapi.h>
#include <mfidl.h>
#include <winrt/base.h>
#include <mutex>
#include <optional>
#include <vector>

namespace refarter::capture {

// A few NV12 encoder-input textures, each wrapped once in a tracked sample.
// A surface returns to the pool exactly when the encoder releases the sample,
// so it is never overwritten while still being read, and nothing is
// allocated per frame.
class SurfacePool {
public:
    struct Surface {
        ID3D11Texture2D* texture;
        winrt::com_ptr<IMFSample> sample; // Release (or hand off) to return it.
    };

    SurfacePool(const FrameConverter& converter, int count);
    ~SurfacePool();
    SurfacePool(const SurfacePool&) = delete;
    SurfacePool& operator=(const SurfacePool&) = delete;

    std::optional<Surface> acquire();

private:
    struct Releaser;
    struct Slot {
        winrt::com_ptr<ID3D11Texture2D> texture;
        IMFSample* identity;
        winrt::com_ptr<IMFSample> free; // Set while the pool owns the sample.
    };
    void returned(IMFSample* sample);

    std::mutex mutex_;
    std::vector<Slot> slots_;
    winrt::com_ptr<Releaser> releaser_;
};

} // namespace refarter::capture
