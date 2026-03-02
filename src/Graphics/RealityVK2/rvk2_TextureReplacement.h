#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

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

struct TextureReplacementImage {
	u16 width = 0U;
	u16 height = 0U;
	std::vector<u32> pixels;
};

class TextureReplacementStore
{
public:
	void clear();
	bool empty() const;
	size_t entryCount() const;
	bool insert(const TextureReplacementCacheKey & _cacheKey, const TextureReplacementImage & _image);
	const TextureReplacementImage * find(const TextureReplacementCacheKey & _cacheKey) const;
	std::vector<TextureReplacementCacheKey> keys() const;

private:
	struct KeyHash {
		size_t operator()(const TextureReplacementCacheKey & _key) const;
	};
	struct KeyEq {
		bool operator()(const TextureReplacementCacheKey & _a, const TextureReplacementCacheKey & _b) const;
	};

	std::unordered_map<TextureReplacementCacheKey, TextureReplacementImage, KeyHash, KeyEq> m_entries;
};

TextureReplacementKey buildTextureReplacementKey(const TextureReplacementRequest & _request);
TextureReplacementCacheKey buildTextureReplacementCacheKey(const TextureReplacementKey & _key);
u32 sampleTextureReplacementImage(const TextureReplacementImage & _image, s32 _s, s32 _t);
bool writeTextureReplacementHTC(const char * _path, const TextureReplacementStore & _store);
bool loadTextureReplacementHTC(const char * _path, TextureReplacementStore & _store);
bool loadTextureReplacementPack(const char * _packPath, TextureReplacementStore & _store);

} // namespace rvk2
