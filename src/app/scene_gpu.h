#pragma once

#include <cstdint>
#include "scene3d.h"

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace luma::app::scene_gpu {
bool Init(ID3D11Device* device, ID3D11DeviceContext* context);
void Shutdown();
// One retained texture per setup view. Zero means the caller should use its fallback.
uintptr_t Image(int slot, const std::vector<s3d::DrawItem>& items, const s3d::Viewport& viewport, float scale);
}
