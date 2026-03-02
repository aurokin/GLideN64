#include "rvk2_Executor.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "rvk2_VIRenderer.h"

namespace {

thread_local const rvk2::TextureReplacementStore * gActiveTextureReplacementStore = nullptr;
thread_local rvk2::ExecutorSummary * gActiveExecutorSummary = nullptr;

inline bool envStringIsTrue(const char * _value)
{
	if (_value == nullptr || _value[0] == '\0')
		return false;
	const char c0 = static_cast<char>(_value[0] | 0x20);
	if (c0 == '1' || c0 == 'y' || c0 == 't')
		return true;
	if ((c0 == 'o') && ((_value[1] | 0x20) == 'n'))
		return true;
	return false;
}

inline bool parseEnvUnsigned(const char * _value, u64 & _out)
{
	if (_value == nullptr || _value[0] == '\0')
		return false;
	char * end = nullptr;
	errno = 0;
	const unsigned long long value = std::strtoull(_value, &end, 0);
	if (errno != 0 || end == nullptr || *end != '\0')
		return false;
	_out = static_cast<u64>(value);
	return true;
}

inline std::string trimAsciiWhitespace(const std::string & _value)
{
	size_t begin = 0U;
	while (begin < _value.size()) {
		const char c = _value[begin];
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
			break;
		++begin;
	}
	size_t end = _value.size();
	while (end > begin) {
		const char c = _value[end - 1U];
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
			break;
		--end;
	}
	return _value.substr(begin, end - begin);
}

inline std::string toLowerAscii(std::string _value)
{
	for (char & c : _value) {
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
	return _value;
}

inline bool parseBooleanToken(const std::string & _token, bool & _out)
{
	const std::string token = toLowerAscii(trimAsciiWhitespace(_token));
	if (token.empty())
		return false;
	if (token == "1" || token == "true" || token == "yes" || token == "on") {
		_out = true;
		return true;
	}
	if (token == "0" || token == "false" || token == "no" || token == "off") {
		_out = false;
		return true;
	}
	return false;
}

inline bool parseUnsignedToken(const std::string & _token, u64 & _out)
{
	const std::string token = trimAsciiWhitespace(_token);
	if (token.empty())
		return false;
	char * end = nullptr;
	errno = 0;
	const unsigned long long value = std::strtoull(token.c_str(), &end, 0);
	if (errno != 0 || end == nullptr || *end != '\0')
		return false;
	_out = static_cast<u64>(value);
	return true;
}

void applyTextureReplacementControlFile(
	const char * _path,
	rvk2::ExecutorConfig & _config)
{
	if (_path == nullptr || _path[0] == '\0')
		return;

	std::FILE * file = std::fopen(_path, "rb");
	if (file == nullptr)
		return;

	bool hasEnableOverride = false;
	bool enableOverride = false;
	char line[4096];
	while (std::fgets(line, static_cast<int>(sizeof(line)), file) != nullptr) {
		std::string raw(line);
		raw = trimAsciiWhitespace(raw);
		if (raw.empty() || raw[0] == '#')
			continue;

		size_t separatorPos = raw.find('=');
		if (separatorPos == std::string::npos)
			separatorPos = raw.find('\t');
		if (separatorPos == std::string::npos)
			continue;

		const std::string key = toLowerAscii(trimAsciiWhitespace(raw.substr(0U, separatorPos)));
		const std::string value = trimAsciiWhitespace(raw.substr(separatorPos + 1U));
		u64 parsed = 0ULL;
		bool parsedBool = false;

		if (key == "enable" || key == "texture_replacement_enable") {
			if (parseBooleanToken(value, parsedBool)) {
				hasEnableOverride = true;
				enableOverride = parsedBool;
				_config.textureReplacementEnable = parsedBool;
			}
			continue;
		}
		if (key == "cache_path" || key == "htc_path") {
			_config.textureReplacementCachePath = value;
			continue;
		}
		if (key == "pack_path") {
			_config.textureReplacementPackPath = value;
			continue;
		}
		if (key == "summary_path") {
			_config.textureReplacementSummaryPath = value;
			continue;
		}
		if (key == "log_summary") {
			if (parseBooleanToken(value, parsedBool))
				_config.textureReplacementLogSummary = parsedBool;
			continue;
		}
		if (key == "max_entries") {
			if (parseUnsignedToken(value, parsed))
				_config.textureReplacementMaxEntries = static_cast<u32>(std::min<u64>(parsed, 0xFFFFFFFFULL));
			continue;
		}
		if (key == "max_pixels") {
			if (parseUnsignedToken(value, parsed))
				_config.textureReplacementMaxPixels = parsed;
			continue;
		}
		if (key == "reload_token") {
			if (parseUnsignedToken(value, parsed))
				_config.textureReplacementReloadToken = parsed;
			continue;
		}
		if (key == "invalidate_token") {
			if (parseUnsignedToken(value, parsed))
				_config.textureReplacementInvalidateToken = parsed;
			continue;
		}
	}

	std::fclose(file);

	if (!hasEnableOverride
		&& (!_config.textureReplacementCachePath.empty()
			|| !_config.textureReplacementPackPath.empty())) {
		_config.textureReplacementEnable = true;
	}
	if (hasEnableOverride && !enableOverride)
		_config.textureReplacementEnable = false;
}

inline u32 clampU32(u32 _value, u32 _minimum, u32 _maximum)
{
	if (_value < _minimum)
		return _minimum;
	if (_value > _maximum)
		return _maximum;
	return _value;
}

inline u32 decodeFillColor(u32 _fillColor, u8 _colorImageSize)
{
	if (_colorImageSize == 3U)
		return _fillColor;

	const u16 color16 = static_cast<u16>(_fillColor & 0xFFFFU);
	const u8 r5 = static_cast<u8>((color16 >> 11) & 0x1FU);
	const u8 g5 = static_cast<u8>((color16 >> 6) & 0x1FU);
	const u8 b5 = static_cast<u8>((color16 >> 1) & 0x1FU);
	const u8 a1 = static_cast<u8>(color16 & 0x1U);
	const u8 r = static_cast<u8>((static_cast<u32>(r5) * 255U + 15U) / 31U);
	const u8 g = static_cast<u8>((static_cast<u32>(g5) * 255U + 15U) / 31U);
	const u8 b = static_cast<u8>((static_cast<u32>(b5) * 255U + 15U) / 31U);
	const u8 a = a1 != 0U ? 255U : 0U;
	return (static_cast<u32>(r) << 24)
		| (static_cast<u32>(g) << 16)
		| (static_cast<u32>(b) << 8)
		| static_cast<u32>(a);
}

inline u32 encodeSurfaceColor(u32 _rgba, u8 _colorImageSize)
{
	if (_colorImageSize != 2U)
		return _rgba;

	const u8 r = static_cast<u8>((_rgba >> 24) & 0xFFU);
	const u8 g = static_cast<u8>((_rgba >> 16) & 0xFFU);
	const u8 b = static_cast<u8>((_rgba >> 8) & 0xFFU);
	const u8 a = static_cast<u8>(_rgba & 0xFFU);
	const u16 r5 = static_cast<u16>((static_cast<u32>(r) * 31U + 127U) / 255U);
	const u16 g5 = static_cast<u16>((static_cast<u32>(g) * 31U + 127U) / 255U);
	const u16 b5 = static_cast<u16>((static_cast<u32>(b) * 31U + 127U) / 255U);
	const u16 a1 = a >= 128U ? 1U : 0U;
	const u16 packed = static_cast<u16>((r5 << 11U) | (g5 << 6U) | (b5 << 1U) | a1);
	return decodeFillColor(static_cast<u32>(packed), 2U);
}

inline u32 mode0Word(const rvk2::RenderWorkPacket & _work)
{
	return static_cast<u32>(_work.otherModes >> 32U);
}

inline u32 mode1Word(const rvk2::RenderWorkPacket & _work)
{
	return static_cast<u32>(_work.otherModes & 0xFFFFFFFFULL);
}

inline bool isImageReadEnabled(const rvk2::RenderWorkPacket & _work)
{
	return (mode1Word(_work) & (1U << 6U)) != 0U;
}

inline bool isAAEnabled(const rvk2::RenderWorkPacket & _work)
{
	return (mode1Word(_work) & (1U << 3U)) != 0U;
}

inline bool isPipelineModeEnabled(const rvk2::RenderWorkPacket & _work)
{
	return (mode0Word(_work) & (1U << 23U)) != 0U;
}

inline u8 decodeDepthMode(const rvk2::RenderWorkPacket & _work)
{
	return static_cast<u8>((mode1Word(_work) >> 10U) & 0x3U);
}

inline u8 decodeTextureFilterMode(const rvk2::RenderWorkPacket & _work)
{
	const u32 mode0 = mode0Word(_work);
	const u8 filterModePrimary = static_cast<u8>((mode0 >> 12U) & 0x3U);
	if (filterModePrimary != 0U)
		return filterModePrimary;
	return static_cast<u8>((mode0 >> 10U) & 0x3U);
}

inline u8 decodeTextureLUTMode(const rvk2::RenderWorkPacket & _work)
{
	return static_cast<u8>((mode0Word(_work) >> 14U) & 0x3U);
}

inline u8 decodeTextureDetailMode(const rvk2::RenderWorkPacket & _work)
{
	return static_cast<u8>((mode0Word(_work) >> 17U) & 0x3U);
}

inline bool isTextureLodEnabled(const rvk2::RenderWorkPacket & _work)
{
	return (mode0Word(_work) & (1U << 16U)) != 0U;
}

inline bool isTexturePerspEnabled(const rvk2::RenderWorkPacket & _work)
{
	return (mode0Word(_work) & (1U << 19U)) != 0U;
}

inline bool isCombineKeyEnabled(const rvk2::RenderWorkPacket & _work)
{
	return (mode0Word(_work) & (1U << 8U)) != 0U;
}

inline bool isConvertOneEnabled(const rvk2::RenderWorkPacket & _work)
{
	return (mode0Word(_work) & (1U << 9U)) != 0U;
}

inline u8 decodeAlphaDitherMode(const rvk2::RenderWorkPacket & _work)
{
	return static_cast<u8>((mode0Word(_work) >> 4U) & 0x3U);
}

inline u8 decodeColorDitherMode(const rvk2::RenderWorkPacket & _work)
{
	return static_cast<u8>((mode0Word(_work) >> 6U) & 0x3U);
}

inline bool isTextureEdgeEnabled(const rvk2::RenderWorkPacket & _work)
{
	return (mode1Word(_work) & (1U << 15U)) != 0U;
}

inline bool hasExplicitScissor(const rvk2::RenderWorkPacket & _work)
{
	return !(
		_work.scissorXH == 0U
		&& _work.scissorYH == 0U
		&& _work.scissorXL == 0U
		&& _work.scissorYL == 0U);
}

struct BlendMuxSelectors
{
	u8 m1a = 0U;
	u8 m1b = 0U;
	u8 m2a = 0U;
	u8 m2b = 0U;
};

inline BlendMuxSelectors decodeBlendMuxSelectors(
	const rvk2::RenderWorkPacket & _work,
	bool _cycle2Selectors)
{
	const u32 mode1 = mode1Word(_work);
	BlendMuxSelectors selectors{};
	if (_cycle2Selectors) {
		selectors.m1a = static_cast<u8>((mode1 >> 28U) & 0x3U);
		selectors.m1b = static_cast<u8>((mode1 >> 24U) & 0x3U);
		selectors.m2a = static_cast<u8>((mode1 >> 20U) & 0x3U);
		selectors.m2b = static_cast<u8>((mode1 >> 16U) & 0x3U);
	}
	else {
		selectors.m1a = static_cast<u8>((mode1 >> 30U) & 0x3U);
		selectors.m1b = static_cast<u8>((mode1 >> 26U) & 0x3U);
		selectors.m2a = static_cast<u8>((mode1 >> 22U) & 0x3U);
		selectors.m2b = static_cast<u8>((mode1 >> 18U) & 0x3U);
	}
	return selectors;
}

inline s64 absS64(s64 _value)
{
	return _value < 0 ? -_value : _value;
}

inline s32 clampS32FromS64(s64 _value)
{
	if (_value < static_cast<s64>(std::numeric_limits<s32>::min()))
		return std::numeric_limits<s32>::min();
	if (_value > static_cast<s64>(std::numeric_limits<s32>::max()))
		return std::numeric_limits<s32>::max();
	return static_cast<s32>(_value);
}

inline s32 wrapCoordPositive(s32 _value, s32 _period)
{
	if (_period <= 0)
		return 0;
	s32 wrapped = _value % _period;
	if (wrapped < 0)
		wrapped += _period;
	return wrapped;
}

inline s32 applyTileAxisTransform(
	s32 _coord5,
	u8 _shift,
	u8 _mask,
	u8 _cm,
	u16 _lo,
	u16 _hi)
{
	s32 texel = _coord5 >> 5U;
	if (_shift != 0U) {
		const u8 effectiveShift = static_cast<u8>(std::min<u32>(_shift, 15U));
		if (effectiveShift <= 10U)
			texel >>= effectiveShift;
		else
			texel <<= static_cast<u8>(16U - effectiveShift);
	}

	const bool mirror = (_cm & 0x1U) != 0U;
	const bool clamp = (_cm & 0x2U) != 0U;
	if (_mask != 0U) {
		const s32 period = 1 << std::min<u32>(_mask, 15U);
		if (clamp)
			texel = std::max<s32>(0, std::min<s32>(period - 1, texel));
		else if (mirror) {
			const s32 mirrorPeriod = period << 1U;
			s32 wrapped = wrapCoordPositive(texel, mirrorPeriod);
			if (wrapped >= period)
				wrapped = (mirrorPeriod - 1) - wrapped;
			texel = wrapped;
		}
		else
			texel = wrapCoordPositive(texel, period);
	}

	const s32 loTexel = static_cast<s32>(_lo >> 2U);
	const s32 hiTexel = static_cast<s32>(_hi >> 2U);
	if (loTexel != 0 || hiTexel != 0 || clamp) {
		const s32 low = std::min(loTexel, hiTexel);
		const s32 high = std::max(loTexel, hiTexel);
		if (texel < low)
			texel = low;
		if (texel > high)
			texel = high;
	}
	return texel;
}

inline void mixTextureSeed(u64 & _seed, u64 _value)
{
	_seed ^= _value + 0x9E3779B97F4A7C15ULL + (_seed << 6U) + (_seed >> 2U);
}

inline u64 buildTextureSeedBase(const rvk2::RenderWorkPacket & _work)
{
	u64 seed = 1469598103934665603ULL;
	mixTextureSeed(seed, static_cast<u64>(_work.textureImageAddress));
	mixTextureSeed(seed, static_cast<u64>(_work.textureImageFormat));
	mixTextureSeed(seed, static_cast<u64>(_work.textureImageSize));
	mixTextureSeed(seed, static_cast<u64>(_work.textureImageWidth));
	mixTextureSeed(seed, static_cast<u64>(_work.tileFormat));
	mixTextureSeed(seed, static_cast<u64>(_work.tileSize));
	mixTextureSeed(seed, static_cast<u64>(_work.tileLine));
	mixTextureSeed(seed, static_cast<u64>(_work.tileTmem));
	mixTextureSeed(seed, static_cast<u64>(_work.tilePalette));
	mixTextureSeed(seed, static_cast<u64>(_work.tileCmt));
	mixTextureSeed(seed, static_cast<u64>(_work.tileCms));
	mixTextureSeed(seed, static_cast<u64>(_work.tileMaskt));
	mixTextureSeed(seed, static_cast<u64>(_work.tileMasks));
	mixTextureSeed(seed, static_cast<u64>(_work.tileShiftt));
	mixTextureSeed(seed, static_cast<u64>(_work.tileShifts));
	mixTextureSeed(seed, static_cast<u64>(_work.tileULS));
	mixTextureSeed(seed, static_cast<u64>(_work.tileULT));
	mixTextureSeed(seed, static_cast<u64>(_work.tileLRS));
	mixTextureSeed(seed, static_cast<u64>(_work.tileLRT));
	mixTextureSeed(seed, static_cast<u64>(_work.tmemLoadKind));
	mixTextureSeed(seed, static_cast<u64>(_work.tmemLoadTile));
	mixTextureSeed(seed, static_cast<u64>(_work.tmemLoadULS));
	mixTextureSeed(seed, static_cast<u64>(_work.tmemLoadULT));
	mixTextureSeed(seed, static_cast<u64>(_work.tmemLoadLRS));
	mixTextureSeed(seed, static_cast<u64>(_work.tmemLoadLRT));
	mixTextureSeed(seed, static_cast<u64>(_work.tmemLoadDXT));
	return seed;
}

inline u32 bilerpColorRGBA(
	u32 _c00,
	u32 _c10,
	u32 _c01,
	u32 _c11,
	u32 _fracS,
	u32 _fracT)
{
	const u32 fracS = std::min<u32>(_fracS, 31U);
	const u32 fracT = std::min<u32>(_fracT, 31U);
	const u32 invS = 32U - fracS;
	const u32 invT = 32U - fracT;
	const auto unpack = [](u32 _c, u32 _shift) -> u32 {
		return (_c >> _shift) & 0xFFU;
	};
	const auto blendAxis = [&](u32 _a, u32 _b) -> u32 {
		return (_a * invS + _b * fracS + 16U) >> 5U;
	};
	const auto blendChannel = [&](u32 _shift) -> u32 {
		const u32 a0 = blendAxis(unpack(_c00, _shift), unpack(_c10, _shift));
		const u32 a1 = blendAxis(unpack(_c01, _shift), unpack(_c11, _shift));
		return (a0 * invT + a1 * fracT + 16U) >> 5U;
	};
	const u32 r = blendChannel(24U);
	const u32 g = blendChannel(16U);
	const u32 b = blendChannel(8U);
	const u32 a = blendChannel(0U);
	return (r << 24U) | (g << 16U) | (b << 8U) | a;
}

inline u32 averageColorRGBA(
	u32 _c00,
	u32 _c10,
	u32 _c01,
	u32 _c11)
{
	const auto averageChannel = [&](u32 _shift) -> u32 {
		const u32 sum =
			((_c00 >> _shift) & 0xFFU)
			+ ((_c10 >> _shift) & 0xFFU)
			+ ((_c01 >> _shift) & 0xFFU)
			+ ((_c11 >> _shift) & 0xFFU);
		return (sum + 2U) >> 2U;
	};
	const u32 r = averageChannel(24U);
	const u32 g = averageChannel(16U);
	const u32 b = averageChannel(8U);
	const u32 a = averageChannel(0U);
	return (r << 24U) | (g << 16U) | (b << 8U) | a;
}

inline u32 applyTextureFilterMode(
	u8 _filterMode,
	u32 _c00,
	u32 _c10,
	u32 _c01,
	u32 _c11,
	u32 _fracS,
	u32 _fracT)
{
	if ((_filterMode & 0x3U) == 1U)
		return bilerpColorRGBA(_c00, _c10, _c01, _c11, _fracS, _fracT);
	if ((_filterMode & 0x3U) == 2U)
		return averageColorRGBA(_c00, _c10, _c01, _c11);
	if ((_filterMode & 0x3U) == 3U) {
		const u32 bilerp = bilerpColorRGBA(_c00, _c10, _c01, _c11, _fracS, _fracT);
		const auto sharpenChannel = [&](u32 _shift) -> u32 {
			const u32 base = (bilerp >> _shift) & 0xFFU;
			const u32 point = (_c00 >> _shift) & 0xFFU;
			return (base * 3U + point + 2U) >> 2U;
		};
		const u32 r = sharpenChannel(24U);
		const u32 g = sharpenChannel(16U);
		const u32 b = sharpenChannel(8U);
		const u32 a = sharpenChannel(0U);
		return (r << 24U) | (g << 16U) | (b << 8U) | a;
	}
	return _c00;
}

inline void applyTextureCoordinateModes(
	const rvk2::RenderWorkPacket & _work,
	s32 & _s,
	s32 & _t,
	s32 & _w,
	bool _includeW)
{
	s64 s = static_cast<s64>(_s);
	s64 t = static_cast<s64>(_t);
	const s64 wAbs = absS64(static_cast<s64>(_w));
	if (_includeW && isTexturePerspEnabled(_work) && wAbs > 0) {
		const s64 denom = (wAbs >> 8U) + 1;
		s = (s * 256 + (denom / 2)) / denom;
		t = (t * 256 + (denom / 2)) / denom;
	}

	if (isTextureLodEnabled(_work)) {
		const s64 lod = _includeW
			? std::min<s64>(255, wAbs >> 12U)
			: std::min<s64>(255, (absS64(s) + absS64(t)) >> 8U);
		const u8 detailMode = decodeTextureDetailMode(_work);
		const s64 signedLod = detailMode == 1U ? -lod : lod;
		s += signedLod;
		t += detailMode == 2U ? (signedLod >> 1U) : signedLod;
	}

	_s = clampS32FromS64(s);
	_t = clampS32FromS64(t);
}

inline u32 applyTextureLUTModeColor(
	const rvk2::RenderWorkPacket & _work,
	u64 _seed,
	u32 _rgba)
{
	const u8 lutMode = decodeTextureLUTMode(_work);
	if (lutMode == 0U)
		return _rgba;

	u8 r = static_cast<u8>((_rgba >> 24U) & 0xFFU);
	u8 g = static_cast<u8>((_rgba >> 16U) & 0xFFU);
	u8 b = static_cast<u8>((_rgba >> 8U) & 0xFFU);
	u8 a = static_cast<u8>(_rgba & 0xFFU);
	u8 index = static_cast<u8>(r ^ g ^ b ^ static_cast<u8>(_work.tilePalette << 4U));
	if (lutMode >= 2U)
		index = static_cast<u8>((static_cast<u32>(r) + static_cast<u32>(g) + static_cast<u32>(b)) / 3U);

	u64 paletteSeed = _seed;
	mixTextureSeed(paletteSeed, static_cast<u64>(index));
	mixTextureSeed(paletteSeed, static_cast<u64>(_work.textureImageAddress));
	mixTextureSeed(paletteSeed, static_cast<u64>(_work.tilePalette));
	paletteSeed *= 0xD6E8FEB86659FD93ULL;

	if (lutMode == 1U) {
		r = static_cast<u8>((paletteSeed >> 8U) & 0xFFU);
		g = static_cast<u8>((paletteSeed >> 24U) & 0xFFU);
		b = static_cast<u8>((paletteSeed >> 40U) & 0xFFU);
		a = 255U;
	}
	else if (lutMode == 2U) {
		const u8 intensity = static_cast<u8>((paletteSeed >> 16U) & 0xFFU);
		const u8 alpha = static_cast<u8>((paletteSeed >> 32U) & 0xFFU);
		r = intensity;
		g = intensity;
		b = intensity;
		a = alpha;
	}
	else {
		const u8 intensity = static_cast<u8>((paletteSeed >> 16U) & 0xFFU);
		const u8 alpha4 = static_cast<u8>((paletteSeed >> 28U) & 0xFU);
		r = intensity;
		g = intensity;
		b = intensity;
		a = static_cast<u8>(alpha4 * 17U);
	}

	return (static_cast<u32>(r) << 24U)
		| (static_cast<u32>(g) << 16U)
		| (static_cast<u32>(b) << 8U)
		| static_cast<u32>(a);
}

inline u8 clampChannelS32(s32 _value)
{
	if (_value < 0)
		return 0U;
	if (_value > 255)
		return 255U;
	return static_cast<u8>(_value);
}

inline u32 applyTextureDetailModeColor(
	const rvk2::RenderWorkPacket & _work,
	u64 _seed,
	u32 _rgba)
{
	const u8 mode = decodeTextureDetailMode(_work);
	if (mode == 0U)
		return _rgba;

	s32 r = static_cast<s32>((_rgba >> 24U) & 0xFFU);
	s32 g = static_cast<s32>((_rgba >> 16U) & 0xFFU);
	s32 b = static_cast<s32>((_rgba >> 8U) & 0xFFU);
	const u8 a = static_cast<u8>(_rgba & 0xFFU);
	const s32 n0 = static_cast<s32>((_seed >> 8U) & 0xFFU);
	const s32 n1 = static_cast<s32>((_seed >> 24U) & 0xFFU);
	const s32 n2 = static_cast<s32>((_seed >> 40U) & 0xFFU);

	if (mode == 1U) {
		r = 128 + ((r - 128) * 3) / 2;
		g = 128 + ((g - 128) * 3) / 2;
		b = 128 + ((b - 128) * 3) / 2;
	}
	else if (mode == 2U) {
		r = (r * 3 + n0 + 2) / 4;
		g = (g * 3 + n1 + 2) / 4;
		b = (b * 3 + n2 + 2) / 4;
	}
	else {
		r = (r + n0 + 1) / 2;
		g = (g + n1 + 1) / 2;
		b = (b + n2 + 1) / 2;
	}

	return (static_cast<u32>(clampChannelS32(r)) << 24U)
		| (static_cast<u32>(clampChannelS32(g)) << 16U)
		| (static_cast<u32>(clampChannelS32(b)) << 8U)
		| static_cast<u32>(a);
}

const rvk2::TextureReplacementImage * findTextureReplacementImage(
	const rvk2::RenderWorkPacket & _work,
	s32 _s,
	s32 _t,
	s32 _w,
	bool _includeW)
{
	if (gActiveExecutorSummary == nullptr
		|| !gActiveExecutorSummary->textureReplacementEnabled) {
		return nullptr;
	}

	++gActiveExecutorSummary->textureReplacementSampleCount;
	if (gActiveTextureReplacementStore == nullptr || gActiveTextureReplacementStore->empty()) {
		++gActiveExecutorSummary->textureReplacementMissCount;
		return nullptr;
	}

	rvk2::TextureReplacementRequest request{};
	request.work = &_work;
	request.s = _s;
	request.t = _t;
	request.w = _w;
	request.perspective = _includeW && isTexturePerspEnabled(_work);
	const rvk2::TextureReplacementKey key = rvk2::buildTextureReplacementKey(request);
	const rvk2::TextureReplacementCacheKey cacheKey = rvk2::buildTextureReplacementCacheKey(key);
	const rvk2::TextureReplacementImage * image = gActiveTextureReplacementStore->find(cacheKey);
	if (image != nullptr)
		++gActiveExecutorSummary->textureReplacementHitCount;
	else
		++gActiveExecutorSummary->textureReplacementMissCount;
	return image;
}

inline u32 samplePseudoTexelColor(
	const rvk2::RenderWorkPacket & _work,
	s32 _s,
	s32 _t,
	s32 _w,
	bool _includeW,
	u32 _x,
	u32 _y)
{
	applyTextureCoordinateModes(_work, _s, _t, _w, _includeW);
	if (const rvk2::TextureReplacementImage * replacement =
			findTextureReplacementImage(_work, _s, _t, _w, _includeW)) {
		return rvk2::sampleTextureReplacementImage(*replacement, _s, _t);
	}
	u64 seed = buildTextureSeedBase(_work);
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_s)));
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_t)));
	if (_includeW)
		mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_w)));
	mixTextureSeed(seed, static_cast<u64>(_x));
	mixTextureSeed(seed, static_cast<u64>(_y));
	mixTextureSeed(seed, static_cast<u64>(_work.syncEpoch));
	seed *= 0x9E3779B97F4A7C15ULL;
	const u8 r = static_cast<u8>((seed >> 8) & 0xFFU);
	const u8 g = static_cast<u8>((seed >> 24) & 0xFFU);
	const u8 b = static_cast<u8>((seed >> 40) & 0xFFU);
	const u8 a = 255U;
	u32 rgba = (static_cast<u32>(r) << 24)
		| (static_cast<u32>(g) << 16)
		| (static_cast<u32>(b) << 8)
		| static_cast<u32>(a);
	rgba = applyTextureLUTModeColor(_work, seed, rgba);
	rgba = applyTextureDetailModeColor(_work, seed, rgba);
	return rgba;
}

