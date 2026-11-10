// Color-picker labels shared by regular lighting and the boot effect editor.
#pragma once
#include "effects.h"

namespace luma::app {
inline const char* SecondaryColorLabel(fx::Kind kind) {
    if (kind == fx::Kind::Comet) return "Background";
    if (kind == fx::Kind::Twinkle) return "Sparkles";
    return fx::IsSetupEffect(kind) ? "Unlit color" : "Second color";
}
}  // namespace luma::app
