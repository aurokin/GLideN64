#pragma once

#include "rvk2_RenderPlan.h"

namespace rvk2 {

struct TextureReplacementRequest {
	const RenderWorkPacket * work = nullptr;
	s32 s = 0;
	s32 t = 0;
	s32 w = 0;
	bool perspective = false;
};

struct TextureReplacementKey {
	u64 nativeTextureHash = 0ULL;
	u64 paletteHash = 0ULL;
	u64 deterministicKey = 0ULL;
	u16 nativeWidth = 0U;
	u16 nativeHeight = 0U;
	u8 format = 0U;
	u8 size = 0U;
	u8 tile = 0U;
	u8 palette = 0U;
	bool ciTexture = false;
};

struct TextureReplacementCacheKey {
	u64 hi = 0ULL;
	u64 lo = 0ULL;
};

TextureReplacementKey buildTextureReplacementKey(const TextureReplacementRequest & _request);
TextureReplacementCacheKey buildTextureReplacementCacheKey(const TextureReplacementKey & _key);

} // namespace rvk2
