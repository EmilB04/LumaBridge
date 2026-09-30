// Monitor support stays behind the back cover, so it cannot poke through the screen.
#pragma once

#include "display_layout.h"
#include "scene3d.h"

namespace luma::app::monitor3d {
inline void Support(s3d::Scene& sc, const displays::Placed& display, int id) {
    // Vertical monitors and small panels are mounted without a stand.
    if (display.portrait || display.resting || displays::Small(display)) return;
    const uint32_t color = s3d::Rgba(43, 48, 57);
    const float center = display.bottom + display.h * 0.45f;
    if (display.bottom < 20) {
        sc.Box({-9, 0, -8}, {9, 1, 5}, s3d::Rgba(35, 39, 47), id);
        sc.Box({-1.5f, 1, -6.4f}, {1.5f, center, -4.6f}, color, id);
        sc.Box({-2, center - 1.4f, -4.6f}, {2, center + 1.4f, -4.4f}, color, id);
    } else {
        sc.Box({-1.2f, center - 1.2f, -14}, {1.2f, center + 1.2f, -4.4f}, color, id);
    }
}
}  // namespace luma::app::monitor3d
