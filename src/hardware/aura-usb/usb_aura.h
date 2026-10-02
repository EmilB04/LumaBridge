// LightingBackend for the motherboard's Aura USB controller: talks to the hardware directly,
// without Armoury Crate or the Aura SDK. Devices are the controller's channels (the board's
// own LEDs and each ARGB header).
#pragma once

#include <cstdint>
#include <vector>

#include "aura_usb.h"
#include "lighting_backend.h"

namespace luma::aurausb {

// USB product IDs of ASUS motherboards' "AURA LED Controller" (the same family: firmware and
// configuration table as in aura_usb_protocol.h); which one a board has depends on its
// generation. 0x1939: ROG STRIX B550-F and others of its time.
inline const std::vector<uint16_t>& MainboardProductIds() {
    static const std::vector<uint16_t> k{0x1939, 0x18F3, 0x19AF, 0x1AA6, 0x1BED};
    return k;
}

// Lists the Aura devices of every controller found, WITHOUT taking control (only the
// firmware / configuration are read; the lights keep whatever they are showing).
std::vector<AuraDeviceInfo> ProbeDevices(const std::vector<uint16_t>& productIds = MainboardProductIds(),
                                         int argbLeds = kDefaultArgbLeds);

class UsbAura : public LightingBackend {
public:
    // USB product ids of Aura motherboard controllers to look for.
    explicit UsbAura(std::vector<uint16_t> productIds = MainboardProductIds(), int argbLeds = kDefaultArgbLeds);

    bool Connect() override;
    void Disconnect(bool releaseControl) override;
    bool IsConnected() const override { return dev_.IsOpen(); }
    const std::vector<AuraDeviceInfo>& Devices() const override { return infos_; }
    void SetSelected(size_t index, bool selected) override;
    bool SetAll(uint32_t auraColor) override;
    bool SetFrames(const std::vector<std::vector<Rgb>>& frames) override;
    void ReenterDirectMode() override { directMode_ = false; }

private:
    bool EnterDirectMode();

    std::vector<uint16_t> pids_;
    int argbLeds_;
    Device dev_;
    std::vector<UsbChannel> channels_;
    std::vector<bool> selected_;
    std::vector<AuraDeviceInfo> infos_;
    bool directMode_ = false;
};

}  // namespace luma::aurausb
