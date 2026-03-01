#pragma once

#include "vulkan_DrawRecorder.h"

namespace vulkan {
namespace packet_normalize {

vulkan::DrawVertex normalizeFallbackVertex(const vulkan::DrawPacket & _packet, const vulkan::DrawVertex & _src);

void normalizePacketPositions(vulkan::DrawPacket & _packet);

} // namespace packet_normalize
} // namespace vulkan
