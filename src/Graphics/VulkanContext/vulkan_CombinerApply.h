#pragma once

#include <Graphics/CombinerProgram.h>

#include "vulkan_DrawRecorder.h"

namespace vulkan {
namespace combiner {

void applyCombinerSolidColorOverride(const graphics::CombinerProgram * _combiner, vulkan::DrawPacket & _packet);

} // namespace combiner
} // namespace vulkan
