#pragma once

#include <Types.h>

namespace vulkan {
namespace combiner {

u32 expandCombinerColorAForSolidCheck(u32 _encoded);
u32 expandCombinerColorBForSolidCheck(u32 _encoded);
u32 expandCombinerColorMForSolidCheck(u32 _encoded);
u32 expandCombinerColorDForSolidCheck(u32 _encoded);
u32 expandCombinerAlphaAForSolidCheck(u32 _encoded);
u32 expandCombinerAlphaBForSolidCheck(u32 _encoded);
u32 expandCombinerAlphaMForSolidCheck(u32 _encoded);
u32 expandCombinerAlphaDForSolidCheck(u32 _encoded);

} // namespace combiner
} // namespace vulkan
