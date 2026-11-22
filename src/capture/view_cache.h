#pragma once

#include <d3d11.h>
#include <winrt/base.h>
#include <cstddef>
#include <utility>
#include <vector>

namespace refarter::capture {

// Views keep their textures alive, so cached raw keys cannot be reused.
template <typename View>
using ViewCache = std::vector<std::pair<ID3D11Texture2D*, winrt::com_ptr<View>>>;

// The view of `texture`, made on first use by `create(View**)`, which returns an HRESULT; a failure throws. Past
// `limit` entries the oldest is dropped.
template <typename View, typename Create>
View* cachedView(ViewCache<View>& cache, ID3D11Texture2D* texture, std::size_t limit, Create create) {
    for (auto& [key, view] : cache) if (key == texture) return view.get();
    winrt::com_ptr<View> view;
    winrt::check_hresult(create(view.put()));
    if (cache.size() >= limit) cache.erase(cache.begin());
    cache.emplace_back(texture, std::move(view));
    return cache.back().second.get();
}

} // namespace refarter::capture