inline u32 pseudoTexel(
	const rvk2::RenderWorkPacket & _work,
	u32 _x,
	u32 _y)
{
	const s32 dx = static_cast<s32>(_x) - static_cast<s32>(_work.rectULX);
	const s32 dy = static_cast<s32>(_y) - static_cast<s32>(_work.rectULY);
	const s32 sRaw = _work.texRectFlip
		? static_cast<s32>(_work.texS) + ((dy * static_cast<s32>(_work.texDSDX)) >> 5)
		: static_cast<s32>(_work.texS) + ((dx * static_cast<s32>(_work.texDSDX)) >> 5);
	const s32 tRaw = _work.texRectFlip
		? static_cast<s32>(_work.texT) + ((dx * static_cast<s32>(_work.texDTDY)) >> 5)
		: static_cast<s32>(_work.texT) + ((dy * static_cast<s32>(_work.texDTDY)) >> 5);
	const s32 s = applyTileAxisTransform(
		sRaw,
		_work.tileShifts,
		_work.tileMasks,
		_work.tileCms,
		_work.tileULS,
		_work.tileLRS);
	const s32 t = applyTileAxisTransform(
		tRaw,
		_work.tileShiftt,
		_work.tileMaskt,
		_work.tileCmt,
		_work.tileULT,
		_work.tileLRT);
	const u8 filterMode = decodeTextureFilterMode(_work);
	if (filterMode == 0U)
		return samplePseudoTexelColor(_work, s, t, 0, false, _x, _y);

	const s32 sNext = applyTileAxisTransform(
		sRaw + 32,
		_work.tileShifts,
		_work.tileMasks,
		_work.tileCms,
		_work.tileULS,
		_work.tileLRS);
	const s32 tNext = applyTileAxisTransform(
		tRaw + 32,
		_work.tileShiftt,
		_work.tileMaskt,
		_work.tileCmt,
		_work.tileULT,
		_work.tileLRT);
	const u32 fracS = static_cast<u32>(sRaw) & 0x1FU;
	const u32 fracT = static_cast<u32>(tRaw) & 0x1FU;
	const u32 c00 = samplePseudoTexelColor(_work, s, t, 0, false, _x, _y);
	const u32 c10 = samplePseudoTexelColor(_work, sNext, t, 0, false, _x, _y);
	const u32 c01 = samplePseudoTexelColor(_work, s, tNext, 0, false, _x, _y);
	const u32 c11 = samplePseudoTexelColor(_work, sNext, tNext, 0, false, _x, _y);
	return applyTextureFilterMode(filterMode, c00, c10, c01, c11, fracS, fracT);
}

