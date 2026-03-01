#pragma once

#include <Types.h>

namespace vulkan {

namespace draw_shader_flags {
constexpr u32 kTexture0 = 1U << 0;
constexpr u32 kTexture1 = 1U << 1;
constexpr u32 kShade = 1U << 2;
constexpr u32 kSpecialTexrectDraw = 1U << 3;
constexpr u32 kSpecialDepthFromTexture1 = 1U << 4;
constexpr u32 kSpecialGammaCorrection = 1U << 5;
constexpr u32 kSpecialFXAA = 1U << 6;
constexpr u32 kSpecialTextDraw = 1U << 7;
constexpr u32 kSpecialDepthFog = 1U << 8;
constexpr u32 kSpecialStrictBlendMux = 1U << 9;
constexpr u32 kCombinerAddConstant = 1U << 10;
constexpr u32 kShadeRGBOnly = 1U << 11;
} // namespace draw_shader_flags

struct DrawPushConstants {
	u32 flags = draw_shader_flags::kShade;
	u32 texrectAlphaTest = 0U;
	u32 texrectFilterMode = 0U;
	u32 reserved0 = 0U;
	f32 texrectTextureWidth = 1.0f;
	f32 texrectTextureHeight = 1.0f;
	f32 gammaLevel = 2.0f;
	f32 reserved1 = 0.0f;
	f32 textColorR = 1.0f;
	f32 textColorG = 1.0f;
	f32 textColorB = 1.0f;
	f32 textColorA = 1.0f;
	f32 fogColorR = 0.0f;
	f32 fogColorG = 0.0f;
	f32 fogColorB = 0.0f;
	f32 fogColorA = 1.0f;
};

} // namespace vulkan
