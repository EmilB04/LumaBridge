// The monitors Windows has, and how they're arranged (see display_layout.h).
#pragma once

#include <vector>

#include "display_layout.h"

namespace luma::app::displays {

// Every active display: its place on Windows' desktop, resolution, refresh rate, physical size
// (from the monitor's EDID, as Windows reports it) and name. Cheap; call when needed.
std::vector<Display> List();

}  // namespace luma::app::displays