struct ColorSurface {
	u8 format = 0U;
	u8 size = 0U;
	u16 width = 0U;
	u16 height = 0U;
	std::vector<u32> pixels;
};

struct DepthSurface {
	u16 width = 0U;
	u16 height = 0U;
	std::vector<s32> values;
};

inline u32 bitsPerPixelFromSurfaceSize(u8 _size)
{
	switch (_size & 0x3U) {
	case 0U:
		return 4U;
	case 1U:
		return 8U;
	case 2U:
		return 16U;
	default:
		return 32U;
	}
}

bool chooseSurfaceForVIOrigin(
	const std::unordered_map<u32, ColorSurface> & _surfaces,
	u32 _viOriginAddress,
	u32 & _outSurfaceAddress)
{
	const auto exact = _surfaces.find(_viOriginAddress);
	if (exact != _surfaces.end()) {
		_outSurfaceAddress = _viOriginAddress;
		return true;
	}

	u32 bestAddress = 0U;
	u32 bestDelta = std::numeric_limits<u32>::max();
	bool found = false;
	for (const auto & entry : _surfaces) {
		const u32 surfaceAddress = entry.first;
		const ColorSurface & surface = entry.second;
		const u64 pixelCount = static_cast<u64>(surface.width) * static_cast<u64>(surface.height);
		const u64 bitCount = pixelCount * static_cast<u64>(bitsPerPixelFromSurfaceSize(surface.size));
		const u64 byteCount = (bitCount + 7ULL) >> 3U;
		const u64 surfaceBegin = static_cast<u64>(surfaceAddress);
		const u64 surfaceEnd = surfaceBegin + byteCount;
		const u64 origin = static_cast<u64>(_viOriginAddress);
		if (origin < surfaceBegin || origin >= surfaceEnd)
			continue;
		const u32 delta = static_cast<u32>(origin - surfaceBegin);
		if (!found || delta < bestDelta) {
			found = true;
			bestDelta = delta;
			bestAddress = surfaceAddress;
		}
	}

	if (!found)
		return false;
	_outSurfaceAddress = bestAddress;
	return true;
}

inline size_t pixelIndex(u16 _width, u16 _x, u16 _y)
{
	return static_cast<size_t>(_y) * static_cast<size_t>(_width) + static_cast<size_t>(_x);
}

struct WriteBounds
{
	u32 x0 = 0U;
	u32 y0 = 0U;
	u32 x1 = 0U;
	u32 y1 = 0U;
};

void ensureSurfaceSize(
	ColorSurface & _surface,
	u16 _requiredWidth,
	u16 _requiredHeight,
	u16 _maxWidth,
	u16 _maxHeight)
{
	const u16 newWidth = static_cast<u16>(clampU32(_requiredWidth, 1U, _maxWidth));
	const u16 newHeight = static_cast<u16>(clampU32(_requiredHeight, 1U, _maxHeight));
	if (newWidth <= _surface.width && newHeight <= _surface.height)
		return;

	const u16 targetWidth = std::max(_surface.width, newWidth);
	const u16 targetHeight = std::max(_surface.height, newHeight);
	std::vector<u32> resized(static_cast<size_t>(targetWidth) * static_cast<size_t>(targetHeight), 0U);
	for (u16 y = 0U; y < _surface.height; ++y) {
		for (u16 x = 0U; x < _surface.width; ++x)
			resized[pixelIndex(targetWidth, x, y)] = _surface.pixels[pixelIndex(_surface.width, x, y)];
	}
	_surface.width = targetWidth;
	_surface.height = targetHeight;
	_surface.pixels.swap(resized);
}

void ensureDepthSurfaceSize(
	DepthSurface & _surface,
	u16 _requiredWidth,
	u16 _requiredHeight,
	u16 _maxWidth,
	u16 _maxHeight)
{
	const u16 newWidth = static_cast<u16>(clampU32(_requiredWidth, 1U, _maxWidth));
	const u16 newHeight = static_cast<u16>(clampU32(_requiredHeight, 1U, _maxHeight));
	if (newWidth <= _surface.width && newHeight <= _surface.height)
		return;

	const u16 targetWidth = std::max(_surface.width, newWidth);
	const u16 targetHeight = std::max(_surface.height, newHeight);
	std::vector<s32> resized(
		static_cast<size_t>(targetWidth) * static_cast<size_t>(targetHeight),
		std::numeric_limits<s32>::max());
	for (u16 y = 0U; y < _surface.height; ++y) {
		for (u16 x = 0U; x < _surface.width; ++x)
			resized[pixelIndex(targetWidth, x, y)] = _surface.values[pixelIndex(_surface.width, x, y)];
	}
	_surface.width = targetWidth;
	_surface.height = targetHeight;
	_surface.values.swap(resized);
}

bool computeWriteBounds(
	const ColorSurface & _surface,
	const rvk2::RenderWorkPacket & _work,
	WriteBounds & _bounds)
{
	const u32 ulx = std::min<u32>(_work.rectULX, _work.rectLRX);
	const u32 uly = std::min<u32>(_work.rectULY, _work.rectLRY);
	const u32 lrx = std::max<u32>(_work.rectULX, _work.rectLRX);
	const u32 lry = std::max<u32>(_work.rectULY, _work.rectLRY);
	if (lrx < ulx || lry < uly)
		return false;

	const u32 scissorX0 = std::min<u32>(_work.scissorXH, _work.scissorXL);
	const u32 scissorY0 = std::min<u32>(_work.scissorYH, _work.scissorYL);
	u32 scissorX1 = std::max<u32>(_work.scissorXH, _work.scissorXL);
	u32 scissorY1 = std::max<u32>(_work.scissorYH, _work.scissorYL);
	const bool defaultScissor = !hasExplicitScissor(_work);
	if (!defaultScissor) {
		// N64 scissor lower edge is exclusive in every phase.
		if (scissorY1 == 0U)
			return false;
		--scissorY1;

		// N64 scissor right edge is exclusive in cycle phases and inclusive in copy/fill.
		const bool inclusiveRightEdge =
			_work.phase == static_cast<u8>(rvk2::RenderPhase::kCopy)
			|| _work.phase == static_cast<u8>(rvk2::RenderPhase::kFill);
		if (!inclusiveRightEdge) {
			if (scissorX1 == 0U)
				return false;
			--scissorX1;
		}
	}
	const u32 clipX0 = defaultScissor ? ulx : scissorX0;
	const u32 clipY0 = defaultScissor ? uly : scissorY0;
	const u32 clipX1 = defaultScissor ? lrx : scissorX1;
	const u32 clipY1 = defaultScissor ? lry : scissorY1;

	const u32 writeX0 = std::max<u32>(ulx, clipX0);
	const u32 writeY0 = std::max<u32>(uly, clipY0);
	const u32 writeX1 = std::min<u32>(lrx, std::min<u32>(clipX1, _surface.width > 0U ? static_cast<u32>(_surface.width - 1U) : 0U));
	const u32 writeY1 = std::min<u32>(lry, std::min<u32>(clipY1, _surface.height > 0U ? static_cast<u32>(_surface.height - 1U) : 0U));
	if (writeX1 < writeX0 || writeY1 < writeY0)
		return false;

	_bounds.x0 = writeX0;
	_bounds.y0 = writeY0;
	_bounds.x1 = writeX1;
	_bounds.y1 = writeY1;
	return true;
}

inline bool passesScissorFieldFilter(const rvk2::RenderWorkPacket & _work, u32 _y)
{
	const bool interlacedFieldScissor = (_work.scissorMode & 0x2U) != 0U;
	if (!interlacedFieldScissor)
		return true;
	const u32 oddField = static_cast<u32>(_work.scissorMode & 0x1U);
	return (_y & 0x1U) == oddField;
}

inline double edgeFunction(
	double _ax,
	double _ay,
	double _bx,
	double _by,
	double _px,
	double _py)
{
	return (_px - _ax) * (_by - _ay) - (_py - _ay) * (_bx - _ax);
}

inline u32 pseudoTriangleColor(
	const rvk2::RenderWorkPacket & _work,
	u32 _x,
	u32 _y)
{
	u64 seed = static_cast<u64>(_work.combineMux);
	seed ^= static_cast<u64>(_work.blendParams) << 29U;
	seed ^= static_cast<u64>(_work.sourcePacketId) << 7U;
	seed ^= static_cast<u64>(_x) << 33U;
	seed ^= static_cast<u64>(_y) << 45U;
	seed ^= static_cast<u64>(_work.syncEpoch);
	seed *= 0x9E3779B97F4A7C15ULL;
	const u8 r = static_cast<u8>((seed >> 9) & 0xFFU);
	const u8 g = static_cast<u8>((seed >> 27) & 0xFFU);
	const u8 b = static_cast<u8>((seed >> 41) & 0xFFU);
	const u8 a = 255U;
	return (static_cast<u32>(r) << 24)
		| (static_cast<u32>(g) << 16)
		| (static_cast<u32>(b) << 8)
		| static_cast<u32>(a);
}

struct ColorRGBA
{
	u8 r = 0U;
	u8 g = 0U;
	u8 b = 0U;
	u8 a = 0U;
};

inline ColorRGBA unpackRGBA(u32 _rgba)
{
	ColorRGBA color{};
	color.r = static_cast<u8>((_rgba >> 24U) & 0xFFU);
	color.g = static_cast<u8>((_rgba >> 16U) & 0xFFU);
	color.b = static_cast<u8>((_rgba >> 8U) & 0xFFU);
	color.a = static_cast<u8>(_rgba & 0xFFU);
	return color;
}

inline u32 packRGBA(const ColorRGBA & _color)
{
	return (static_cast<u32>(_color.r) << 24U)
		| (static_cast<u32>(_color.g) << 16U)
		| (static_cast<u32>(_color.b) << 8U)
		| static_cast<u32>(_color.a);
}

inline u8 clampU8FromS32(s32 _value)
{
	if (_value < 0)
		return 0U;
	if (_value > 255)
		return 255U;
	return static_cast<u8>(_value);
}

