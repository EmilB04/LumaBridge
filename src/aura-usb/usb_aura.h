// LightingBackend for the motherboard's Aura USB controller: talks to the hardware directly,
// without Armoury Crate or the Aura SDK. Devices are the controller's channels (the board's
// own LEDs and each ARGB header).
#pragma once

#include <cstdint>
#include <vector>

#include "aura_usb.h"
#include "lighting_backend.h"

namespace luma::aurausb {

class UsbAura : public LightingBackend {
public:
    // USB product ids of Aura motherboard controllers to look for.
    explicit UsbAura(std::vector<uint16_t> productIds = {0x1939}, int argbLeds = kDefaultArgbLeds);

    bool Connect() override;
    void Disconnect(bool releaseControl) override;
    bool IsConnected() const override { return dev_.IsOpen(); }
    const std::vector<AuraDeviceInfo>& Devices() const override { return infos_; }
    void SetSelected(size_t index, bool selected) override;
    bool SetAll(uint32_t auraColor) override;

private:
    bool EnterDirectMode();

    std::vector<uint16_t> pids_;
    int argbLeds_;
    Device dev_;
    std::vector<UsbChannel> channels_;
    std::vector<bool> selected_;
    std::vector<AuraDeviceInfo> infos_;
    uint64_t lastDirectModeAt_ = 0;
};

}  // namespace luma::aurausb
