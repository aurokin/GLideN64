#include "rvk2_TextureReplacement.h"

#include <algorithm>

namespace {

constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;

inline void hashByte(u64 & _hash, u8 _value)
{
	_hash ^= static_cast<u64>(_value);
	_hash *= kFnvPrime;
}

inline void hashU16(u64 & _hash, u16 _value)
{
	hashByte(_hash, static_cast<u8>(_value & 0xFFU));
	hashByte(_hash, static_cast<u8>((_value >> 8U) & 0xFFU));
}

inline void hashU32(u64 & _hash, u32 _value)
{
	hashByte(_hash, static_cast<u8>(_value & 0xFFU));
	hashByte(_hash, static_cast<u8>((_value >> 8U) & 0xFFU));
	hashByte(_hash, static_cast<u8>((_value >> 16U) & 0xFFU));
	hashByte(_hash, static_cast<u8>((_value >> 24U) & 0xFFU));
}

inline void hashU64(u64 & _hash, u64 _value)
{
	hashU32(_hash, static_cast<u32>(_value & 0xFFFFFFFFULL));
	hashU32(_hash, static_cast<u32>((_value >> 32U) & 0xFFFFFFFFULL));
}

inline void hashS32(u64 & _hash, s32 _value)
{
	hashU32(_hash, static_cast<u32>(_value));
}

inline u16 decodeTileSpanTexels(u16 _a, u16 _b)
{
	const u32 low = std::min<u32>(_a, _b);
	const u32 high = std::max<u32>(_a, _b);
	if (high <= low)
		return 1U;
	const u32 spanSubTexel = high - low;
	const u32 spanTexel = (spanSubTexel >> 2U) + 1U;
	return static_cast<u16>(std::max<u32>(1U, std::min<u32>(spanTexel, 0xFFFFU)));
}

inline bool isCITexture(const rvk2::RenderWorkPacket & _work)
{
	return _work.textureImageFormat == 2U || _work.tileFormat == 2U;
}

u64 buildNativeTextureHash(const rvk2::RenderWorkPacket & _work)
{
	u64 hash = kFnvOffset;
	hashByte(hash, _work.textureImageFormat);
	hashByte(hash, _work.textureImageSize);
	hashU16(hash, _work.textureImageWidth);
	hashU32(hash, _work.textureImageAddress);
	hashByte(hash, _work.tileFormat);
	hashByte(hash, _work.tileSize);
	hashU16(hash, _work.tileLine);
	hashU16(hash, _work.tileTmem);
	hashByte(hash, _work.tilePalette);
	hashByte(hash, _work.tileCms);
	hashByte(hash, _work.tileCmt);
	hashByte(hash, _work.tileMasks);
	hashByte(hash, _work.tileMaskt);
	hashByte(hash, _work.tileShifts);
	hashByte(hash, _work.tileShiftt);
	hashU16(hash, _work.tileULS);
	hashU16(hash, _work.tileULT);
	hashU16(hash, _work.tileLRS);
	hashU16(hash, _work.tileLRT);
	hashByte(hash, _work.tmemLoadKind);
	hashByte(hash, _work.tmemLoadTile);
	hashU16(hash, _work.tmemLoadULS);
	hashU16(hash, _work.tmemLoadULT);
	hashU16(hash, _work.tmemLoadLRS);
	hashU16(hash, _work.tmemLoadLRT);
	hashU16(hash, _work.tmemLoadDXT);
	return hash;
}

u64 buildPaletteHash(const rvk2::RenderWorkPacket & _work, bool _ciTexture)
{
	u64 hash = kFnvOffset;
	hashByte(hash, _work.tilePalette);
	hashU64(hash, _work.keyState);
	hashU64(hash, _work.convertState);
	if (_ciTexture) {
		hashByte(hash, _work.textureImageFormat);
		hashByte(hash, _work.textureImageSize);
		hashU32(hash, _work.textureImageAddress);
	}
	return hash;
}

} // namespace

namespace rvk2 {

TextureReplacementKey buildTextureReplacementKey(const TextureReplacementRequest & _request)
{
	TextureReplacementKey key{};
	if (_request.work == nullptr)
		return key;

	const RenderWorkPacket & work = *_request.work;
	key.ciTexture = isCITexture(work);
	key.format = work.textureImageFormat;
	key.size = work.textureImageSize;
	key.tile = work.tile;
	key.palette = work.tilePalette;
	key.nativeWidth = decodeTileSpanTexels(work.tileULS, work.tileLRS);
	key.nativeHeight = decodeTileSpanTexels(work.tileULT, work.tileLRT);
	key.nativeTextureHash = buildNativeTextureHash(work);
	key.paletteHash = buildPaletteHash(work, key.ciTexture);

	u64 deterministic = kFnvOffset;
	hashU64(deterministic, key.nativeTextureHash);
	hashU64(deterministic, key.paletteHash);
	hashU16(deterministic, key.nativeWidth);
	hashU16(deterministic, key.nativeHeight);
	hashByte(deterministic, key.format);
	hashByte(deterministic, key.size);
	hashByte(deterministic, key.tile);
	hashByte(deterministic, key.palette);
	hashByte(deterministic, key.ciTexture ? 1U : 0U);
	hashS32(deterministic, _request.s);
	hashS32(deterministic, _request.t);
	hashS32(deterministic, _request.w);
	hashByte(deterministic, _request.perspective ? 1U : 0U);
	key.deterministicKey = deterministic;
	return key;
}

TextureReplacementCacheKey buildTextureReplacementCacheKey(const TextureReplacementKey & _key)
{
	TextureReplacementCacheKey cacheKey{};
	u64 hi = kFnvOffset;
	u64 lo = kFnvOffset;
	hashU64(hi, _key.nativeTextureHash);
	hashU64(hi, _key.paletteHash);
	hashU16(hi, _key.nativeWidth);
	hashU16(hi, _key.nativeHeight);
	hashU64(lo, _key.deterministicKey);
	hashByte(lo, _key.format);
	hashByte(lo, _key.size);
	hashByte(lo, _key.tile);
	hashByte(lo, _key.palette);
	hashByte(lo, _key.ciTexture ? 1U : 0U);
	cacheKey.hi = hi;
	cacheKey.lo = lo;
	return cacheKey;
}

} // namespace rvk2