inline u8 mixU8WithWeight(u8 _base, u8 _target, u8 _mix)
{
	const u32 invMix = static_cast<u32>(255U - _mix);
	const u32 value =
		static_cast<u32>(_base) * invMix
		+ static_cast<u32>(_target) * static_cast<u32>(_mix);
	return static_cast<u8>((value + 127U) / 255U);
}

inline u8 laneByteFromDigest(u64 _value, u32 _lane)
{
	return static_cast<u8>((_value >> ((_lane & 0x7U) * 8U)) & 0xFFULL);
}

struct CombinerCycleSelectors
{
	u8 colorA = 0U;
	u8 colorB = 0U;
	u8 colorC = 0U;
	u8 colorD = 0U;
	u8 alphaA = 0U;
	u8 alphaB = 0U;
	u8 alphaC = 0U;
	u8 alphaD = 0U;
};

inline CombinerCycleSelectors decodeCombinerCycleSelectors(
	u64 _combineMux,
	bool _cycle2Selectors)
{
	const u32 mode0 = static_cast<u32>(_combineMux >> 32U);
	const u32 mode1 = static_cast<u32>(_combineMux & 0xFFFFFFFFULL);
	CombinerCycleSelectors selectors{};
	if (_cycle2Selectors) {
		selectors.colorA = static_cast<u8>((mode0 >> 5U) & 0xFU);
		selectors.colorB = static_cast<u8>((mode1 >> 24U) & 0xFU);
		selectors.colorC = static_cast<u8>(mode0 & 0x1FU);
		selectors.colorD = static_cast<u8>((mode1 >> 6U) & 0x7U);
		selectors.alphaA = static_cast<u8>((mode1 >> 21U) & 0x7U);
		selectors.alphaB = static_cast<u8>((mode1 >> 3U) & 0x7U);
		selectors.alphaC = static_cast<u8>((mode1 >> 18U) & 0x7U);
		selectors.alphaD = static_cast<u8>(mode1 & 0x7U);
	}
	else {
		selectors.colorA = static_cast<u8>((mode0 >> 20U) & 0xFU);
		selectors.colorB = static_cast<u8>((mode1 >> 28U) & 0xFU);
		selectors.colorC = static_cast<u8>((mode0 >> 15U) & 0x1FU);
		selectors.colorD = static_cast<u8>((mode1 >> 15U) & 0x7U);
		selectors.alphaA = static_cast<u8>((mode0 >> 12U) & 0x7U);
		selectors.alphaB = static_cast<u8>((mode1 >> 12U) & 0x7U);
		selectors.alphaC = static_cast<u8>((mode0 >> 9U) & 0x7U);
		selectors.alphaD = static_cast<u8>((mode1 >> 9U) & 0x7U);
	}
	return selectors;
}

inline u64 packCombinerCycleSelectors(const CombinerCycleSelectors & _selectors)
{
	return static_cast<u64>(_selectors.colorA)
		| (static_cast<u64>(_selectors.colorB) << 5U)
		| (static_cast<u64>(_selectors.colorC) << 10U)
		| (static_cast<u64>(_selectors.colorD) << 16U)
		| (static_cast<u64>(_selectors.alphaA) << 20U)
		| (static_cast<u64>(_selectors.alphaB) << 23U)
		| (static_cast<u64>(_selectors.alphaC) << 26U)
		| (static_cast<u64>(_selectors.alphaD) << 29U);
}

struct CombinerColorInputs
{
	u8 combined = 0U;
	u8 texel0 = 0U;
	u8 texel1 = 0U;
	u8 primitive = 0U;
	u8 shade = 0U;
	u8 environment = 0U;
	u8 center = 0U;
	u8 scale = 0U;
	u8 combinedAlpha = 0U;
	u8 texel0Alpha = 0U;
	u8 texel1Alpha = 0U;
	u8 primitiveAlpha = 0U;
	u8 shadeAlpha = 0U;
	u8 environmentAlpha = 0U;
	u8 lodFraction = 0U;
	u8 primLodFrac = 0U;
	u8 noise = 0U;
	u8 k4 = 0U;
	u8 k5 = 0U;
	u8 one = 255U;
	u8 zero = 0U;
};

struct CombinerAlphaInputs
{
	u8 combined = 0U;
	u8 texel0 = 0U;
	u8 texel1 = 0U;
	u8 primitive = 0U;
	u8 shade = 0U;
	u8 environment = 0U;
	u8 primLodFrac = 0U;
	u8 one = 255U;
	u8 zero = 0U;
};

inline u8 selectSyntheticCombinerColorInput(
	u8 _selector,
	const CombinerColorInputs & _in)
{
	switch (_selector & 0x1FU) {
	case 0U:
		return _in.combined;
	case 1U:
		return _in.texel0;
	case 2U:
		return _in.texel1;
	case 3U:
		return _in.primitive;
	case 4U:
		return _in.shade;
	case 5U:
		return _in.environment;
	case 6U:
		return _in.one;
	case 7U:
		return _in.combinedAlpha;
	case 8U:
		return _in.texel0Alpha;
	case 9U:
		return _in.texel1Alpha;
	case 10U:
		return _in.primitiveAlpha;
	case 11U:
		return _in.shadeAlpha;
	case 12U:
		return _in.environmentAlpha;
	case 13U:
		return _in.lodFraction;
	case 14U:
		return _in.primLodFrac;
	case 15U:
		return _in.k5;
	case 16U:
		return _in.noise;
	case 17U:
		return _in.k4;
	case 18U:
		return _in.k5;
	case 19U:
		return _in.one;
	case 20U:
		return _in.zero;
	case 31U:
		return _in.zero;
	default:
		return static_cast<u8>(_in.noise ^ _in.center ^ _in.scale);
	}
}

inline u8 selectSyntheticCombinerAlphaInput(
	u8 _selector,
	const CombinerAlphaInputs & _in)
{
	switch (_selector & 0x7U) {
	case 0U:
		return _in.combined;
	case 1U:
		return _in.texel0;
	case 2U:
		return _in.texel1;
	case 3U:
		return _in.primitive;
	case 4U:
		return _in.shade;
	case 5U:
		return _in.environment;
	case 6U:
		return _in.primLodFrac;
	default:
		return _in.zero;
	}
}

inline u8 evalSyntheticCombinerColorChannel(
	u8 _aSel,
	u8 _bSel,
	u8 _cSel,
	u8 _dSel,
	const CombinerColorInputs & _in)
{
	const s32 a = static_cast<s32>(selectSyntheticCombinerColorInput(_aSel, _in));
	const s32 b = static_cast<s32>(selectSyntheticCombinerColorInput(_bSel, _in));
	const s32 c = static_cast<s32>(selectSyntheticCombinerColorInput(_cSel, _in));
	const s32 d = static_cast<s32>(selectSyntheticCombinerColorInput(_dSel, _in));
	const s32 value = ((a - b) * c + 127) / 255 + d;
	return clampU8FromS32(value);
}

inline u8 evalSyntheticCombinerAlphaChannel(
	u8 _aSel,
	u8 _bSel,
	u8 _cSel,
	u8 _dSel,
	const CombinerAlphaInputs & _in)
{
	const s32 a = static_cast<s32>(selectSyntheticCombinerAlphaInput(_aSel, _in));
	const s32 b = static_cast<s32>(selectSyntheticCombinerAlphaInput(_bSel, _in));
	const s32 c = static_cast<s32>(selectSyntheticCombinerAlphaInput(_cSel, _in));
	const s32 d = static_cast<s32>(selectSyntheticCombinerAlphaInput(_dSel, _in));
	const s32 value = ((a - b) * c + 127) / 255 + d;
	return clampU8FromS32(value);
}

