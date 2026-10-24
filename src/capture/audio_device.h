#pragma once

#include <string>
#include <vector>

namespace refarter::capture {

// What one audio track records: nothing, whatever Windows uses as the default
// device (following it when it changes), or one fixed endpoint. A fixed
// endpoint that is missing is waited for; it never falls back to another one.
struct AudioDevice {
    enum class Mode { Off, SystemDefault, Fixed };
    Mode mode = Mode::Off;
    std::wstring id;   // Windows endpoint ID, stable across restarts; Fixed only.
    std::wstring name; // Friendly name when it was chosen, shown while it is missing.

    bool enabled() const { return mode != Mode::Off; }
    bool operator==(const AudioDevice&) const = default;
};

// One active endpoint, as listed when a device menu opens.
struct AudioEndpoint {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

// Menu labels in list order. Identical names (two of the same headset) get
// " · 2", " · 3"... so each row stays distinguishable; IDs stay the identity.
inline std::vector<std::wstring> endpointLabels(const std::vector<AudioEndpoint>& endpoints) {
    std::vector<std::wstring> labels;
    for (size_t i = 0; i < endpoints.size(); ++i) {
        auto label = endpoints[i].name.empty() ? std::wstring(L"unnamed device") : endpoints[i].name;
        size_t same = 0, before = 0;
        for (size_t j = 0; j < endpoints.size(); ++j) {
            if (endpoints[j].name != endpoints[i].name) continue;
            ++same;
            if (j < i) ++before;
        }
        if (same > 1) label += L" · " + std::to_wstring(before + 1);
        labels.push_back(std::move(label));
    }
    return labels;
}

} // namespace refarter::capture
