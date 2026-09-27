// Thin wrapper over the ASUS Aura Sync COM SDK (ProgID "aura.sdk.1", served by the
// LightingService that Armoury Crate / Aura installs).
//
// Uses IDispatch late binding instead of #import-ing AuraServiceLib, so building needs no
// ASUS headers or type library -- only the runtime service has to be installed.
//
// Threading: every method must be called from the same thread, and that thread must have
// called CoInitializeEx. The proxy DLL owns a dedicated worker thread for this.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace luma {

struct AuraDeviceInfo {
    std::wstring name;
    uint32_t type = 0;
    int lightCount = 0;
    int width = 0;   // keyboards report a key matrix; 0 when not provided
    int height = 0;
};

const wchar_t* AuraDeviceTypeName(uint32_t type);

class AuraBridge {
public:
    AuraBridge();
    ~AuraBridge();
    AuraBridge(const AuraBridge&) = delete;
    AuraBridge& operator=(const AuraBridge&) = delete;

    // Return false to never write a device. Note SwitchMode() takes SDK control of every
    // Aura device, so an excluded device may freeze on its last color instead of running
    // its Armoury Crate effect until control is released.
    using DeviceFilter = std::function<bool(const AuraDeviceInfo&)>;

    // Creates the SDK object, calls SwitchMode() to take control and enumerates devices.
    // Returns false (with the HRESULT logged) if the Aura service isn't available.
    bool Connect(const DeviceFilter& filter = nullptr);

    // Hands lighting back to Armoury Crate (ReleaseControl) if requested, and drops all
    // COM references.
    void Disconnect(bool releaseControl);

    bool IsConnected() const;

    // Every enumerated device, including filtered-out ones.
    const std::vector<AuraDeviceInfo>& Devices() const;

    // Includes / excludes device `index` (into Devices()) from SetAll.
    void SetSelected(size_t index, bool selected);
    bool IsSelected(size_t index) const;

    // Sets every light on every selected device to `auraColor` (0x00BBGGRR) and applies.
    // Returns false on a COM failure (e.g. service restarted) -- caller should Disconnect
    // and try Connect again later.
    bool SetAll(uint32_t auraColor);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace luma