inline u32 applySyntheticCombiner(
	const rvk2::RenderWorkPacket & _work,
	u32 _textureColor,
	u32 _shadeColor,
	u32 _baseColor,
	u32 _dstColor,
	u32 _x,
	u32 _y,
	bool _cycle2Selectors = false)
{
	const CombinerCycleSelectors selectors = decodeCombinerCycleSelectors(
		_work.combineMux,
		_cycle2Selectors && _work.phase == static_cast<u8>(rvk2::RenderPhase::kCycle2));
	const u64 selectorWord = packCombinerCycleSelectors(selectors);
	const ColorRGBA tex = unpackRGBA(_textureColor);
	const ColorRGBA shade = unpackRGBA(_shadeColor);
	const ColorRGBA base = unpackRGBA(_baseColor);
	const ColorRGBA dst = unpackRGBA(_dstColor);
	const ColorRGBA prim = unpackRGBA(_work.primColor);
	const ColorRGBA env = unpackRGBA(_work.envColor);
	const ColorRGBA blend = unpackRGBA(_work.blendColor);
	const ColorRGBA fog = unpackRGBA(_work.fogColor);

	u64 noiseSeed = selectorWord;
	noiseSeed ^= _work.sourcePacketId << 9U;
	noiseSeed ^= static_cast<u64>(_x) << 33U;
	noiseSeed ^= static_cast<u64>(_y) << 45U;
	noiseSeed ^= static_cast<u64>(_work.syncEpoch) << 17U;
	noiseSeed ^= _work.otherModes;
	noiseSeed ^= _work.keyState;
	noiseSeed ^= _work.convertState;
	noiseSeed ^= static_cast<u64>(_work.primColor) << 5U;
	noiseSeed ^= static_cast<u64>(_work.envColor) << 11U;
	noiseSeed ^= static_cast<u64>(_work.blendColor) << 19U;
	noiseSeed ^= static_cast<u64>(_work.fogColor) << 27U;
	noiseSeed *= 0xD6E8FEB86659FD93ULL;

	const ColorRGBA muxConstant{
		static_cast<u8>((selectorWord >> 0U) & 0xFFU),
		static_cast<u8>((selectorWord >> 8U) & 0xFFU),
		static_cast<u8>((selectorWord >> 16U) & 0xFFU),
		static_cast<u8>((selectorWord >> 24U) & 0xFFU)
	};
	auto selectStateColor = [&](u8 _selector, u8 _channelIndex) -> u8 {
		switch (_selector & 0x3U) {
		case 0U:
			return _channelIndex == 0U ? prim.r : (_channelIndex == 1U ? prim.g : (_channelIndex == 2U ? prim.b : prim.a));
		case 1U:
			return _channelIndex == 0U ? env.r : (_channelIndex == 1U ? env.g : (_channelIndex == 2U ? env.b : env.a));
		case 2U:
			return _channelIndex == 0U ? blend.r : (_channelIndex == 1U ? blend.g : (_channelIndex == 2U ? blend.b : blend.a));
		default:
			return _channelIndex == 0U ? fog.r : (_channelIndex == 1U ? fog.g : (_channelIndex == 2U ? fog.b : fog.a));
		}
	};
	const u64 modeSelectorWord = _work.otherModes ^ _work.keyState ^ (_work.convertState << 7U);
	const u8 modeR = static_cast<u8>((modeSelectorWord >> 0U) & 0x3ULL);
	const u8 modeG = static_cast<u8>((modeSelectorWord >> 2U) & 0x3ULL);
	const u8 modeB = static_cast<u8>((modeSelectorWord >> 4U) & 0x3ULL);
	const u8 modeA = static_cast<u8>((modeSelectorWord >> 6U) & 0x3ULL);
	const u8 mixR = static_cast<u8>((_work.convertState >> 0U) & 0xFFULL);
	const u8 mixG = static_cast<u8>((_work.convertState >> 8U) & 0xFFULL);
	const u8 mixB = static_cast<u8>((_work.convertState >> 16U) & 0xFFULL);
	const u8 mixA = static_cast<u8>((_work.convertState >> 24U) & 0xFFULL);
	const auto blendConst = [](u8 _base, u8 _state, u8 _mix) -> u8 {
		const u32 invMix = static_cast<u32>(255U - _mix);
		const u32 value =
			static_cast<u32>(_base) * invMix
			+ static_cast<u32>(_state) * static_cast<u32>(_mix);
		return static_cast<u8>((value + 127U) / 255U);
	};
	const ColorRGBA constant{
		blendConst(muxConstant.r, selectStateColor(modeR, 0U), mixR),
		blendConst(muxConstant.g, selectStateColor(modeG, 1U), mixG),
		blendConst(muxConstant.b, selectStateColor(modeB, 2U), mixB),
		blendConst(muxConstant.a, selectStateColor(modeA, 3U), mixA)
	};
	const ColorRGBA noise{
		static_cast<u8>((noiseSeed >> 8U) & 0xFFU),
		static_cast<u8>((noiseSeed >> 24U) & 0xFFU),
		static_cast<u8>((noiseSeed >> 40U) & 0xFFU),
		255U
	};

	const u8 colorASel = selectors.colorA;
	const u8 colorBSel = selectors.colorB;
	const u8 colorCSel = selectors.colorC;
	const u8 colorDSel = selectors.colorD;
	const u8 alphaASel = selectors.alphaA;
	const u8 alphaBSel = selectors.alphaB;
	const u8 alphaCSel = selectors.alphaC;
	const u8 alphaDSel = selectors.alphaD;
	const u8 lodFraction = laneByteFromDigest(_work.convertState, 0U);
	const u8 primLodFrac = laneByteFromDigest(_work.convertState, 1U);
	const u8 k4 = laneByteFromDigest(_work.convertState, 2U);
	const u8 k5 = laneByteFromDigest(_work.convertState, 3U);
	const ColorRGBA texel1{
		mixU8WithWeight(tex.r, env.r, 96U),
		mixU8WithWeight(tex.g, env.g, 96U),
		mixU8WithWeight(tex.b, env.b, 96U),
		mixU8WithWeight(tex.a, env.a, 96U)
	};

	const auto makeColorInputs = [&](
		u8 _combined,
		u8 _tex0,
		u8 _tex1,
		u8 _primitive,
		u8 _shade,
		u8 _environment,
		u8 _constant,
		u8 _noise,
		u8 _dst,
		u8 _lane) -> CombinerColorInputs {
		CombinerColorInputs in{};
		in.combined = _combined;
		in.texel0 = _tex0;
		in.texel1 = _tex1;
		in.primitive = _primitive;
		in.shade = _shade;
		in.environment = _environment;
		in.center = laneByteFromDigest(_work.keyState, _lane);
		in.scale = laneByteFromDigest(_work.keyState, _lane + 3U);
		in.combinedAlpha = base.a;
		in.texel0Alpha = tex.a;
		in.texel1Alpha = texel1.a;
		in.primitiveAlpha = prim.a;
		in.shadeAlpha = shade.a;
		in.environmentAlpha = env.a;
		in.lodFraction = lodFraction;
		in.primLodFrac = primLodFrac;
		in.noise = _noise;
		in.k4 = k4;
		in.k5 = k5;
		in.one = 255U;
		in.zero = 0U;
		// Blend destination and constant term are folded in to preserve strong destination/constant sensitivity.
		in.combined = mixU8WithWeight(in.combined, _dst, 28U);
		in.primitive = mixU8WithWeight(in.primitive, _constant, 20U);
		return in;
	};

	const auto makeAlphaInputs = [&](u8 _noise) -> CombinerAlphaInputs {
		CombinerAlphaInputs in{};
		in.combined = base.a;
		in.texel0 = tex.a;
		in.texel1 = texel1.a;
		in.primitive = prim.a;
		in.shade = shade.a;
		in.environment = env.a;
		in.primLodFrac = primLodFrac ^ _noise;
		in.one = 255U;
		in.zero = 0U;
		return in;
	};
	const CombinerColorInputs colorInputsR =
		makeColorInputs(base.r, tex.r, texel1.r, prim.r, shade.r, env.r, constant.r, noise.r, dst.r, 0U);
	const CombinerColorInputs colorInputsG =
		makeColorInputs(base.g, tex.g, texel1.g, prim.g, shade.g, env.g, constant.g, noise.g, dst.g, 1U);
	const CombinerColorInputs colorInputsB =
		makeColorInputs(base.b, tex.b, texel1.b, prim.b, shade.b, env.b, constant.b, noise.b, dst.b, 2U);
	const CombinerAlphaInputs alphaInputs = makeAlphaInputs(noise.a);

	const ColorRGBA out{
		evalSyntheticCombinerColorChannel(colorASel, colorBSel, colorCSel, colorDSel, colorInputsR),
		evalSyntheticCombinerColorChannel(colorASel, colorBSel, colorCSel, colorDSel, colorInputsG),
		evalSyntheticCombinerColorChannel(colorASel, colorBSel, colorCSel, colorDSel, colorInputsB),
		evalSyntheticCombinerAlphaChannel(alphaASel, alphaBSel, alphaCSel, alphaDSel, alphaInputs)
	};
	ColorRGBA resolved = out;
	if (_work.textured) {
		const u8 textureInfluence = static_cast<u8>(24U + ((colorASel ^ colorCSel) & 0x1FU));
		resolved.r = mixU8WithWeight(resolved.r, tex.r, textureInfluence);
		resolved.g = mixU8WithWeight(resolved.g, tex.g, textureInfluence);
		resolved.b = mixU8WithWeight(resolved.b, tex.b, textureInfluence);
	}
	if (isImageReadEnabled(_work)) {
		const u8 dstInfluence = static_cast<u8>(16U + ((colorDSel ^ alphaDSel) & 0x1FU));
		resolved.r = mixU8WithWeight(resolved.r, dst.r, dstInfluence);
		resolved.g = mixU8WithWeight(resolved.g, dst.g, dstInfluence);
		resolved.b = mixU8WithWeight(resolved.b, dst.b, dstInfluence);
	}
	const u8 textureDetailMode = decodeTextureDetailMode(_work);
	if (textureDetailMode == 1U) {
		const auto stretch = [](u8 _value) -> u8 {
			const s32 expanded = 128 + ((static_cast<s32>(_value) - 128) * 3) / 2;
			return clampU8FromS32(expanded);
		};
		resolved.r = stretch(resolved.r);
		resolved.g = stretch(resolved.g);
		resolved.b = stretch(resolved.b);
	}
	else if (textureDetailMode == 2U) {
		resolved.r = mixU8WithWeight(resolved.r, tex.r, 48U);
		resolved.g = mixU8WithWeight(resolved.g, tex.g, 48U);
		resolved.b = mixU8WithWeight(resolved.b, tex.b, 48U);
	}
	else if (textureDetailMode == 3U) {
		resolved.r = mixU8WithWeight(resolved.r, noise.r, 40U);
		resolved.g = mixU8WithWeight(resolved.g, noise.g, 40U);
		resolved.b = mixU8WithWeight(resolved.b, noise.b, 40U);
	}
	if (isCombineKeyEnabled(_work)) {
		const u32 laneShift = static_cast<u32>((_x + _y) & 0x7U) * 8U;
		const u8 keyMix = static_cast<u8>((_work.keyState >> laneShift) & 0xFFULL);
		resolved.a = static_cast<u8>(
			(static_cast<u32>(resolved.a) * static_cast<u32>(keyMix) + 127U) / 255U);
		const u8 invMix = static_cast<u8>(255U - keyMix);
		resolved.r = static_cast<u8>(
			(static_cast<u32>(resolved.r) * static_cast<u32>(invMix)
				+ static_cast<u32>(keyMix) * static_cast<u32>(noise.r)
				+ 127U) / 255U);
		resolved.g = static_cast<u8>(
			(static_cast<u32>(resolved.g) * static_cast<u32>(invMix)
				+ static_cast<u32>(keyMix) * static_cast<u32>(noise.g)
				+ 127U) / 255U);
		resolved.b = static_cast<u8>(
			(static_cast<u32>(resolved.b) * static_cast<u32>(invMix)
				+ static_cast<u32>(keyMix) * static_cast<u32>(noise.b)
				+ 127U) / 255U);
	}
	if (isConvertOneEnabled(_work))
		resolved.a = 255U;
	return packRGBA(resolved);
}

inline u8 blendChannel(u8 _src, u8 _dst, u32 _srcWeight, u32 _dstWeight)
{
	const u32 sum = _srcWeight + _dstWeight;
	if (sum == 0U)
		return _src;
	const u32 blended =
		static_cast<u32>(_src) * _srcWeight
		+ static_cast<u32>(_dst) * _dstWeight;
	return static_cast<u8>((blended + (sum / 2U)) / sum);
}

inline s32 bayerDither4x4(u32 _x, u32 _y)
{
	static constexpr s32 kBayer4x4[16] = {
		-8, 0, -6, 2,
		4, -4, 6, -2,
		-5, 3, -7, 1,
		7, -1, 5, -3
	};
	return kBayer4x4[((_y & 0x3U) << 2U) | (_x & 0x3U)];
}

inline s32 syntheticNoiseSigned8(
	const rvk2::RenderWorkPacket & _work,
	u32 _x,
	u32 _y,
	u32 _lane)
{
	u64 seed = 1469598103934665603ULL;
	mixTextureSeed(seed, static_cast<u64>(_x));
	mixTextureSeed(seed, static_cast<u64>(_y));
	mixTextureSeed(seed, static_cast<u64>(_lane));
	mixTextureSeed(seed, static_cast<u64>(_work.sourcePacketId));
	mixTextureSeed(seed, static_cast<u64>(_work.syncEpoch));
	mixTextureSeed(seed, static_cast<u64>(_work.otherModes));
	const s32 raw = static_cast<s32>((seed >> 16U) & 0xFFU);
	return raw - 128;
}

inline u8 applySyntheticDitherMode(
	u8 _value,
	u8 _mode,
	s32 _bayer,
	s32 _noise)
{
	s32 adjusted = static_cast<s32>(_value);
	switch (_mode & 0x3U) {
	case 1U:
		adjusted += _bayer;
		break;
	case 2U:
		adjusted += _noise >> 4U;
		break;
	case 3U:
		adjusted += (_bayer + (_noise >> 4U)) / 2;
		break;
	default:
		break;
	}
	return clampChannelS32(adjusted);
}

struct SyntheticCoverageSample
{
	u8 input = 0U;
	u8 destination = 0U;
	u8 resolved = 0U;
};

inline u8 alphaToCoverage3(u8 _alpha)
{
	return static_cast<u8>(_alpha >> 5U);
}

inline u8 resolveCoverageDestination(u8 _input, u8 _destination, u8 _cvgDest)
{
	switch (_cvgDest & 0x3U) {
	case 0U:
		return static_cast<u8>(std::min<u32>(7U, static_cast<u32>(_input) + static_cast<u32>(_destination)));
	case 1U:
		return static_cast<u8>((static_cast<u32>(_input) + static_cast<u32>(_destination)) & 0x7U);
	case 2U:
		return 7U;
	default:
		return _destination;
	}
}

inline SyntheticCoverageSample evaluateSyntheticCoverage(
	const rvk2::RenderWorkPacket & _work,
	u8 _coverageAlpha,
	u8 _dstAlpha,
	u32 _x,
	u32 _y)
{
	u64 coverageSeed = 1469598103934665603ULL;
	mixTextureSeed(coverageSeed, static_cast<u64>(_x));
	mixTextureSeed(coverageSeed, static_cast<u64>(_y));
	mixTextureSeed(coverageSeed, static_cast<u64>(_work.sourcePacketId));
	mixTextureSeed(coverageSeed, static_cast<u64>(_work.syncEpoch));
	mixTextureSeed(coverageSeed, static_cast<u64>(_work.keyState));
	mixTextureSeed(coverageSeed, static_cast<u64>(_work.blendMask));
	const u8 stochasticCoverage = static_cast<u8>((coverageSeed >> 5U) & 0x7U);

	SyntheticCoverageSample sample{};
	sample.destination = alphaToCoverage3(_dstAlpha);
	u8 inputCoverage = stochasticCoverage;
	if (_work.alphaCvgSel)
		inputCoverage = alphaToCoverage3(_coverageAlpha);
	if (_work.cvgXAlpha) {
		const u8 alphaCoverage = alphaToCoverage3(_coverageAlpha);
		inputCoverage = static_cast<u8>(
			(static_cast<u32>(inputCoverage) * static_cast<u32>(alphaCoverage) + 3U) / 7U);
	}
	sample.input = static_cast<u8>(std::min<u32>(7U, static_cast<u32>(inputCoverage)));
	sample.resolved = resolveCoverageDestination(sample.input, sample.destination, _work.cvgDest);
	return sample;
}

