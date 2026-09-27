// A way to reach Aura hardware. Two implementations:
//   - UsbAura    (src/hardware/aura-usb): the motherboard's Aura USB controller, directly. Default.
//   - AuraBridge (src/hardware): ASUS's Aura SDK (COM). Opt-in: on current Armoury Crate
//     versions it reports no devices, and one of its device plug-ins can fail-fast the
//     process that loads it.
// Used from a single worker thread (AuraMirror).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace luma {

struct AuraDeviceInfo {
    std::wstring name;
    uint32_t type = 0;  // Aura device type code (0x00010000 motherboard, ...)
    int lightCount = 0;
    int width = 0;  // keyboards report a key matrix; 0 when not provided
    int height = 0;
};

class LightingBackend {
public:
    virtual ~LightingBackend() = default;

    virtual bool Connect() = 0;
    // `releaseControl`: hand the lights back to Armoury Crate as far as the backend can.
    virtual void Disconnect(bool releaseControl) = 0;
    virtual bool IsConnected() const = 0;

    virtual const std::vector<AuraDeviceInfo>& Devices() const = 0;
    virtual void SetSelected(size_t index, bool selected) = 0;

    // Shows `auraColor` (0x00BBGGRR) on every selected device. False on an I/O failure:
    // the caller disconnects and reconnects later.
    virtual bool SetAll(uint32_t auraColor) = 0;
};

}  // namespace luma
