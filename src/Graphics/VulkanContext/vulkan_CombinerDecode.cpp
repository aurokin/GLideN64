#include "vulkan_CombinerDecode.h"

#include <array>
#include <Combiner.h>
#include <gDP.h>

namespace vulkan {
namespace combiner {

u32 expandCombinerColorAForSolidCheck(u32 _encoded)
{
	static const std::array<u32, 16> kExpanded = {
		G_GCI_COMBINED, G_GCI_TEXEL0, G_GCI_TEXEL1, G_GCI_PRIMITIVE,
		G_GCI_SHADE, G_GCI_ENVIRONMENT, G_GCI_ONE, G_GCI_NOISE,
		G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO,
		G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO
	};
	return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
}

u32 expandCombinerColorBForSolidCheck(u32 _encoded)
{
	static const std::array<u32, 16> kExpanded = {
		G_GCI_COMBINED, G_GCI_TEXEL0, G_GCI_TEXEL1, G_GCI_PRIMITIVE,
		G_GCI_SHADE, G_GCI_ENVIRONMENT, G_GCI_CENTER, G_GCI_K4,
		G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO,
		G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO
	};
	return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
}

u32 expandCombinerColorMForSolidCheck(u32 _encoded)
{
	static const std::array<u32, 32> kExpanded = {
		G_GCI_COMBINED, G_GCI_TEXEL0, G_GCI_TEXEL1, G_GCI_PRIMITIVE,
		G_GCI_SHADE, G_GCI_ENVIRONMENT, G_GCI_SCALE, G_GCI_COMBINED_ALPHA,
		G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA, G_GCI_SHADE_ALPHA,
		G_GCI_ENV_ALPHA, G_GCI_LOD_FRACTION, G_GCI_PRIM_LOD_FRAC, G_GCI_K5,
		G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO,
		G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO,
		G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO,
		G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO
	};
	return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
}

u32 expandCombinerColorDForSolidCheck(u32 _encoded)
{
	static const std::array<u32, 8> kExpanded = {
		G_GCI_COMBINED, G_GCI_TEXEL0, G_GCI_TEXEL1, G_GCI_PRIMITIVE,
		G_GCI_SHADE, G_GCI_ENVIRONMENT, G_GCI_ONE, G_GCI_ZERO
	};
	return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
}

u32 expandCombinerAlphaMForSolidCheck(u32 _encoded)
{
	static const std::array<u32, 8> kExpanded = {
		G_GCI_LOD_FRACTION, G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA,
		G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA, G_GCI_PRIM_LOD_FRAC, G_GCI_ZERO
	};
	return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
}

u32 expandCombinerAlphaAForSolidCheck(u32 _encoded)
{
	static const std::array<u32, 8> kExpanded = {
		G_GCI_COMBINED, G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA,
		G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA, G_GCI_ONE, G_GCI_ZERO
	};
	return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
}

u32 expandCombinerAlphaBForSolidCheck(u32 _encoded)
{
	static const std::array<u32, 8> kExpanded = {
		G_GCI_COMBINED, G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA,
		G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA, G_GCI_ONE, G_GCI_ZERO
	};
	return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
}

u32 expandCombinerAlphaDForSolidCheck(u32 _encoded)
{
	static const std::array<u32, 8> kExpanded = {
		G_GCI_COMBINED, G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA,
		G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA, G_GCI_ONE, G_GCI_ZERO
	};
	return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
}

} // namespace combiner
} // namespace vulkan