inline u32 applySyntheticBlender(
	const rvk2::RenderWorkPacket & _work,
	u32 _blendParams,
	u32 _srcColor,
	u32 _dstColor,
	u32 _x,
	u32 _y,
	bool _cycle2Selectors = false)
{
	ColorRGBA src = unpackRGBA(_srcColor);
	const ColorRGBA dst = unpackRGBA(_dstColor);
	const ColorRGBA blendState = unpackRGBA(_work.blendColor);
	const ColorRGBA fogState = unpackRGBA(_work.fogColor);
	const ColorRGBA primState = unpackRGBA(_work.primColor);

	u32 srcWeight = static_cast<u32>(_blendParams & 0xFFU);
	u32 dstWeight = static_cast<u32>((_blendParams >> 8U) & 0xFFU);
	const u32 alphaScale = static_cast<u32>((_blendParams >> 16U) & 0xFFU) + 1U;
	const u32 coverageBias = static_cast<u32>((_blendParams >> 24U) & 0xFFU);
	const u32 modeLo = static_cast<u32>(_work.otherModes & 0xFFFFFFFFULL);
	srcWeight += modeLo & 0xFU;
	dstWeight += (modeLo >> 4U) & 0xFU;
	const u32 modeAlphaScale = ((modeLo >> 8U) & 0x1FU) + 1U;
	const u32 modulatedAlphaScale = std::min<u32>(alphaScale * modeAlphaScale, 1024U);
	const u32 dynamicCoverageBias = coverageBias ^ static_cast<u32>(_work.keyState & 0xFFULL);
	const u8 tintMix = static_cast<u8>((_work.convertState >> 16U) & 0xFFULL);
	const u8 fogMix = static_cast<u8>((_work.convertState >> 24U) & 0x7FULL);
	const auto mixChannel = [](u8 _base, u8 _target, u8 _mix) -> u8 {
		const u32 invMix = static_cast<u32>(255U - _mix);
		const u32 value =
			static_cast<u32>(_base) * invMix
			+ static_cast<u32>(_target) * static_cast<u32>(_mix);
		return static_cast<u8>((value + 127U) / 255U);
	};
	src.r = mixChannel(src.r, blendState.r, tintMix);
	src.g = mixChannel(src.g, blendState.g, tintMix);
	src.b = mixChannel(src.b, blendState.b, tintMix);
	src.a = mixChannel(src.a, primState.a, tintMix);
	src.r = mixChannel(src.r, fogState.r, fogMix);
	src.g = mixChannel(src.g, fogState.g, fogMix);
	src.b = mixChannel(src.b, fogState.b, fogMix);

	const bool pipelineMode = isPipelineModeEnabled(_work);
	const bool aaEnable = isAAEnabled(_work);
	const BlendMuxSelectors selectors = decodeBlendMuxSelectors(
		_work,
		_cycle2Selectors && _work.phase == static_cast<u8>(rvk2::RenderPhase::kCycle2));
	const auto selectColorSource = [pipelineMode](
		u8 _selector,
		u8 _src,
		u8 _dst,
		u8 _blend,
		u8 _fog,
		u8 _prim) -> u8 {
		switch (_selector & 0x3U) {
		case 0U:
			return _src;
		case 1U:
			return _dst;
		case 2U:
			return _blend;
		default:
			return pipelineMode ? _prim : _fog;
		}
	};
	const auto selectAlphaSource = [pipelineMode](
		u8 _selector,
		u8 _srcA,
		u8 _dstA,
		u8 _blendA,
		u8 _fogA,
		u8 _primA) -> u8 {
		switch (_selector & 0x3U) {
		case 0U:
			return _srcA;
		case 1U:
			return _dstA;
		case 2U:
			return _blendA;
		default:
			return pipelineMode ? _primA : _fogA;
		}
	};
	const auto mixQuarter = [](u8 _base, u8 _injected) -> u8 {
		return static_cast<u8>(
			(static_cast<u32>(_base) * 3U + static_cast<u32>(_injected) + 2U) / 4U);
	};
	const ColorRGBA srcSelectorColor{
		selectColorSource(selectors.m1b, src.r, dst.r, blendState.r, fogState.r, primState.r),
		selectColorSource(selectors.m1b, src.g, dst.g, blendState.g, fogState.g, primState.g),
		selectColorSource(selectors.m1b, src.b, dst.b, blendState.b, fogState.b, primState.b),
		selectColorSource(selectors.m1b, src.a, dst.a, blendState.a, fogState.a, primState.a)
	};
	const ColorRGBA dstSelectorColor{
		selectColorSource(selectors.m2b, src.r, dst.r, blendState.r, fogState.r, primState.r),
		selectColorSource(selectors.m2b, src.g, dst.g, blendState.g, fogState.g, primState.g),
		selectColorSource(selectors.m2b, src.b, dst.b, blendState.b, fogState.b, primState.b),
		selectColorSource(selectors.m2b, src.a, dst.a, blendState.a, fogState.a, primState.a)
	};
	const u8 srcSelectorAlpha = selectAlphaSource(
		selectors.m1a,
		src.a,
		dst.a,
		blendState.a,
		fogState.a,
		primState.a);
	const u8 dstSelectorAlpha = selectAlphaSource(
		selectors.m2a,
		src.a,
		dst.a,
		blendState.a,
		fogState.a,
		primState.a);
	src.r = mixQuarter(src.r, srcSelectorColor.r);
	src.g = mixQuarter(src.g, srcSelectorColor.g);
	src.b = mixQuarter(src.b, srcSelectorColor.b);
	src.a = mixQuarter(src.a, srcSelectorAlpha);
	ColorRGBA dstResolved{
		mixQuarter(dst.r, dstSelectorColor.r),
		mixQuarter(dst.g, dstSelectorColor.g),
		mixQuarter(dst.b, dstSelectorColor.b),
		mixQuarter(dst.a, dstSelectorAlpha)
	};
	srcWeight += static_cast<u32>(srcSelectorAlpha >> 4U);
	dstWeight += static_cast<u32>(dstSelectorAlpha >> 4U);

	const bool useCoverageControls =
		_work.colorOnCvg
		|| _work.cvgXAlpha
		|| _work.alphaCvgSel
		|| _work.forceBlender
		|| _work.cvgDest != 0U
		|| _work.blendMask != 0U;
	const SyntheticCoverageSample coverage = useCoverageControls
		? evaluateSyntheticCoverage(_work, src.a, dstResolved.a, _x, _y)
		: SyntheticCoverageSample{};
	if (useCoverageControls && _work.cvgXAlpha) {
		const u32 coverageAlphaScale =
			(static_cast<u32>(coverage.resolved) * 255U + 3U) / 7U;
		src.a = static_cast<u8>(
			(static_cast<u32>(src.a) * coverageAlphaScale + 127U) / 255U);
	}

	if (srcWeight == 0U && dstWeight == 0U)
		srcWeight = 255U;

	const u32 srcAlpha = static_cast<u32>(src.a) + 1U;
	srcWeight = (srcWeight * srcAlpha * modulatedAlphaScale + 32767U) / (256U * 256U);
	dstWeight = (dstWeight * (256U - srcAlpha) + 127U) / 256U;

	if (useCoverageControls) {
		const u32 srcCoverageScale = aaEnable ? 20U : 12U;
		const u32 dstCoverageScale = aaEnable ? 12U : 6U;
		srcWeight += static_cast<u32>(coverage.resolved) * srcCoverageScale;
		dstWeight += static_cast<u32>(7U - coverage.resolved) * dstCoverageScale;
		if (pipelineMode) {
			srcWeight += static_cast<u32>(coverage.input) * 2U;
			dstWeight += static_cast<u32>(coverage.destination);
		}
		srcWeight += static_cast<u32>(_work.blendMask & 0x3U);
		dstWeight += static_cast<u32>((_work.blendMask >> 2U) & 0x3U);
		if (_work.forceBlender) {
			srcWeight += 8U;
			dstWeight += 8U;
		}
	}

	if ((_blendParams & 0x80000000U) != 0U) {
		const u32 coverage = (static_cast<u32>(_x) * 29U + static_cast<u32>(_y) * 17U + dynamicCoverageBias) & 0xFFU;
		srcWeight += coverage >> 4U;
		dstWeight += (255U - coverage) >> 4U;
	}
	if (srcWeight == 0U && dstWeight == 0U)
		srcWeight = 1U;

	ColorRGBA out{};
	out.r = blendChannel(src.r, dstResolved.r, srcWeight, dstWeight);
	out.g = blendChannel(src.g, dstResolved.g, srcWeight, dstWeight);
	out.b = blendChannel(src.b, dstResolved.b, srcWeight, dstWeight);
	out.a = blendChannel(src.a, dstResolved.a, srcWeight, dstWeight);
	if (useCoverageControls && _work.alphaCvgSel)
		out.a = static_cast<u8>((static_cast<u32>(coverage.resolved) * 255U + 3U) / 7U);
	if (useCoverageControls && aaEnable) {
		const u8 coverageAlpha = static_cast<u8>(
			(static_cast<u32>(coverage.resolved) * 255U + 3U) / 7U);
		out.a = static_cast<u8>(
			(static_cast<u32>(out.a) * 3U + static_cast<u32>(coverageAlpha) + 2U) / 4U);
	}
	if (pipelineMode) {
		out.r = mixChannel(out.r, primState.r, 24U);
		out.g = mixChannel(out.g, primState.g, 24U);
		out.b = mixChannel(out.b, primState.b, 24U);
	}
	if (useCoverageControls && !aaEnable) {
		const auto quantizeCoverageLane = [](u8 _value) -> u8 {
			const u32 lane = (static_cast<u32>(_value) * 7U + 127U) / 255U;
			return static_cast<u8>((lane * 255U + 3U) / 7U);
		};
		out.r = quantizeCoverageLane(out.r);
		out.g = quantizeCoverageLane(out.g);
		out.b = quantizeCoverageLane(out.b);
	}

	const u8 colorDitherMode = decodeColorDitherMode(_work);
	const u8 alphaDitherMode = decodeAlphaDitherMode(_work);
	if (colorDitherMode != 0U || alphaDitherMode != 0U) {
		// Under active scissoring, the Y dither index uses bits [2:1].
		const u32 ditherY = hasExplicitScissor(_work) ? (_y >> 1U) : _y;
		const s32 bayer = bayerDither4x4(_x, ditherY);
		if (colorDitherMode != 0U) {
			out.r = applySyntheticDitherMode(
				out.r,
				colorDitherMode,
				bayer,
				syntheticNoiseSigned8(_work, _x, _y, 0U));
			out.g = applySyntheticDitherMode(
				out.g,
				colorDitherMode,
				bayer,
				syntheticNoiseSigned8(_work, _x, _y, 1U));
			out.b = applySyntheticDitherMode(
				out.b,
				colorDitherMode,
				bayer,
				syntheticNoiseSigned8(_work, _x, _y, 2U));
		}
		if (alphaDitherMode != 0U) {
			out.a = applySyntheticDitherMode(
				out.a,
				alphaDitherMode,
				bayer,
				syntheticNoiseSigned8(_work, _x, _y, 3U));
		}
	}

	if (isTextureEdgeEnabled(_work) && out.a > 0U && out.a < 128U)
		out.a = 255U;
	if (isConvertOneEnabled(_work))
		out.a = 255U;
	return packRGBA(out);
}

inline u32 applySyntheticBlender(
	const rvk2::RenderWorkPacket & _work,
	u32 _srcColor,
	u32 _dstColor,
	u32 _x,
	u32 _y)
{
	return applySyntheticBlender(_work, _work.blendParams, _srcColor, _dstColor, _x, _y, false);
}

inline bool passesSyntheticAlphaCompare(
	const rvk2::RenderWorkPacket & _work,
	u32 _pixel,
	u32 _x,
	u32 _y)
{
	if (_work.phase == static_cast<u8>(rvk2::RenderPhase::kCopy)
		|| _work.phase == static_cast<u8>(rvk2::RenderPhase::kFill))
		return true;

	if (_work.alphaCompare == 0U)
		return true;

	const u8 alpha = static_cast<u8>(_pixel & 0xFFU);
	if (_work.alphaCompare == 1U) {
		const u8 threshold = static_cast<u8>(_work.blendColor & 0xFFU);
		return alpha >= threshold;
	}
	if (_work.alphaCompare == 2U) {
		const u8 threshold = static_cast<u8>(
			(static_cast<u32>(_x) * 17U
				+ static_cast<u32>(_y) * 29U
				+ static_cast<u32>(_work.syncEpoch & 0xFFU)
				+ static_cast<u32>(_work.keyState & 0xFFULL))
			& 0xFFU);
		return alpha >= threshold;
	}
	return alpha != 0U;
}

inline bool passesSyntheticCoverageWrite(
	const rvk2::RenderWorkPacket & _work,
	u32 _pixel,
	u32 _dstColor,
	u32 _x,
	u32 _y)
{
	if (_work.phase == static_cast<u8>(rvk2::RenderPhase::kCopy)
		|| _work.phase == static_cast<u8>(rvk2::RenderPhase::kFill))
		return true;

	if (!_work.colorOnCvg)
		return true;

	const ColorRGBA src = unpackRGBA(_pixel);
	const ColorRGBA dst = unpackRGBA(_dstColor);
	const SyntheticCoverageSample coverage =
		evaluateSyntheticCoverage(_work, src.a, dst.a, _x, _y);
	return coverage.resolved != 0U;
}

inline s32 combineDYDerivative(s32 _dy, s32 _de, bool _lmajor)
{
	return _lmajor ? (_dy + _de) : (_dy - _de);
}

inline s32 evalCoefficientAtPixel(
	s32 _base,
	s32 _dx,
	s32 _dy,
	u32 _x,
	u32 _y)
{
	const s64 value =
		static_cast<s64>(_base)
		+ static_cast<s64>(_dx) * static_cast<s64>(_x)
		+ static_cast<s64>(_dy) * static_cast<s64>(_y);
	if (value < static_cast<s64>(std::numeric_limits<s32>::min()))
		return std::numeric_limits<s32>::min();
	if (value > static_cast<s64>(std::numeric_limits<s32>::max()))
		return std::numeric_limits<s32>::max();
	return static_cast<s32>(value);
}

inline u8 coefficientComponentByte(s32 _value)
{
	return static_cast<u8>((static_cast<u32>(_value) >> 8U) & 0xFFU);
}

inline u32 evaluateTriangleShadeColor(
	const rvk2::RenderWorkPacket & _work,
	u32 _x,
	u32 _y)
{
	const s32 drdy = combineDYDerivative(_work.triangleShadeDRDY, _work.triangleShadeDRDE, _work.triangleLMajor);
	const s32 dgdy = combineDYDerivative(_work.triangleShadeDGDY, _work.triangleShadeDGDE, _work.triangleLMajor);
	const s32 dbdy = combineDYDerivative(_work.triangleShadeDBDY, _work.triangleShadeDBDE, _work.triangleLMajor);
	const s32 dady = combineDYDerivative(_work.triangleShadeDADY, _work.triangleShadeDADE, _work.triangleLMajor);
	const u8 r = coefficientComponentByte(evalCoefficientAtPixel(_work.triangleShadeR, _work.triangleShadeDRDX, drdy, _x, _y));
	const u8 g = coefficientComponentByte(evalCoefficientAtPixel(_work.triangleShadeG, _work.triangleShadeDGDX, dgdy, _x, _y));
	const u8 b = coefficientComponentByte(evalCoefficientAtPixel(_work.triangleShadeB, _work.triangleShadeDBDX, dbdy, _x, _y));
	const u8 a = coefficientComponentByte(evalCoefficientAtPixel(_work.triangleShadeA, _work.triangleShadeDADX, dady, _x, _y));
	return (static_cast<u32>(r) << 24U)
		| (static_cast<u32>(g) << 16U)
		| (static_cast<u32>(b) << 8U)
		| static_cast<u32>(a);
}

