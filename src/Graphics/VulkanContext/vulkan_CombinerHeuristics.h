#pragma once

#include <Types.h>
#include <CombinerKey.h>
#include <Graphics/CombinerProgram.h>

namespace vulkan {
namespace combiner {

graphics::CombinerProgram * createInferredCombinerProgram(const CombinerKey & _key);

u32 resolveShaderFlags(const graphics::CombinerProgram * _combiner, bool _defaultShade, bool _defaultTexture0);

} // namespace combiner
} // namespace vulkan
