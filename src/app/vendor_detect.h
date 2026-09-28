// Suggested answers for the setup guide: the USB devices plugged in, the lighting software
// installed, and what the SMBIOS scan and the Aura probe found. Read-only.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

#include "setup_plan.h"

namespace luma::app {

struct SetupHardware;

// `hw`: the SMBIOS scan (board, memory); `auraFound`: the Aura probe found devices.
setup::Answers DetectVendors(const SetupHardware& hw, bool auraFound);

// Every USB device plugged in, as (vendor, product).
std::vector<std::pair<uint16_t, uint16_t>> UsbDevices();

// Whether a USB device with this vendor and one of these product IDs is plugged in.
bool UsbDevicePresent(uint16_t vid, std::initializer_list<uint16_t> pids);

}  // namespace luma::app