inline u32 evaluateTriangleTextureColor(
	const rvk2::RenderWorkPacket & _work,
	u32 _x,
	u32 _y)
{
	const s32 dsdy = combineDYDerivative(_work.triangleTexDSDY, _work.triangleTexDSDE, _work.triangleLMajor);
	const s32 dtdy = combineDYDerivative(_work.triangleTexDTDY, _work.triangleTexDTDE, _work.triangleLMajor);
	const s32 dwdy = combineDYDerivative(_work.triangleTexDWDY, _work.triangleTexDWDE, _work.triangleLMajor);
	const s32 sRaw = evalCoefficientAtPixel(_work.triangleTexS, _work.triangleTexDSDX, dsdy, _x, _y);
	const s32 tRaw = evalCoefficientAtPixel(_work.triangleTexT, _work.triangleTexDTDX, dtdy, _x, _y);
	const s32 w = evalCoefficientAtPixel(_work.triangleTexW, _work.triangleTexDWDX, dwdy, _x, _y);
	const s32 s = applyTileAxisTransform(
		sRaw,
		_work.tileShifts,
		_work.tileMasks,
		_work.tileCms,
		_work.tileULS,
		_work.tileLRS);
	const s32 t = applyTileAxisTransform(
		tRaw,
		_work.tileShiftt,
		_work.tileMaskt,
		_work.tileCmt,
		_work.tileULT,
		_work.tileLRT);
	const u8 filterMode = decodeTextureFilterMode(_work);
	if (filterMode == 0U)
		return samplePseudoTexelColor(_work, s, t, w, true, _x, _y);

	const s32 sNext = applyTileAxisTransform(
		sRaw + 32,
		_work.tileShifts,
		_work.tileMasks,
		_work.tileCms,
		_work.tileULS,
		_work.tileLRS);
	const s32 tNext = applyTileAxisTransform(
		tRaw + 32,
		_work.tileShiftt,
		_work.tileMaskt,
		_work.tileCmt,
		_work.tileULT,
		_work.tileLRT);
	const u32 fracS = static_cast<u32>(sRaw) & 0x1FU;
	const u32 fracT = static_cast<u32>(tRaw) & 0x1FU;
	const u32 c00 = samplePseudoTexelColor(_work, s, t, w, true, _x, _y);
	const u32 c10 = samplePseudoTexelColor(_work, sNext, t, w, true, _x, _y);
	const u32 c01 = samplePseudoTexelColor(_work, s, tNext, w, true, _x, _y);
	const u32 c11 = samplePseudoTexelColor(_work, sNext, tNext, w, true, _x, _y);
	return applyTextureFilterMode(filterMode, c00, c10, c01, c11, fracS, fracT);
}

inline u32 modulateRGBA(u32 _base, u32 _shade)
{
	const u32 br = (_base >> 24U) & 0xFFU;
	const u32 bg = (_base >> 16U) & 0xFFU;
	const u32 bb = (_base >> 8U) & 0xFFU;
	const u32 ba = _base & 0xFFU;
	const u32 sr = (_shade >> 24U) & 0xFFU;
	const u32 sg = (_shade >> 16U) & 0xFFU;
	const u32 sb = (_shade >> 8U) & 0xFFU;
	const u32 sa = _shade & 0xFFU;
	const u32 r = (br * sr + 127U) / 255U;
	const u32 g = (bg * sg + 127U) / 255U;
	const u32 b = (bb * sb + 127U) / 255U;
	const u32 a = (ba * sa + 127U) / 255U;
	return (r << 24U) | (g << 16U) | (b << 8U) | a;
}

inline s32 evaluateTriangleDepth(
	const rvk2::RenderWorkPacket & _work,
	u32 _x,
	u32 _y)
{
	if (_work.depthSource != 0U) {
		const s64 depth =
			(static_cast<s64>(_work.primDepthZ) << 8U)
			+ static_cast<s64>(_work.primDepthDelta)
				* static_cast<s64>(_x + _y);
		if (depth < static_cast<s64>(std::numeric_limits<s32>::min()))
			return std::numeric_limits<s32>::min();
		if (depth > static_cast<s64>(std::numeric_limits<s32>::max()))
			return std::numeric_limits<s32>::max();
		return static_cast<s32>(depth);
	}
	const s32 dzdy = combineDYDerivative(_work.triangleDZDY, _work.triangleDZDE, _work.triangleLMajor);
	return evalCoefficientAtPixel(_work.triangleZ, _work.triangleDZDX, dzdy, _x, _y);
}

inline bool passesSyntheticDepthCompare(
	const rvk2::RenderWorkPacket & _work,
	s32 _z,
	s32 _depthValue)
{
	if (!_work.depthCompareEnable)
		return true;
	if (_depthValue == std::numeric_limits<s32>::max())
		return true;

	const s64 z = static_cast<s64>(_z);
	const s64 depth = static_cast<s64>(_depthValue);
	switch (decodeDepthMode(_work)) {
	case 0U:
		return z <= depth;
	case 1U:
		return z <= (depth + 0x20LL);
	case 2U:
		return z <= (depth + 0x100LL);
	default:
		return (z - depth) <= 0x40LL && (depth - z) <= 0x40LL;
	}
}

inline bool shouldUpdateSyntheticDepth(
	const rvk2::RenderWorkPacket & _work,
	s32 _z,
	s32 _depthValue)
{
	if (!_work.depthUpdateEnable)
		return false;
	switch (decodeDepthMode(_work)) {
	case 2U:
		return static_cast<s64>(_z) <= static_cast<s64>(_depthValue);
	case 3U:
		return false;
	default:
		return true;
	}
}

inline u32 chooseTriangleTextureSourceColor(
	const rvk2::RenderWorkPacket & _work,
	u32 _x,
	u32 _y)
{
	const bool useTriangleTexture = _work.textured && _work.triangleTextureEnable;
	return useTriangleTexture
		? evaluateTriangleTextureColor(_work, _x, _y)
		: (_work.textured ? pseudoTexel(_work, _x, _y) : pseudoTriangleColor(_work, _x, _y));
}

inline u32 chooseTriangleShadeSourceColor(
	const rvk2::RenderWorkPacket & _work,
	u32 _x,
	u32 _y)
{
	return _work.triangleShadeEnable ? evaluateTriangleShadeColor(_work, _x, _y) : 0xFFFFFFFFU;
}

inline u32 chooseTriangleBaseColor(
	const rvk2::RenderWorkPacket & _work,
	u32 _x,
	u32 _y)
{
	const bool useTriangleShade = _work.triangleShadeEnable;
	const u32 texturedColor = chooseTriangleTextureSourceColor(_work, _x, _y);
	if (!_work.textured && useTriangleShade)
		return evaluateTriangleShadeColor(_work, _x, _y);
	if (!useTriangleShade)
		return texturedColor;
	return modulateRGBA(texturedColor, evaluateTriangleShadeColor(_work, _x, _y));
}

inline u32 runSyntheticCycle1Pipeline(
	const rvk2::RenderWorkPacket & _work,
	u32 _textureColor,
	u32 _shadeColor,
	u32 _baseColor,
	u32 _dstColor,
	u32 _x,
	u32 _y)
{
	const u32 combinedColor = applySyntheticCombiner(
		_work,
		_textureColor,
		_shadeColor,
		_baseColor,
		_dstColor,
		_x,
		_y,
		false);
	return applySyntheticBlender(_work, combinedColor, _dstColor, _x, _y);
}

inline u32 runSyntheticPhasePipeline(
	const rvk2::RenderWorkPacket & _work,
	u32 _textureColor,
	u32 _shadeColor,
	u32 _baseColor,
	u32 _dstColor,
	u32 _x,
	u32 _y,
	u32 * _coverageDestinationColor = nullptr)
{
	const u8 phase = _work.phase;
	if (phase == static_cast<u8>(rvk2::RenderPhase::kCopy)) {
		if (_coverageDestinationColor != nullptr)
			*_coverageDestinationColor = _dstColor;
		return _work.textured ? _textureColor : _baseColor;
	}
	if (phase == static_cast<u8>(rvk2::RenderPhase::kFill)) {
		if (_coverageDestinationColor != nullptr)
			*_coverageDestinationColor = _dstColor;
		return _baseColor;
	}
	if (phase != static_cast<u8>(rvk2::RenderPhase::kCycle2)) {
		if (_coverageDestinationColor != nullptr)
			*_coverageDestinationColor = _dstColor;
		return runSyntheticCycle1Pipeline(_work, _textureColor, _shadeColor, _baseColor, _dstColor, _x, _y);
	}

	const u32 cycle1CombinedColor = applySyntheticCombiner(
		_work,
		_textureColor,
		_shadeColor,
		_baseColor,
		_dstColor,
		_x,
		_y,
		false);
	const u32 cycle1Color = applySyntheticBlender(
		_work,
		cycle1CombinedColor,
		_dstColor,
		_x,
		_y,
		false);
	const u32 stage2CombinedColor = applySyntheticCombiner(
		_work,
		_textureColor,
		_shadeColor,
		cycle1CombinedColor,
		cycle1Color,
			_x,
			_y,
			true);
	if (_coverageDestinationColor != nullptr)
		*_coverageDestinationColor = cycle1Color;
	return applySyntheticBlender(
		_work,
		_work.blendParams,
		stage2CombinedColor,
		cycle1Color,
		_x,
		_y,
		true);
}

inline bool phaseUsesDepth(u8 _phase)
{
	return _phase == static_cast<u8>(rvk2::RenderPhase::kCycle1)
		|| _phase == static_cast<u8>(rvk2::RenderPhase::kCycle2);
}

void writeRect(
	ColorSurface & _surface,
	const rvk2::RenderWorkPacket & _work,
	const rvk2::ExecutorConfig & _config,
	rvk2::ExecutorSummary & _summary)
{
	const u32 ulx = std::min<u32>(_work.rectULX, _work.rectLRX);
	const u32 uly = std::min<u32>(_work.rectULY, _work.rectLRY);
	const u32 lrx = std::max<u32>(_work.rectULX, _work.rectLRX);
	const u32 lry = std::max<u32>(_work.rectULY, _work.rectLRY);
	if (lrx < ulx || lry < uly)
		return;

	const u16 requiredWidth = static_cast<u16>(std::min<u32>(lrx + 1U, _config.maxSurfaceWidth));
	const u16 requiredHeight = static_cast<u16>(std::min<u32>(lry + 1U, _config.maxSurfaceHeight));
	ensureSurfaceSize(_surface, requiredWidth, requiredHeight, _config.maxSurfaceWidth, _config.maxSurfaceHeight);
	WriteBounds bounds{};
	if (!computeWriteBounds(_surface, _work, bounds))
		return;

	for (u32 y = bounds.y0; y <= bounds.y1; ++y) {
		if (!passesScissorFieldFilter(_work, y))
			continue;
			for (u32 x = bounds.x0; x <= bounds.x1; ++x) {
				const size_t colorIdx = pixelIndex(_surface.width, static_cast<u16>(x), static_cast<u16>(y));
				const u32 dstColor = _surface.pixels[colorIdx];
				const u32 pipelineDstColor = isImageReadEnabled(_work) ? dstColor : 0x00000000U;
				u32 coverageDestinationColor = pipelineDstColor;
				const u32 rgba =
					_work.opKind == static_cast<u8>(rvk2::RasterOpKind::kFillRect)
					? decodeFillColor(_work.fillColor, _work.colorImageSize)
					: runSyntheticPhasePipeline(
						_work,
					pseudoTexel(_work, x, y),
						0xFFFFFFFFU,
						pseudoTexel(_work, x, y),
						pipelineDstColor,
						x,
						y,
						&coverageDestinationColor);
				if (!passesSyntheticAlphaCompare(_work, rgba, x, y))
					continue;
				if (!passesSyntheticCoverageWrite(_work, rgba, coverageDestinationColor, x, y))
					continue;
				_surface.pixels[colorIdx] = encodeSurfaceColor(rgba, _work.colorImageSize);
				++_summary.colorWriteCount;
		}
	}
}

