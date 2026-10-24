#include "surface_pool.h"

namespace refarter::capture {

struct SurfacePool::Releaser : winrt::implements<Releaser, IMFAsyncCallback> {
    std::mutex mutex;
    SurfacePool* owner = nullptr;

    STDMETHODIMP GetParameters(DWORD*, DWORD*) noexcept override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult* result) noexcept override {
        winrt::com_ptr<IUnknown> object;
        if (FAILED(result->GetObject(object.put()))) return S_OK;
        auto sample = object.try_as<IMFSample>();
        std::lock_guard lock(mutex);
        if (owner && sample) owner->returned(sample.get());
        return S_OK;
    }
};

SurfacePool::SurfacePool(const FrameConverter& converter, int count) : releaser_(winrt::make_self<Releaser>()) {
    releaser_->owner = this;
    for (int i = 0; i < count; ++i) {
        Slot slot;
        slot.texture = converter.createTarget();
        winrt::com_ptr<IMFTrackedSample> tracked;
        winrt::check_hresult(MFCreateTrackedSample(tracked.put()));
        auto sample = tracked.as<IMFSample>();
        winrt::com_ptr<IMFMediaBuffer> buffer;
        winrt::check_hresult(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), slot.texture.get(), 0, FALSE, buffer.put()));
        DWORD length = 0;
        if (auto planar = buffer.try_as<IMF2DBuffer>(); planar && SUCCEEDED(planar->GetContiguousLength(&length))) {
            buffer->SetCurrentLength(length);
        }
        winrt::check_hresult(sample->AddBuffer(buffer.get()));
        slot.identity = sample.get();
        slot.free = sample;
        slots_.push_back(std::move(slot));
    }
}

SurfacePool::~SurfacePool() {
    std::lock_guard lock(releaser_->mutex);
    releaser_->owner = nullptr;
}

std::optional<SurfacePool::Surface> SurfacePool::acquire() {
    std::lock_guard lock(mutex_);
    for (auto& slot : slots_) {
        if (!slot.free) continue;
        // One-shot: the callback fires when the last reference is released.
        winrt::check_hresult(slot.free.as<IMFTrackedSample>()->SetAllocator(releaser_.get(), nullptr));
        return Surface{slot.texture.get(), std::move(slot.free)};
    }
    return std::nullopt;
}

void SurfacePool::returned(IMFSample* sample) {
    std::lock_guard lock(mutex_);
    for (auto& slot : slots_) {
        if (slot.identity == sample) slot.free.copy_from(sample);
    }
}

} // namespace refarter::capture
