// Suggested answers for the setup guide: the USB devices plugged in, the lighting software
// installed, and what the SMBIOS scan and the Aura probe found. Read-only.
#pragma once

#include "setup_plan.h"

namespace luma::app {

struct SetupHardware;

// `hw`: the SMBIOS scan (board, memory); `auraFound`: the Aura probe found devices.
setup::Answers DetectVendors(const SetupHardware& hw, bool auraFound);

}  // namespace luma::app