void writeTriangle(
	ColorSurface & _surface,
	DepthSurface * _depthSurface,
	const rvk2::RenderWorkPacket & _work,
	const rvk2::ExecutorConfig & _config,
	rvk2::ExecutorSummary & _summary)
{
	const u32 ulx = std::min<u32>(_work.rectULX, _work.rectLRX);
	const u32 uly = std::min<u32>(_work.rectULY, _work.rectLRY);
	const u32 lrx = std::max<u32>(_work.rectULX, _work.rectLRX);
	const u32 lry = std::max<u32>(_work.rectULY, _work.rectLRY);
	if (lrx < ulx || lry < uly)
		return;

	const u16 requiredWidth = static_cast<u16>(std::min<u32>(lrx + 1U, _config.maxSurfaceWidth));
	const u16 requiredHeight = static_cast<u16>(std::min<u32>(lry + 1U, _config.maxSurfaceHeight));
	ensureSurfaceSize(_surface, requiredWidth, requiredHeight, _config.maxSurfaceWidth, _config.maxSurfaceHeight);
	if (_depthSurface != nullptr)
		ensureDepthSurfaceSize(*_depthSurface, requiredWidth, requiredHeight, _config.maxSurfaceWidth, _config.maxSurfaceHeight);

	WriteBounds bounds{};
	if (!computeWriteBounds(_surface, _work, bounds))
		return;

	const double yh = static_cast<double>(_work.triangleYH) * 0.25;
	const double ym = static_cast<double>(_work.triangleYM) * 0.25;
	const double yl = static_cast<double>(_work.triangleYL) * 0.25;
	const double xh = static_cast<double>(_work.triangleXH) / 65536.0;
	const double xl = static_cast<double>(_work.triangleXL) / 65536.0;
	const double xLongAtYL =
		(static_cast<double>(_work.triangleXH)
			+ static_cast<double>(_work.triangleDxHDY) * static_cast<double>(static_cast<s32>(_work.triangleYL) - static_cast<s32>(_work.triangleYH)))
		/ 65536.0;

	const double ax = xh;
	const double ay = yh;
	const double bx = xl;
	const double by = ym;
	const double cx = xLongAtYL;
	const double cy = yl;
	const double area = edgeFunction(ax, ay, bx, by, cx, cy);
	if (area == 0.0)
		return;

	for (u32 y = bounds.y0; y <= bounds.y1; ++y) {
		if (!passesScissorFieldFilter(_work, y))
			continue;
		const double py = static_cast<double>(y) + 0.5;
		for (u32 x = bounds.x0; x <= bounds.x1; ++x) {
			const double px = static_cast<double>(x) + 0.5;
			const double e0 = edgeFunction(ax, ay, bx, by, px, py);
			const double e1 = edgeFunction(bx, by, cx, cy, px, py);
			const double e2 = edgeFunction(cx, cy, ax, ay, px, py);
			const bool inside = area > 0.0
				? (e0 >= 0.0 && e1 >= 0.0 && e2 >= 0.0)
				: (e0 <= 0.0 && e1 <= 0.0 && e2 <= 0.0);
			if (!inside)
				continue;

				const size_t colorIdx = pixelIndex(_surface.width, static_cast<u16>(x), static_cast<u16>(y));
				const u32 dstColor = _surface.pixels[colorIdx];
				const u32 pipelineDstColor = isImageReadEnabled(_work) ? dstColor : 0x00000000U;
				u32 coverageDestinationColor = pipelineDstColor;
				const u32 textureColor = chooseTriangleTextureSourceColor(_work, x, y);
				const u32 shadeColor = chooseTriangleShadeSourceColor(_work, x, y);
				const u32 baseColor = chooseTriangleBaseColor(_work, x, y);
			const u32 rgba = runSyntheticPhasePipeline(
				_work,
				textureColor,
					shadeColor,
					baseColor,
					pipelineDstColor,
					x,
					y,
					&coverageDestinationColor);
				if (!passesSyntheticAlphaCompare(_work, rgba, x, y))
					continue;
				if (!passesSyntheticCoverageWrite(_work, rgba, coverageDestinationColor, x, y))
					continue;

			if (_depthSurface != nullptr
				&& phaseUsesDepth(_work.phase)
				&& _work.depthTest
				&& _work.triangleZBufferEnable
				&& (_work.depthCompareEnable || _work.depthUpdateEnable)) {
				const s32 z = evaluateTriangleDepth(_work, x, y);
				const size_t depthIdx = pixelIndex(_depthSurface->width, static_cast<u16>(x), static_cast<u16>(y));
				if (depthIdx >= _depthSurface->values.size())
					continue;
				const s32 depthValue = _depthSurface->values[depthIdx];
				if (!passesSyntheticDepthCompare(_work, z, depthValue))
					continue;
				if (shouldUpdateSyntheticDepth(_work, z, depthValue))
					_depthSurface->values[depthIdx] = z;
			}

			_surface.pixels[colorIdx] = encodeSurfaceColor(rgba, _work.colorImageSize);
			++_summary.colorWriteCount;
		}
	}
}
} // namespace

namespace rvk2 {

ExecutorConfig loadExecutorConfigFromEnv()
{
	ExecutorConfig config{};
	const VIRendererConfig viConfig = loadVIRendererConfigFromEnv();
	config.presentAspectX = viConfig.aspectX;
	config.presentAspectY = viConfig.aspectY;
	const char * txEnable = std::getenv("REALITYVK_RVK2_TEX_REPLACEMENT");
	if (envStringIsTrue(txEnable))
		config.textureReplacementEnable = true;
	const char * txLogSummary = std::getenv("REALITYVK_RVK2_TX_LOG_SUMMARY");
	if (envStringIsTrue(txLogSummary))
		config.textureReplacementLogSummary = true;
	const char * txSummaryPath = std::getenv("REALITYVK_RVK2_TX_SUMMARY_PATH");
	if (txSummaryPath != nullptr && txSummaryPath[0] != '\0')
		config.textureReplacementSummaryPath = txSummaryPath;
	const char * txCachePath = std::getenv("REALITYVK_RVK2_TX_HTC_PATH");
	if (txCachePath != nullptr && txCachePath[0] != '\0') {
		config.textureReplacementCachePath = txCachePath;
		config.textureReplacementEnable = true;
	}
	const char * txPackPath = std::getenv("REALITYVK_RVK2_TX_PACK_PATH");
	if (txPackPath != nullptr && txPackPath[0] != '\0') {
		config.textureReplacementPackPath = txPackPath;
		config.textureReplacementEnable = true;
	}
	u64 parsed = 0ULL;
	if (parseEnvUnsigned(std::getenv("REALITYVK_RVK2_TX_MAX_ENTRIES"), parsed))
		config.textureReplacementMaxEntries = static_cast<u32>(std::min<u64>(parsed, 0xFFFFFFFFULL));
	if (parseEnvUnsigned(std::getenv("REALITYVK_RVK2_TX_MAX_PIXELS"), parsed))
		config.textureReplacementMaxPixels = parsed;
	if (parseEnvUnsigned(std::getenv("REALITYVK_RVK2_TX_RELOAD_TOKEN"), parsed))
		config.textureReplacementReloadToken = parsed;
	if (parseEnvUnsigned(std::getenv("REALITYVK_RVK2_TX_INVALIDATE_TOKEN"), parsed))
		config.textureReplacementInvalidateToken = parsed;
	applyTextureReplacementControlFile(
		std::getenv("REALITYVK_RVK2_TX_CONTROL_FILE"),
		config);
	return config;
}

Executor::Executor(const ExecutorConfig & _config)
	: m_config(_config)
{
}

void Executor::updateConfig(const ExecutorConfig & _config)
{
	const bool replacementConfigChanged =
		m_config.textureReplacementEnable != _config.textureReplacementEnable
		|| m_config.textureReplacementCachePath != _config.textureReplacementCachePath
		|| m_config.textureReplacementPackPath != _config.textureReplacementPackPath
		|| m_config.textureReplacementMaxEntries != _config.textureReplacementMaxEntries
		|| m_config.textureReplacementMaxPixels != _config.textureReplacementMaxPixels;
	const bool reloadTokenChanged =
		m_config.textureReplacementReloadToken != _config.textureReplacementReloadToken;
	const bool invalidateTokenChanged =
		m_config.textureReplacementInvalidateToken != _config.textureReplacementInvalidateToken;

	m_config = _config;

	if (!_config.textureReplacementEnable
		|| invalidateTokenChanged
		|| replacementConfigChanged
		|| reloadTokenChanged) {
		m_textureReplacementStore.clear();
		m_textureReplacementLoaded = false;
	}
}

void Executor::ensureTextureReplacementLoaded()
{
	if (m_textureReplacementLoaded)
		return;
	m_textureReplacementLoaded = true;
	if (!m_config.textureReplacementEnable)
		return;
	TextureReplacementStore loadedStore{};
	if (!m_config.textureReplacementCachePath.empty()) {
		rvk2::loadTextureReplacementHTC(
			m_config.textureReplacementCachePath.c_str(),
			loadedStore);
	}
	if (!m_config.textureReplacementPackPath.empty()) {
		rvk2::loadTextureReplacementPack(
			m_config.textureReplacementPackPath.c_str(),
			loadedStore);
	}
	loadedStore.applyLimits(
		static_cast<size_t>(m_config.textureReplacementMaxEntries),
		m_config.textureReplacementMaxPixels);
	m_textureReplacementStore = std::move(loadedStore);
}

ExecutorOutput Executor::executeWithOutput(
	const std::vector<RenderWorkPacket> & _workPackets,
	const std::vector<SubmissionBatchPacket> & _batches)
{
	ensureTextureReplacementLoaded();
	const TextureReplacementStore * previousReplacementStore = gActiveTextureReplacementStore;
	gActiveTextureReplacementStore =
		(!m_textureReplacementStore.empty() && m_config.textureReplacementEnable)
		? &m_textureReplacementStore
		: nullptr;

	ExecutorOutput output{};
	ExecutorSummary & summary = output.summary;
	summary.presentAspectX = m_config.presentAspectX;
	summary.presentAspectY = m_config.presentAspectY;
	summary.textureReplacementEnabled = m_config.textureReplacementEnable;
	summary.textureReplacementEntryCount = static_cast<u64>(m_textureReplacementStore.entryCount());
	summary.textureReplacementPixelCount = m_textureReplacementStore.totalPixels();
	ExecutorSummary * previousExecutorSummary = gActiveExecutorSummary;
	gActiveExecutorSummary = &summary;

	VIRendererConfig viConfig{};
	viConfig.aspectX = m_config.presentAspectX;
	viConfig.aspectY = m_config.presentAspectY;
	viConfig.maxOutputWidth = m_config.maxSurfaceWidth;
	viConfig.maxOutputHeight = m_config.maxSurfaceHeight;
	const VIRenderer viRenderer(viConfig);

	std::unordered_map<u32, ColorSurface> surfaces;
	std::unordered_map<u32, DepthSurface> depthSurfaces;
	u32 lastSurfaceAddress = 0U;

	for (const SubmissionBatchPacket & batch : _batches) {
		++summary.executedBatchCount;
		if (batch.firstWorkIndex >= _workPackets.size())
			continue;
		const u32 lastIndex = std::min<u32>(
			batch.lastWorkIndex,
			static_cast<u32>(_workPackets.size() - 1U));

		for (u32 workIndex = batch.firstWorkIndex; workIndex <= lastIndex; ++workIndex) {
			const RenderWorkPacket & work = _workPackets[workIndex];
			++summary.executedWorkCount;
			if (work.opKind != static_cast<u8>(RasterOpKind::kFillRect)
				&& work.opKind != static_cast<u8>(RasterOpKind::kTexRect)
				&& work.opKind != static_cast<u8>(RasterOpKind::kTriangle))
				continue;

			ColorSurface & surface = surfaces[work.colorImageAddress];
			surface.format = work.colorImageFormat;
			surface.size = work.colorImageSize;
			if (surface.width == 0U)
				surface.width = std::max<u16>(1U, std::min<u16>(work.colorImageWidth, m_config.maxSurfaceWidth));
			if (surface.height == 0U)
				surface.height = 1U;
			if (surface.pixels.empty())
				surface.pixels.resize(static_cast<size_t>(surface.width) * static_cast<size_t>(surface.height), 0U);

			if (work.opKind == static_cast<u8>(RasterOpKind::kTriangle)) {
				DepthSurface * depthSurface = nullptr;
				if (phaseUsesDepth(work.phase) && work.depthTest && work.triangleZBufferEnable) {
					const u32 depthAddress = work.depthImageAddress != 0U
						? work.depthImageAddress
						: work.colorImageAddress;
					DepthSurface & depth = depthSurfaces[depthAddress];
					if (depth.width == 0U)
						depth.width = std::max<u16>(1U, std::min<u16>(work.colorImageWidth, m_config.maxSurfaceWidth));
					if (depth.height == 0U)
						depth.height = 1U;
					if (depth.values.empty()) {
						depth.values.resize(
							static_cast<size_t>(depth.width) * static_cast<size_t>(depth.height),
							std::numeric_limits<s32>::max());
					}
					depthSurface = &depth;
				}
				writeTriangle(surface, depthSurface, work, m_config, summary);
			}
			else
				writeRect(surface, work, m_config, summary);
			lastSurfaceAddress = work.colorImageAddress;
		}
	}

	summary.surfaceCount = static_cast<u64>(surfaces.size());
	u32 presentSurfaceAddress = lastSurfaceAddress;
	bool viOriginMatchedSurface = false;
	if (m_config.viRegistersValid) {
		const u32 viOriginAddress = m_config.viOrigin & 0x00FFFFFFU;
		u32 matchedSurfaceAddress = presentSurfaceAddress;
		if (chooseSurfaceForVIOrigin(surfaces, viOriginAddress, matchedSurfaceAddress)) {
			presentSurfaceAddress = matchedSurfaceAddress;
			viOriginMatchedSurface = true;
		}
	}
	const auto it = surfaces.find(presentSurfaceAddress);
	if (it != surfaces.end()) {
		VIFrameInput presentInput{};
		presentInput.sourceAddressValid = viOriginMatchedSurface;
		presentInput.sourceAddress = presentSurfaceAddress;
		presentInput.sourceWidth = it->second.width;
		presentInput.sourceHeight = it->second.height;
		presentInput.sourcePixels = &it->second.pixels;
		presentInput.registers.valid = m_config.viRegistersValid;
		presentInput.registers.status = m_config.viStatus;
		presentInput.registers.origin = m_config.viOrigin;
		presentInput.registers.width = m_config.viWidth;
		presentInput.registers.vCurrentLine = m_config.viVCurrentLine;
		presentInput.registers.vSync = m_config.viVSync;
		presentInput.registers.hStart = m_config.viHStart;
		presentInput.registers.vStart = m_config.viVStart;
		presentInput.registers.xScale = m_config.viXScale;
		presentInput.registers.yScale = m_config.viYScale;
		const VIFrameSummary viSummary = viRenderer.present(presentInput, &output.presentFrame.pixels);
		summary.presentHash = viSummary.presentHash;
		summary.presentWidth = viSummary.presentWidth;
		summary.presentHeight = viSummary.presentHeight;
		output.presentFrame.width = viSummary.presentWidth;
		output.presentFrame.height = viSummary.presentHeight;
	}
	gActiveExecutorSummary = previousExecutorSummary;
	gActiveTextureReplacementStore = previousReplacementStore;
	return output;
}

ExecutorSummary Executor::execute(
	const std::vector<RenderWorkPacket> & _workPackets,
	const std::vector<SubmissionBatchPacket> & _batches)
{
	return executeWithOutput(_workPackets, _batches).summary;
}

} // namespace rvk2
