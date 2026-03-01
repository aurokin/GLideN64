#pragma once

#include "vulkan_DrawRecorder.h"

namespace vulkan {
namespace blendmux {

void applyStrictBlendMuxPacketState(bool _texrect, vulkan::DrawPacket & _packet);

} // namespace blendmux
} // namespace vulkan
