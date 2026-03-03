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

#include "N64.h"
#include "rvk2_VIRenderer.h"

namespace {

thread_local const rvk2::TextureReplacementStore * gActiveTextureReplacementStore = nullptr;
thread_local rvk2::ExecutorSummary * gActiveExecutorSummary = nullptr;
thread_local const u64 * gActiveExecutorTMEMWords = nullptr;
constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;

inline void updateHashByte(u64 & _hash, u8 _value)
{
	_hash ^= static_cast<u64>(_value);
	_hash *= kFnvPrime;
}

inline u64 hashSurfacePixels(const std::vector<u32> & _pixels)
{
	u64 hash = kFnvOffset;
	for (u32 pixel : _pixels) {
		updateHashByte(hash, static_cast<u8>((pixel >> 0U) & 0xFFU));
		updateHashByte(hash, static_cast<u8>((pixel >> 8U) & 0xFFU));
		updateHashByte(hash, static_cast<u8>((pixel >> 16U) & 0xFFU));
		updateHashByte(hash, static_cast<u8>((pixel >> 24U) & 0xFFU));
	}
	return hash;
}

inline const u64 * activeTMEMWords()
{
	return gActiveExecutorTMEMWords != nullptr ? gActiveExecutorTMEMWords : TMEM;
}

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

enum class DebugStageViewMode : u8
{
	kFinal = 0U,
	kTexelRaw,
	kCombinerOut,
	kBlenderOut,
	kVISource,
};

DebugStageViewMode debugStageViewMode()
{
	static const DebugStageViewMode mode = []() -> DebugStageViewMode {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_STAGE_VIEW");
		if (raw == nullptr || raw[0] == '\0')
			return DebugStageViewMode::kFinal;
		const std::string token = toLowerAscii(trimAsciiWhitespace(raw));
		if (token == "off" || token == "final")
			return DebugStageViewMode::kFinal;
		if (token == "texel_raw" || token == "texel")
			return DebugStageViewMode::kTexelRaw;
		if (token == "combiner_out" || token == "combiner")
			return DebugStageViewMode::kCombinerOut;
		if (token == "blender_out" || token == "blender")
			return DebugStageViewMode::kBlenderOut;
		if (token == "vi_source" || token == "source")
			return DebugStageViewMode::kVISource;
		return DebugStageViewMode::kFinal;
	}();
	return mode;
}

bool debugDisableCycle2PrevMemoryColor()
{
	static const bool disabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_DISABLE_CYCLE2_PREV_MEMORY");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return disabled;
}

bool debugForceBlenderDivide()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_FORCE_BLEND_DIVIDE");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugSwapTmem16Samples()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_SWAP_TMEM16");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugDisableTriangleWrites()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_DISABLE_TRIANGLE_WRITES");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugDisableTexRectWrites()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_DISABLE_TEXRECT_WRITES");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugSwapTmem4Nibbles()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_SWAP_TMEM4_NIBBLES");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugAltTmem8OddXor()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_ALT_TMEM8_XOR");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugDisableTexturePerspCoord()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_DISABLE_TEXTURE_PERSP_COORD");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugDisableTextureLodCoord()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_DISABLE_TEXTURE_LOD_COORD");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugDisableCoverageControls()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_DISABLE_COVERAGE_CONTROLS");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugDisableColorOnCvgInhibit()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_DISABLE_COLOR_ON_CVG_INHIBIT");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugBypassBlender()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_BYPASS_BLENDER");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugDisableBlenderDither()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_DISABLE_BLENDER_DITHER");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugDisableBlendMemoryColorSource()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_DISABLE_BLEND_MEMORY_COLOR_SOURCE");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugDisableImageRead()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_DISABLE_IMAGE_READ");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugForceTexelAlphaOpaque()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_FORCE_TEXEL_ALPHA_OPAQUE");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugCycle2SecondPassMemoryFromCycle1()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_CYCLE2_SECOND_PASS_MEMORY_FROM_CYCLE1");
		if (raw == nullptr || raw[0] == '\0')
			return true;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : true;
	}();
	return enabled;
}

bool debugDisableTextureLUTApply()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_DISABLE_TEXTURE_LUT_APPLY");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugCycle1CombinerUseCycle1Selectors()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_CYCLE1_COMBINER_USE_CYCLE1_SELECTORS");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugTmem32UseDirectLinearFetch()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_TMEM32_DIRECT_LINEAR");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugTmem32UseXor02()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_TMEM32_XOR02");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugTmem32PackHighToLowRGBA()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_TMEM32_PACK_HIGH_TO_LOW");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugTextureFilterStrictPrimary()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_TEXTURE_FILTER_STRICT_PRIMARY");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugTextureFilterMode3UsesBilerp()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_TEXTURE_FILTER_MODE3_BILERP");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugTextureFilterMode2UsesAverage()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_TEXTURE_FILTER_MODE2_AVERAGE");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugPseudoTriangleUsePrimColor()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_PSEUDO_TRIANGLE_USE_PRIM_COLOR");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugForceAllTexelAlphaOpaque()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_FORCE_ALL_TEXEL_ALPHA_OPAQUE");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugForceTexel1UsesTile0()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_FORCE_TEXEL1_TILE0");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugForcePipelineModeOn()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_FORCE_PIPELINE_MODE_ON");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
}

bool debugForcePipelineModeOff()
{
	static const bool enabled = []() -> bool {
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_FORCE_PIPELINE_MODE_OFF");
		if (raw == nullptr || raw[0] == '\0')
			return false;
		bool parsed = false;
		return parseBooleanToken(raw, parsed) ? parsed : false;
	}();
	return enabled;
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

enum : u8
{
	kTextureBucketMaskDisabled = 0U,
	kTextureBucketMaskAny = 1U,
	kTextureBucketMaskLUTOnly = 2U,
	kTextureBucketMaskNoLUT = 3U,
};

struct DebugTextureBucketMaskConfig
{
	bool enabled = false;
	std::array<u8, rvk2::kExecutorTextureFormatSizeBuckets> mode{};
};

inline u8 mergeTextureBucketMaskMode(u8 _current, u8 _incoming)
{
	if (_current == kTextureBucketMaskDisabled)
		return _incoming;
	if (_incoming == kTextureBucketMaskDisabled || _current == _incoming)
		return _current;
	if (_current == kTextureBucketMaskAny || _incoming == kTextureBucketMaskAny)
		return kTextureBucketMaskAny;
	return kTextureBucketMaskAny;
}

inline bool parseTextureBucketMaskToken(
	const std::string & _token,
	u8 & _format,
	u8 & _size,
	u8 & _mode)
{
	const std::string token = toLowerAscii(trimAsciiWhitespace(_token));
	if (token.empty() || token[0] != 'f')
		return false;

	size_t pos = 1U;
	while (pos < token.size() && token[pos] >= '0' && token[pos] <= '9')
		++pos;
	if (pos == 1U || pos >= token.size() || token[pos] != 's')
		return false;

	const std::string formatToken = token.substr(1U, pos - 1U);
	++pos;
	const size_t sizeBegin = pos;
	while (pos < token.size() && token[pos] >= '0' && token[pos] <= '9')
		++pos;
	if (pos == sizeBegin)
		return false;

	const std::string sizeToken = token.substr(sizeBegin, pos - sizeBegin);
	u64 formatValue = 0ULL;
	u64 sizeValue = 0ULL;
	if (!parseUnsignedToken(formatToken, formatValue)
		|| !parseUnsignedToken(sizeToken, sizeValue))
		return false;
	if (formatValue >= rvk2::kExecutorTextureFormatBuckets
		|| sizeValue >= rvk2::kExecutorTextureSizeBuckets)
		return false;

	u8 mode = kTextureBucketMaskAny;
	if (pos < token.size()) {
		const std::string suffix = token.substr(pos);
		if (suffix == "l" || suffix == "lut")
			mode = kTextureBucketMaskLUTOnly;
		else if (suffix == "n" || suffix == "nolut")
			mode = kTextureBucketMaskNoLUT;
		else
			return false;
	}

	_format = static_cast<u8>(formatValue);
	_size = static_cast<u8>(sizeValue);
	_mode = mode;
	return true;
}

DebugTextureBucketMaskConfig debugTextureBucketMaskConfig()
{
	static const DebugTextureBucketMaskConfig config = []() -> DebugTextureBucketMaskConfig {
		DebugTextureBucketMaskConfig parsed{};
		const char * raw = std::getenv("REALITYVK_RVK2_DEBUG_TEXTURE_BUCKET_MASK");
		if (raw == nullptr || raw[0] == '\0')
			return parsed;

		const std::string value(raw);
		size_t begin = 0U;
		while (begin <= value.size()) {
			const size_t comma = value.find(',', begin);
			const std::string token = trimAsciiWhitespace(
				comma == std::string::npos
					? value.substr(begin)
					: value.substr(begin, comma - begin));
			if (!token.empty()) {
				const std::string lower = toLowerAscii(token);
				if (lower == "off" || lower == "none")
					return DebugTextureBucketMaskConfig{};
				if (lower == "*" || lower == "all") {
					parsed.enabled = true;
					std::fill(
						parsed.mode.begin(),
						parsed.mode.end(),
						static_cast<u8>(kTextureBucketMaskAny));
				}
				else {
					u8 format = 0U;
					u8 size = 0U;
					u8 mode = kTextureBucketMaskDisabled;
					if (parseTextureBucketMaskToken(lower, format, size, mode)) {
						parsed.enabled = true;
						const u32 index =
							static_cast<u32>(format) * rvk2::kExecutorTextureSizeBuckets
							+ static_cast<u32>(size);
						parsed.mode[index] =
							mergeTextureBucketMaskMode(parsed.mode[index], mode);
					}
				}
			}

			if (comma == std::string::npos)
				break;
			begin = comma + 1U;
		}

		return parsed;
	}();
	return config;
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
		if (key == "cache_path" || key == "hts_path") {
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
	if (debugDisableImageRead())
		return false;
	return (mode1Word(_work) & (1U << 6U)) != 0U;
}

inline bool isAAEnabled(const rvk2::RenderWorkPacket & _work)
{
	return (mode1Word(_work) & (1U << 3U)) != 0U;
}

inline bool isPipelineModeEnabled(const rvk2::RenderWorkPacket & _work)
{
	if (debugForcePipelineModeOn())
		return true;
	if (debugForcePipelineModeOff())
		return false;
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
	if (debugTextureFilterStrictPrimary())
		return filterModePrimary;
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

inline s32 perspectiveDivideFixed16(s32 _coord, s32 _w)
{
	if (_w == 0)
		return _coord;

	const __int128 den = static_cast<__int128>(_w);
	__int128 num = static_cast<__int128>(_coord) << 16U;
	const __int128 absDen = den < 0 ? -den : den;
	const __int128 round = absDen >> 1U;
	const bool sameSign = (num >= 0) == (den >= 0);
	num += sameSign ? round : -round;

	const __int128 value = num / den;
	if (value < static_cast<__int128>(std::numeric_limits<s32>::min()))
		return std::numeric_limits<s32>::min();
	if (value > static_cast<__int128>(std::numeric_limits<s32>::max()))
		return std::numeric_limits<s32>::max();
	return static_cast<s32>(value);
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

struct TileAxisSampleCoord
{
	s32 texel = 0;
	u8 frac = 0U;
};

inline s32 applyTileAxisShiftCoord5(
	s32 _coord5,
	u8 _shift)
{
	if (_shift != 0U) {
		s64 shifted = static_cast<s64>(_coord5);
		const u8 effectiveShift = static_cast<u8>(std::min<u32>(_shift, 15U));
		if (effectiveShift <= 10U)
			shifted >>= effectiveShift;
		else
			shifted <<= static_cast<u8>(16U - effectiveShift);
		return clampS32FromS64(shifted);
	}
	return _coord5;
}

inline TileAxisSampleCoord applyTileAxisTransform(
	s32 _coord5,
	u8 _shift,
	u8 _mask,
	u8 _cm,
	u16 _lo,
	u16 _hi,
	bool _clampAllowed)
{
	const s32 shiftedCoord5 = applyTileAxisShiftCoord5(_coord5, _shift);
	TileAxisSampleCoord mapped{};
	mapped.texel = shiftedCoord5 >> 5U;
	mapped.frac = static_cast<u8>(static_cast<u32>(shiftedCoord5) & 0x1FU);

	const s32 loTexel = static_cast<s32>(_lo >> 2U);
	const s32 hiTexel = static_cast<s32>(_hi >> 2U);
	const s32 tileBase = std::min(loTexel, hiTexel);
	const s32 tileTop = std::max(loTexel, hiTexel);
	s32 localTexel = mapped.texel - tileBase;

	const bool mirror = (_cm & 0x1U) != 0U;
	const bool clamp = ((_cm & 0x2U) != 0U) && _clampAllowed;
	if (clamp) {
		const s32 maxLocal = std::max<s32>(0, tileTop - tileBase);
		if (localTexel < 0)
			localTexel = 0;
		else if (localTexel > maxLocal)
			localTexel = maxLocal;
	}
	else if (_mask != 0U) {
		const s32 period = 1 << std::min<u32>(_mask, 15U);
		if (mirror) {
			const s32 mirrorPeriod = period << 1U;
			s32 wrapped = wrapCoordPositive(localTexel, mirrorPeriod);
			if (wrapped >= period)
				wrapped = (mirrorPeriod - 1) - wrapped;
			localTexel = wrapped;
		}
		else
			localTexel = wrapCoordPositive(localTexel, period);
	}

	mapped.texel = tileBase + localTexel;
	return mapped;
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
	mixTextureSeed(seed, static_cast<u64>(_work.textured ? 1U : 0U));
	mixTextureSeed(seed, static_cast<u64>(_work.texRectFlip ? 1U : 0U));
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_work.texS)));
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_work.texT)));
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_work.texDSDX)));
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_work.texDTDY)));
	mixTextureSeed(seed, static_cast<u64>(_work.triangleTextureEnable ? 1U : 0U));
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_work.triangleTexS)));
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_work.triangleTexT)));
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_work.triangleTexW)));
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_work.triangleTexDSDX)));
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_work.triangleTexDTDX)));
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_work.triangleTexDWDX)));
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_work.triangleTexDSDY)));
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_work.triangleTexDTDY)));
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_work.triangleTexDWDY)));
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_work.triangleTexDSDE)));
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_work.triangleTexDTDE)));
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_work.triangleTexDWDE)));
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
	// N64 filter mode mapping:
	// 0: point, 2: bilerp, 3: average (1 is reserved/invalid).
	const u8 mode = static_cast<u8>(_filterMode & 0x3U);
	if (mode == 2U) {
		if (debugTextureFilterMode2UsesAverage())
			return averageColorRGBA(_c00, _c10, _c01, _c11);
		return bilerpColorRGBA(_c00, _c10, _c01, _c11, _fracS, _fracT);
	}
	if (mode == 1U)
		return bilerpColorRGBA(_c00, _c10, _c01, _c11, _fracS, _fracT);
	if (mode == 3U)
		return debugTextureFilterMode3UsesBilerp()
			? bilerpColorRGBA(_c00, _c10, _c01, _c11, _fracS, _fracT)
			: averageColorRGBA(_c00, _c10, _c01, _c11);
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
	if (_includeW
		&& isTexturePerspEnabled(_work)
		&& !debugDisableTexturePerspCoord()
		&& _w != 0) {
		s = perspectiveDivideFixed16(static_cast<s32>(s), _w);
		t = perspectiveDivideFixed16(static_cast<s32>(t), _w);
	}

	if (isTextureLodEnabled(_work) && !debugDisableTextureLodCoord()) {
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
	(void)_seed;
	if (debugDisableTextureLUTApply())
		return _rgba;
	const u8 lutMode = decodeTextureLUTMode(_work);
	if (lutMode == 0U)
		return _rgba;

	// CI decode stores index in RGB lanes.
	const u8 index = static_cast<u8>((_rgba >> 24U) & 0xFFU);
	const u16 tlut = static_cast<u16>(activeTMEMWords()[(0x100U + static_cast<u32>(index)) & 0x1FFU] & 0xFFFFULL);
	if (lutMode == 3U) {
		// IA16 TLUT entries follow A:I byte order.
		const u8 a = static_cast<u8>((tlut >> 8U) & 0xFFU);
		const u8 i = static_cast<u8>(tlut & 0xFFU);
		return (static_cast<u32>(i) << 24U)
			| (static_cast<u32>(i) << 16U)
			| (static_cast<u32>(i) << 8U)
			| static_cast<u32>(a);
	}

	// RGBA16 TLUT entries in TMEM use swapped 16-bit word order.
	const u16 tlutRgba = static_cast<u16>((tlut << 8U) | (tlut >> 8U));
	const auto expand5 = [](u16 _v) -> u8 {
		return static_cast<u8>((static_cast<u32>(_v) * 255U + 15U) / 31U);
	};
	const u8 r = expand5(static_cast<u16>((tlutRgba >> 11U) & 0x1FU));
	const u8 g = expand5(static_cast<u16>((tlutRgba >> 6U) & 0x1FU));
	const u8 b = expand5(static_cast<u16>((tlutRgba >> 1U) & 0x1FU));
	const u8 a = (tlutRgba & 0x1U) != 0U ? 255U : 0U;
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
	const s32 n0 = static_cast<s32>((_seed >> 8U) & 0xFFULL);
	const s32 n1 = static_cast<s32>((_seed >> 24U) & 0xFFULL);
	const s32 n2 = static_cast<s32>((_seed >> 40U) & 0xFFULL);

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

inline u8 expand5To8(u8 _value)
{
	return static_cast<u8>((static_cast<u32>(_value) * 255U + 15U) / 31U);
}

inline u8 expand4To8(u8 _value)
{
	return static_cast<u8>((_value << 4U) | _value);
}

inline u8 expand3To8(u8 _value)
{
	return static_cast<u8>((static_cast<u32>(_value) * 255U + 3U) / 7U);
}

inline u32 packRgba8(u8 _r, u8 _g, u8 _b, u8 _a)
{
	return (static_cast<u32>(_r) << 24U)
		| (static_cast<u32>(_g) << 16U)
		| (static_cast<u32>(_b) << 8U)
		| static_cast<u32>(_a);
}

inline bool rdramReadable()
{
	return RDRAM != nullptr && RDRAMSize != 0U;
}

inline u8 readRdramByteWrapped(u32 _address)
{
	return RDRAM[(_address & RDRAMSize) ^ 3U];
}

inline u16 readRdramU16Wrapped(u32 _address)
{
	const u8 hi = readRdramByteWrapped(_address);
	const u8 lo = readRdramByteWrapped(_address + 1U);
	return static_cast<u16>((static_cast<u16>(hi) << 8U) | static_cast<u16>(lo));
}

inline u32 readRdramU32Wrapped(u32 _address)
{
	const u8 b0 = readRdramByteWrapped(_address + 0U);
	const u8 b1 = readRdramByteWrapped(_address + 1U);
	const u8 b2 = readRdramByteWrapped(_address + 2U);
	const u8 b3 = readRdramByteWrapped(_address + 3U);
	return (static_cast<u32>(b0) << 24U)
		| (static_cast<u32>(b1) << 16U)
		| (static_cast<u32>(b2) << 8U)
		| static_cast<u32>(b3);
}

inline u8 readRdramPacked4(u32 _baseAddress, u64 _texelIndex)
{
	const u32 byteAddress = _baseAddress + static_cast<u32>(_texelIndex >> 1U);
	const u8 packed = readRdramByteWrapped(byteAddress);
	const bool lowNibble = (_texelIndex & 1ULL) != 0ULL;
	return lowNibble
		? static_cast<u8>(packed & 0x0FU)
		: static_cast<u8>((packed >> 4U) & 0x0FU);
}

inline u8 readRdramTexel8(u32 _baseAddress, u64 _texelIndex)
{
	return readRdramByteWrapped(_baseAddress + static_cast<u32>(_texelIndex));
}

inline u16 readRdramTexel16(u32 _baseAddress, u64 _texelIndex)
{
	return readRdramU16Wrapped(_baseAddress + static_cast<u32>(_texelIndex << 1U));
}

inline u32 readRdramTexel32(u32 _baseAddress, u64 _texelIndex)
{
	return readRdramU32Wrapped(_baseAddress + static_cast<u32>(_texelIndex << 2U));
}

inline u32 decodeYUVSampleToPseudoRGBA(u8 _y, u8 _u, u8 _v)
{
	// Keep YUV sample unpack deterministic in RVK2 by encoding Y/V/U into RGB lanes.
	return packRgba8(_y, _v, _u, 255U);
}

inline bool sampleTextureFromRDRAM(
	const rvk2::RenderWorkPacket & _work,
	s32 _s,
	s32 _t,
	u32 & _outRgba,
	bool & _outNeedsLUT,
	u8 & _outRejectReason)
{
	_outRejectReason = 0U;
	if (!rdramReadable()) {
		_outRejectReason = 3U;
		return false;
	}

	const u8 format = (_work.tileFormat & 0x7U) <= 4U
		? (_work.tileFormat & 0x7U)
		: (_work.textureImageFormat & 0x7U);
	const u8 size = _work.tileSize & 0x3U;
	const u16 imageWidth = _work.textureImageWidth != 0U ? _work.textureImageWidth : 1U;
	const u32 baseAddress = _work.textureImageAddress & 0x00FFFFFFU;
	const u32 s = static_cast<u32>(_s & 0xFFFF);
	const u32 t = static_cast<u32>(_t & 0xFFFF);
	const u64 texelIndex = static_cast<u64>(t) * static_cast<u64>(imageWidth) + static_cast<u64>(s);
	const u8 lutMode = decodeTextureLUTMode(_work);

	_outNeedsLUT = false;
	switch (size) {
	case 0U: { // 4b
		const u8 value4 = readRdramPacked4(baseAddress, texelIndex);
		switch (format) {
		case 2U: { // CI4
			u8 index = value4;
			if (lutMode != 0U) {
				index = static_cast<u8>((_work.tilePalette << 4U) | index);
				_outNeedsLUT = true;
			}
			_outRgba = packRgba8(index, index, index, 255U);
			return true;
		}
		case 3U: { // IA3/1
			const u8 intensity = expand3To8(static_cast<u8>((value4 >> 1U) & 0x07U));
			const u8 alpha = (value4 & 0x1U) != 0U ? 255U : 0U;
			_outRgba = packRgba8(intensity, intensity, intensity, alpha);
			return true;
		}
		case 4U: { // I4
			const u8 intensity = expand4To8(value4);
			_outRgba = packRgba8(intensity, intensity, intensity, 255U);
			return true;
		}
		default:
			_outRejectReason = 1U;
			return false;
		}
	}

	case 1U: { // 8b
		const u8 value8 = readRdramTexel8(baseAddress, texelIndex);
		switch (format) {
		case 2U: { // CI8
			u8 index = value8;
			if (lutMode != 0U)
				_outNeedsLUT = true;
			_outRgba = packRgba8(index, index, index, 255U);
			return true;
		}
		case 3U: { // IA4/4
			const u8 intensity = expand4To8(static_cast<u8>((value8 >> 4U) & 0x0FU));
			const u8 alpha = expand4To8(static_cast<u8>(value8 & 0x0FU));
			_outRgba = packRgba8(intensity, intensity, intensity, alpha);
			return true;
		}
		case 4U: { // I8
			_outRgba = packRgba8(value8, value8, value8, 255U);
			return true;
		}
		default:
			_outRejectReason = 1U;
			return false;
		}
	}

	case 2U: { // 16b
		const u16 value16 = readRdramTexel16(baseAddress, texelIndex);
		switch (format) {
		case 0U: { // RGBA16
			const u8 r5 = static_cast<u8>((value16 >> 11U) & 0x1FU);
			const u8 g5 = static_cast<u8>((value16 >> 6U) & 0x1FU);
			const u8 b5 = static_cast<u8>((value16 >> 1U) & 0x1FU);
			const u8 a = (value16 & 0x1U) != 0U ? 255U : 0U;
			_outRgba = packRgba8(expand5To8(r5), expand5To8(g5), expand5To8(b5), a);
			return true;
		}
		case 1U: { // YUV16 (4:2:2 packed as U Y0 V Y1 per texel pair)
			const u64 pairBaseTexel = texelIndex & ~1ULL;
			const u32 pairAddress = baseAddress + static_cast<u32>(pairBaseTexel << 1U);
			const u8 u = readRdramByteWrapped(pairAddress + 0U);
			const u8 y0 = readRdramByteWrapped(pairAddress + 1U);
			const u8 v = readRdramByteWrapped(pairAddress + 2U);
			const u8 y1 = readRdramByteWrapped(pairAddress + 3U);
			const u8 y = (texelIndex & 1ULL) != 0ULL ? y1 : y0;
			_outRgba = decodeYUVSampleToPseudoRGBA(y, u, v);
			return true;
		}
		case 2U: { // CI16 (low 8-bit index)
			const u8 index = static_cast<u8>(value16 & 0xFFU);
			if (lutMode != 0U)
				_outNeedsLUT = true;
			_outRgba = packRgba8(index, index, index, 255U);
			return true;
		}
		case 3U: { // IA8/8
			const u8 intensity = static_cast<u8>((value16 >> 8U) & 0xFFU);
			const u8 alpha = static_cast<u8>(value16 & 0xFFU);
			_outRgba = packRgba8(intensity, intensity, intensity, alpha);
			return true;
		}
		default:
			_outRejectReason = 1U;
			return false;
		}
	}

	case 3U: { // 32b
		if (format != 0U) {
			_outRejectReason = 1U;
			return false;
		}
		const u32 value32 = readRdramTexel32(baseAddress, texelIndex);
		const u8 r = static_cast<u8>((value32 >> 24U) & 0xFFU);
		const u8 g = static_cast<u8>((value32 >> 16U) & 0xFFU);
		const u8 b = static_cast<u8>((value32 >> 8U) & 0xFFU);
		const u8 a = static_cast<u8>(value32 & 0xFFU);
		_outRgba = packRgba8(r, g, b, a);
		return true;
	}

	default:
		_outRejectReason = 2U;
		return false;
	}
}

inline u8 readTmem4BitPaletteColor(u16 _offset, u16 _x, u16 _i)
{
	const u8 * tmem8 = reinterpret_cast<const u8 *>(activeTMEMWords());
	return tmem8[((static_cast<u32>(_offset) << 3U) + (((static_cast<u32>(_x) >> 1U) ^ (static_cast<u32>(_i) << 1U)))) & 0xFFFU];
}

inline u8 readTmem8BitColor(u16 _offset, u16 _x, u16 _i)
{
	const u8 * tmem8 = reinterpret_cast<const u8 *>(activeTMEMWords());
	const u32 oddRowXor =
		debugAltTmem8OddXor()
			? static_cast<u32>(_i)
			: (static_cast<u32>(_i) << 1U);
	return tmem8[((static_cast<u32>(_offset) << 3U) + (static_cast<u32>(_x) ^ oddRowXor)) & 0xFFFU];
}

inline u16 readTmem16BitColor(u16 _offset, u16 _x, u16 _i)
{
	const u16 * tmem16 = reinterpret_cast<const u16 *>(activeTMEMWords());
	u16 value = tmem16[((static_cast<u32>(_offset) << 2U) + (static_cast<u32>(_x) ^ static_cast<u32>(_i))) & 0x7FFU];
	if (debugSwapTmem16Samples())
		value = static_cast<u16>((value << 8U) | (value >> 8U));
	return value;
}

inline u16 swapU16(u16 _value)
{
	return static_cast<u16>((_value << 8U) | (_value >> 8U));
}

inline u32 bitsPerTexelFromSize(u8 _size)
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

inline u32 xor13ForT(u16 _t)
{
	return (_t & 1U) != 0U ? 3U : 1U;
}

inline u32 xorForTmem32T(u16 _t)
{
	if (debugTmem32UseXor02())
		return (_t & 1U) != 0U ? 2U : 0U;
	return xor13ForT(_t);
}

inline s32 computeLegacySplit32LineStride(const rvk2::RenderWorkPacket & _work)
{
	const u16 lowU = std::min<u16>(_work.tileULS, _work.tileLRS);
	const u16 highU = std::max<u16>(_work.tileULS, _work.tileLRS);
	u32 tileWidth = ((static_cast<u32>(highU) - static_cast<u32>(lowU)) >> 2U) + 1U;
	if (tileWidth == 0U)
		tileWidth = 1U;

	s32 wid64 = static_cast<s32>(tileWidth) << 2;
	if ((wid64 & 15) != 0)
		wid64 += 16;
	wid64 &= ~15;
	wid64 >>= 3;
	s32 line32 = static_cast<s32>(_work.tileLine) << 1;
	line32 = (line32 - wid64) << 3;
	if (wid64 < 1)
		wid64 = 1;
	const s32 width = wid64 << 1;
	return width + (line32 >> 2);
}

inline u32 readTmem32SplitPacked(const rvk2::RenderWorkPacket & _work, u16 _s, u16 _t, s32 _lineStride, u32 _xor)
{
	const u16 * tmem16 = reinterpret_cast<const u16 *>(activeTMEMWords());
	const s32 tline =
		(static_cast<s32>(_work.tileTmem & 0x1FFU) << 2)
		+ _lineStride * static_cast<s32>(_t);
	const u32 taddr = (static_cast<u32>(tline + static_cast<s32>(_s)) ^ _xor) & 0x3FFU;
	const u16 gr = swapU16(tmem16[taddr]);
	const u16 ab = swapU16(tmem16[taddr | 0x400U]);
	return (static_cast<u32>(ab) << 16U) | static_cast<u32>(gr);
}

inline u32 packSplit32ToRGBA(u32 _packed, bool _highToLowRGBA)
{
	const u8 r = _highToLowRGBA
		? static_cast<u8>((_packed >> 24U) & 0xFFU)
		: static_cast<u8>(_packed & 0xFFU);
	const u8 g = _highToLowRGBA
		? static_cast<u8>((_packed >> 16U) & 0xFFU)
		: static_cast<u8>((_packed >> 8U) & 0xFFU);
	const u8 b = _highToLowRGBA
		? static_cast<u8>((_packed >> 8U) & 0xFFU)
		: static_cast<u8>((_packed >> 16U) & 0xFFU);
	const u8 a = _highToLowRGBA
		? static_cast<u8>(_packed & 0xFFU)
		: static_cast<u8>((_packed >> 24U) & 0xFFU);
	return packRgba8(r, g, b, a);
}

inline u32 decodeAuthoritativeTMEM32Color(
	const rvk2::RenderWorkPacket & _work,
	u16 _s,
	u16 _t)
{
	const bool highToLowRGBA = debugTmem32PackHighToLowRGBA();
	if (debugTmem32UseDirectLinearFetch()) {
		const u32 * tmem32 = reinterpret_cast<const u32 *>(activeTMEMWords());
		const u16 i = static_cast<u16>((_t & 1U) << 1U);
		const u16 tmemOffset = static_cast<u16>(
			(_work.tileTmem + static_cast<u16>(_work.tileLine * _t)) & 0x1FFU);
		const u32 packed = tmem32[
			((static_cast<u32>(tmemOffset) << 1U)
				+ (static_cast<u32>(_s) ^ static_cast<u32>(i)))
			& 0x3FFU];
		return packSplit32ToRGBA(packed, highToLowRGBA);
	}

	// Authoritative 32b TMEM path follows legacy loader addressing:
	// split GR/AB words, odd/even row XOR, and line32 stride derived from tile span.
	const s32 lineStride = computeLegacySplit32LineStride(_work);
	return packSplit32ToRGBA(
		readTmem32SplitPacked(_work, _s, _t, lineStride, xorForTmem32T(_t)),
		highToLowRGBA);
}

inline void applyTileDescriptorToWork(
	rvk2::RenderWorkPacket & _work,
	u8 _tileIndex,
	u8 _format,
	u8 _size,
	u16 _line,
	u16 _tmem,
	u8 _palette,
	u8 _cmt,
	u8 _cms,
	u8 _maskt,
	u8 _masks,
	u8 _shiftt,
	u8 _shifts,
	u16 _uls,
	u16 _ult,
	u16 _lrs,
	u16 _lrt)
{
	_work.tile = _tileIndex & 0x7U;
	_work.tileFormat = _format;
	_work.tileSize = _size;
	_work.tileLine = _line;
	_work.tileTmem = _tmem;
	_work.tilePalette = _palette;
	_work.tileCmt = _cmt;
	_work.tileCms = _cms;
	_work.tileMaskt = _maskt;
	_work.tileMasks = _masks;
	_work.tileShiftt = _shiftt;
	_work.tileShifts = _shifts;
	_work.tileULS = _uls;
	_work.tileULT = _ult;
	_work.tileLRS = _lrs;
	_work.tileLRT = _lrt;
}

inline const rvk2::RenderWorkPacket & selectTexelSlotWork(
	const rvk2::RenderWorkPacket & _work,
	bool _texel1Slot,
	rvk2::RenderWorkPacket & _scratch)
{
	if (_texel1Slot && debugForceTexel1UsesTile0())
		return _work;
	if (!_texel1Slot || !_work.tile1Valid)
		return _work;
	_scratch = _work;
	applyTileDescriptorToWork(
		_scratch,
		_work.tile1Index,
		_work.tile1Format,
		_work.tile1Size,
		_work.tile1Line,
		_work.tile1Tmem,
		_work.tile1Palette,
		_work.tile1Cmt,
		_work.tile1Cms,
		_work.tile1Maskt,
		_work.tile1Masks,
		_work.tile1Shiftt,
		_work.tile1Shifts,
		_work.tile1ULS,
		_work.tile1ULT,
		_work.tile1LRS,
		_work.tile1LRT);
	return _scratch;
}

inline bool sampleCITextureFromTMEM(
	const rvk2::RenderWorkPacket & _work,
	s32 _s,
	s32 _t,
	u32 & _outRgba,
	bool & _outNeedsLUT,
	u8 & _outRejectReason)
{
	_outRejectReason = 0U;
	const u8 format = (_work.tileFormat & 0x7U) <= 4U
		? (_work.tileFormat & 0x7U)
		: (_work.textureImageFormat & 0x7U);
	const u8 size = _work.tileSize & 0x3U;
	const s32 texelS = _s;
	const s32 texelT = _t;

	const u8 lutMode = decodeTextureLUTMode(_work);
	const u32 tMemMask = lutMode == 0U ? 0x1FFU : 0xFFU;
	const u16 t = static_cast<u16>(texelT & 0xFFFF);
	const u16 s = static_cast<u16>(texelS & 0xFFFF);
	const u16 tmemOffset = static_cast<u16>(
		(_work.tileTmem + static_cast<u16>(_work.tileLine * t)) & tMemMask);
	const u16 i = static_cast<u16>((t & 1U) << 1U);

	_outNeedsLUT = false;
	switch (size) {
		case 0U: { // 4b
			const u8 packed = readTmem4BitPaletteColor(tmemOffset, s, i);
			const bool lowNibble = (s & 1U) != 0U;
			const bool selectLowNibble =
				debugSwapTmem4Nibbles() ? !lowNibble : lowNibble;
			const u8 value4 =
				selectLowNibble
					? static_cast<u8>(packed & 0x0FU)
					: static_cast<u8>((packed >> 4U) & 0x0FU);
			switch (format) {
		case 2U: { // CI4
			u8 index = value4;
			if (lutMode != 0U) {
				index = static_cast<u8>((_work.tilePalette << 4U) | index);
				_outNeedsLUT = true;
			}
			_outRgba = packRgba8(index, index, index, 255U);
			return true;
		}
		case 3U: { // IA3/1
			const u8 intensity = expand3To8(static_cast<u8>((value4 >> 1U) & 0x07U));
			const u8 alpha = (value4 & 0x1U) != 0U ? 255U : 0U;
			_outRgba = packRgba8(intensity, intensity, intensity, alpha);
			return true;
		}
		case 4U: { // I4
			const u8 intensity = expand4To8(value4);
			_outRgba = packRgba8(intensity, intensity, intensity, 255U);
			return true;
		}
		default:
			_outRejectReason = 1U;
			return false;
		}
	}

	case 1U: { // 8b
		const u8 value8 = readTmem8BitColor(tmemOffset, s, i);
		switch (format) {
		case 2U: { // CI8
			u8 index = value8;
			if (lutMode != 0U)
				_outNeedsLUT = true;
			_outRgba = packRgba8(index, index, index, 255U);
			return true;
		}
		case 3U: { // IA4/4
			const u8 intensity = expand4To8(static_cast<u8>((value8 >> 4U) & 0x0FU));
			const u8 alpha = expand4To8(static_cast<u8>(value8 & 0x0FU));
			_outRgba = packRgba8(intensity, intensity, intensity, alpha);
			return true;
		}
		case 4U: { // I8
			_outRgba = packRgba8(value8, value8, value8, 255U);
			return true;
		}
		default:
			_outRejectReason = 1U;
			return false;
		}
	}

	case 2U: { // 16b
		const u16 value16 = readTmem16BitColor(tmemOffset, s, i);
		switch (format) {
		case 0U: { // RGBA16
			const u8 r5 = static_cast<u8>((value16 >> 11U) & 0x1FU);
			const u8 g5 = static_cast<u8>((value16 >> 6U) & 0x1FU);
			const u8 b5 = static_cast<u8>((value16 >> 1U) & 0x1FU);
			const u8 a = (value16 & 0x1U) != 0U ? 255U : 0U;
			_outRgba = packRgba8(expand5To8(r5), expand5To8(g5), expand5To8(b5), a);
			return true;
		}
		case 2U: { // CI16 (low 8-bit index)
			u8 index = static_cast<u8>(value16 & 0xFFU);
			if (lutMode != 0U)
				_outNeedsLUT = true;
			_outRgba = packRgba8(index, index, index, 255U);
			return true;
		}
		case 3U: { // IA8/8
			const u8 intensity = static_cast<u8>((value16 >> 8U) & 0xFFU);
			const u8 alpha = static_cast<u8>(value16 & 0xFFU);
			_outRgba = packRgba8(intensity, intensity, intensity, alpha);
			return true;
		}
		default:
			_outRejectReason = 1U;
			return false;
		}
	}

	case 3U: { // 32b
		if (format != 0U) {
			_outRejectReason = 1U;
			return false;
		}
		_outRgba = decodeAuthoritativeTMEM32Color(_work, s, t);
		return true;
	}

	default:
		_outRejectReason = 2U;
		return false;
	}
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

enum : u32
{
	kTexelSourceReplacementBit = 1U << 0U,
	kTexelSourceTMEMBit = 1U << 1U,
	kTexelSourceRdramBit = 1U << 2U,
	kTexelSourceSyntheticBit = 1U << 3U,
};

inline u8 effectiveTextureFormat(const rvk2::RenderWorkPacket & _work)
{
	const u8 tileFormat = static_cast<u8>(_work.tileFormat & 0x7U);
	if (tileFormat <= 4U)
		return tileFormat;
	return static_cast<u8>(_work.textureImageFormat & 0x7U);
}

inline u8 effectiveTextureSize(const rvk2::RenderWorkPacket & _work)
{
	return static_cast<u8>(_work.tileSize & 0x3U);
}

inline bool debugTextureBucketMaskAllowsSample(
	const rvk2::RenderWorkPacket & _work,
	bool _needsLUT)
{
	const DebugTextureBucketMaskConfig config = debugTextureBucketMaskConfig();
	if (!config.enabled)
		return true;

	const u8 format = effectiveTextureFormat(_work);
	const u8 size = effectiveTextureSize(_work);
	bool allowed = false;
	if (format < rvk2::kExecutorTextureFormatBuckets
		&& size < rvk2::kExecutorTextureSizeBuckets) {
		const u32 index =
			static_cast<u32>(format) * rvk2::kExecutorTextureSizeBuckets
			+ static_cast<u32>(size);
		const u8 mode = config.mode[index];
		if (mode == kTextureBucketMaskAny)
			allowed = true;
		else if (mode == kTextureBucketMaskLUTOnly)
			allowed = _needsLUT;
		else if (mode == kTextureBucketMaskNoLUT)
			allowed = !_needsLUT;
	}

	if (gActiveExecutorSummary != nullptr) {
		if (allowed)
			++gActiveExecutorSummary->textureBucketMaskAllowCount;
		else
			++gActiveExecutorSummary->textureBucketMaskRejectCount;
	}

	return allowed;
}

inline void recordTextureSampleMode(
	const rvk2::RenderWorkPacket & _work,
	bool _needsLUT,
	u8 _sampleSlot)
{
	if (gActiveExecutorSummary == nullptr)
		return;

	const u8 filterMode = static_cast<u8>(decodeTextureFilterMode(_work) & 0x3U);
	const u8 lutMode = static_cast<u8>(decodeTextureLUTMode(_work) & 0x3U);
	++gActiveExecutorSummary->textureFilterModeSampleCount[filterMode];
	++gActiveExecutorSummary->textureLUTModeSampleCount[lutMode];

	const u8 format = effectiveTextureFormat(_work);
	const u8 size = effectiveTextureSize(_work);
	if (format < rvk2::kExecutorTextureFormatBuckets)
		++gActiveExecutorSummary->textureFormatSampleCount[format];
	if (size < rvk2::kExecutorTextureSizeBuckets)
		++gActiveExecutorSummary->textureSizeSampleCount[size];
	if (format < rvk2::kExecutorTextureFormatBuckets
		&& size < rvk2::kExecutorTextureSizeBuckets) {
		const u32 index =
			static_cast<u32>(format) * rvk2::kExecutorTextureSizeBuckets
			+ static_cast<u32>(size);
		++gActiveExecutorSummary->textureFormatSizeSampleCount[index];
		if (_needsLUT)
			++gActiveExecutorSummary->textureFormatSizeLUTSampleCount[index];
		if (_sampleSlot < rvk2::kExecutorTextureSampleSlotBuckets) {
			++gActiveExecutorSummary->textureFormatSizeSampleCountBySlot[_sampleSlot][index];
			if (_needsLUT)
				++gActiveExecutorSummary->textureFormatSizeLUTSampleCountBySlot[_sampleSlot][index];
		}
	}
}

inline u32 samplePseudoTexelColor(
	const rvk2::RenderWorkPacket & _work,
	s32 _s,
	s32 _t,
	s32 _w,
	bool _includeW,
	u32 _x,
	u32 _y,
	u32 * _sourceBits = nullptr,
	u8 _sampleSlot = rvk2::kExecutorTextureSampleSlotTexel0)
{
	if (gActiveExecutorSummary != nullptr)
		++gActiveExecutorSummary->textureSampleCount;
	const bool lutModeEnabled = decodeTextureLUTMode(_work) != 0U;
	if (const rvk2::TextureReplacementImage * replacement =
			findTextureReplacementImage(_work, _s, _t, _w, _includeW)) {
		if (_sourceBits != nullptr)
			*_sourceBits |= kTexelSourceReplacementBit;
		const u32 replacementColor =
			rvk2::sampleTextureReplacementImage(*replacement, _s, _t);
		if (!debugTextureBucketMaskAllowsSample(_work, lutModeEnabled))
			return 0x000000FFU;
		return replacementColor;
	}

	u64 seed = buildTextureSeedBase(_work);
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_s)));
	mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_t)));
	if (_includeW)
		mixTextureSeed(seed, static_cast<u64>(static_cast<u32>(_w)));
	mixTextureSeed(seed, static_cast<u64>(_x));
	mixTextureSeed(seed, static_cast<u64>(_y));
	mixTextureSeed(seed, static_cast<u64>(_work.syncEpoch));

	u32 sampledColor = 0U;
	bool needsLUT = false;
	u8 tmemReject = 0U;

	if (gActiveExecutorSummary != nullptr)
		++gActiveExecutorSummary->textureTmemAttemptCount;
	if (sampleCITextureFromTMEM(
			_work,
			_s,
			_t,
			sampledColor,
			needsLUT,
			tmemReject)) {
		recordTextureSampleMode(_work, needsLUT, _sampleSlot);
		if (gActiveExecutorSummary != nullptr) {
			++gActiveExecutorSummary->textureTmemSampleCount;
			if (needsLUT)
				++gActiveExecutorSummary->textureLUTSampleCount;
		}
		if (_sourceBits != nullptr)
			*_sourceBits |= kTexelSourceTMEMBit;
		if (needsLUT)
			sampledColor = applyTextureLUTModeColor(_work, seed, sampledColor);
		if (debugForceAllTexelAlphaOpaque())
			sampledColor = (sampledColor & 0xFFFFFF00U) | 0x000000FFU;
		sampledColor = applyTextureDetailModeColor(_work, seed, sampledColor);
		if (!debugTextureBucketMaskAllowsSample(_work, needsLUT))
			return 0x000000FFU;
		return sampledColor;
	}
	if (gActiveExecutorSummary != nullptr) {
		if (tmemReject == 1U)
			++gActiveExecutorSummary->textureTmemRejectFormatCount;
		else if (tmemReject == 2U)
			++gActiveExecutorSummary->textureTmemRejectSizeCount;
		else if (tmemReject == 3U)
			++gActiveExecutorSummary->textureTmemRejectCoordCount;
	}
	needsLUT = false;
	u8 rdramReject = 0U;
	if (sampleTextureFromRDRAM(
			_work,
			_s,
			_t,
			sampledColor,
			needsLUT,
			rdramReject)) {
		(void)rdramReject;
		recordTextureSampleMode(_work, needsLUT, _sampleSlot);
		if (gActiveExecutorSummary != nullptr)
			++gActiveExecutorSummary->textureRdramSampleCount;
		if (_sourceBits != nullptr)
			*_sourceBits |= kTexelSourceRdramBit;
		if (needsLUT) {
			if (gActiveExecutorSummary != nullptr)
				++gActiveExecutorSummary->textureLUTSampleCount;
			sampledColor = applyTextureLUTModeColor(_work, seed, sampledColor);
		}
		if (debugForceAllTexelAlphaOpaque())
			sampledColor = (sampledColor & 0xFFFFFF00U) | 0x000000FFU;
		sampledColor = applyTextureDetailModeColor(_work, seed, sampledColor);
		if (!debugTextureBucketMaskAllowsSample(_work, needsLUT))
			return 0x000000FFU;
		return sampledColor;
	}
	// Keep TMEM as the primary path. If TMEM decode rejects, attempt direct
	// RDRAM decode as a best-effort fallback before synthetic diagnostics.
	if (gActiveExecutorSummary != nullptr)
		++gActiveExecutorSummary->textureSyntheticSampleCount;
	if (_sourceBits != nullptr)
		*_sourceBits |= kTexelSourceSyntheticBit;

	// Keep synthetic fallback spatially coherent so missing-texture regions
	// present as a stable debug pattern instead of random noise.
	const u32 sPattern = static_cast<u32>(absS64(static_cast<s64>(_s)));
	const u32 tPattern = static_cast<u32>(absS64(static_cast<s64>(_t)));
	const bool checker = (((sPattern >> 2U) ^ (tPattern >> 2U)) & 0x1U) != 0U;
	const u32 coordHash =
		(sPattern * 0x9E3779B1U)
		^ (tPattern * 0x85EBCA6BU)
		^ ((sPattern >> 11U) | (tPattern << 7U));
	const u8 coordR = static_cast<u8>((coordHash >> 0U) & 0xFFU);
	const u8 coordG = static_cast<u8>((coordHash >> 8U) & 0xFFU);
	const u8 coordB = static_cast<u8>((coordHash >> 16U) & 0xFFU);
	const u64 fallbackSeed = buildTextureSeedBase(_work);
	const u8 stateR = static_cast<u8>((fallbackSeed >> 8U) & 0xFFU);
	const u8 stateG = static_cast<u8>((fallbackSeed >> 24U) & 0xFFU);
	const u8 stateB = static_cast<u8>((fallbackSeed >> 40U) & 0xFFU);
	const u8 tag = static_cast<u8>(
		(((_work.tile & 0x7U) << 5U)
			| ((_work.tileSize & 0x3U) << 3U)
			| (_work.tileFormat & 0x7U))
		& 0xFFU);
	const u8 r = static_cast<u8>((checker ? 0xB0U : 0x30U) ^ (stateR & 0x30U) ^ (coordR & 0x3FU));
	const u8 g = static_cast<u8>((checker ? 0x50U : 0xD0U) ^ (stateG & 0x30U) ^ (coordG & 0x3FU) ^ tag);
	const u8 b = static_cast<u8>((checker ? 0x30U : 0x90U) ^ (stateB & 0x30U) ^ (coordB & 0x3FU) ^ static_cast<u8>(tag << 1U));
	const u8 a = 255U;
	u32 rgba = (static_cast<u32>(r) << 24)
		| (static_cast<u32>(g) << 16)
		| (static_cast<u32>(b) << 8)
		| static_cast<u32>(a);
	recordTextureSampleMode(_work, lutModeEnabled, _sampleSlot);
	if (lutModeEnabled) {
		if (gActiveExecutorSummary != nullptr)
			++gActiveExecutorSummary->textureLUTSampleCount;
		rgba = applyTextureLUTModeColor(_work, seed, rgba);
	}
	if (debugForceAllTexelAlphaOpaque())
		rgba = (rgba & 0xFFFFFF00U) | 0x000000FFU;
	rgba = applyTextureDetailModeColor(_work, seed, rgba);
	if (!debugTextureBucketMaskAllowsSample(_work, lutModeEnabled))
		return 0x000000FFU;
	return rgba;
}

inline u32 pseudoTexel(
	const rvk2::RenderWorkPacket & _work,
	u32 _x,
	u32 _y,
	u32 * _sourceBits = nullptr,
	u8 _sampleSlot = rvk2::kExecutorTextureSampleSlotTexel0)
{
	const s32 dx = static_cast<s32>(_x) - static_cast<s32>(_work.rectULX);
	const s32 dy = static_cast<s32>(_y) - static_cast<s32>(_work.rectULY);
	const s32 sRaw = _work.texRectFlip
		? static_cast<s32>(_work.texS) + ((dy * static_cast<s32>(_work.texDSDX)) >> 5)
		: static_cast<s32>(_work.texS) + ((dx * static_cast<s32>(_work.texDSDX)) >> 5);
	const s32 tRaw = _work.texRectFlip
		? static_cast<s32>(_work.texT) + ((dx * static_cast<s32>(_work.texDTDY)) >> 5)
		: static_cast<s32>(_work.texT) + ((dy * static_cast<s32>(_work.texDTDY)) >> 5);
	s32 sCoordRaw = sRaw;
	s32 tCoordRaw = tRaw;
	s32 wCoordRaw = 0;
	applyTextureCoordinateModes(_work, sCoordRaw, tCoordRaw, wCoordRaw, false);
	const bool clampAllowed =
		_work.phase != static_cast<u8>(rvk2::RenderPhase::kCopy);
	const TileAxisSampleCoord sCoord = applyTileAxisTransform(
		sCoordRaw,
		_work.tileShifts,
		_work.tileMasks,
		_work.tileCms,
		_work.tileULS,
		_work.tileLRS,
		clampAllowed);
	const TileAxisSampleCoord tCoord = applyTileAxisTransform(
		tCoordRaw,
		_work.tileShiftt,
		_work.tileMaskt,
		_work.tileCmt,
		_work.tileULT,
		_work.tileLRT,
		clampAllowed);
	const u8 filterMode = decodeTextureFilterMode(_work);
	if (filterMode == 0U)
		return samplePseudoTexelColor(
			_work,
			sCoord.texel,
			tCoord.texel,
			wCoordRaw,
			false,
			_x,
			_y,
			_sourceBits,
			_sampleSlot);

	const s32 sNextRaw = sCoordRaw + 32;
	const s32 tNextRaw = tCoordRaw + 32;
	const TileAxisSampleCoord sNextCoord = applyTileAxisTransform(
		sNextRaw,
		_work.tileShifts,
		_work.tileMasks,
		_work.tileCms,
		_work.tileULS,
		_work.tileLRS,
		clampAllowed);
	const TileAxisSampleCoord tNextCoord = applyTileAxisTransform(
		tNextRaw,
		_work.tileShiftt,
		_work.tileMaskt,
		_work.tileCmt,
		_work.tileULT,
		_work.tileLRT,
		clampAllowed);
	const u32 fracS = static_cast<u32>(sCoord.frac);
	const u32 fracT = static_cast<u32>(tCoord.frac);
	const u32 c00 = samplePseudoTexelColor(_work, sCoord.texel, tCoord.texel, wCoordRaw, false, _x, _y, _sourceBits, _sampleSlot);
	const u32 c10 = samplePseudoTexelColor(_work, sNextCoord.texel, tCoord.texel, wCoordRaw, false, _x, _y, _sourceBits, _sampleSlot);
	const u32 c01 = samplePseudoTexelColor(_work, sCoord.texel, tNextCoord.texel, wCoordRaw, false, _x, _y, _sourceBits, _sampleSlot);
	const u32 c11 = samplePseudoTexelColor(_work, sNextCoord.texel, tNextCoord.texel, wCoordRaw, false, _x, _y, _sourceBits, _sampleSlot);
	return applyTextureFilterMode(filterMode, c00, c10, c01, c11, fracS, fracT);
}

inline u32 pseudoTexelForSlot(
	const rvk2::RenderWorkPacket & _work,
	u32 _x,
	u32 _y,
	bool _texel1Slot,
	u32 * _sourceBits = nullptr,
	u8 _sampleSlot = rvk2::kExecutorTextureSampleSlotTexel0)
{
	rvk2::RenderWorkPacket sampleWork{};
	const rvk2::RenderWorkPacket & slotWork =
		selectTexelSlotWork(_work, _texel1Slot, sampleWork);
	return pseudoTexel(slotWork, _x, _y, _sourceBits, _sampleSlot);
}

struct ColorSurface {
	u8 format = 0U;
	u8 size = 0U;
	u16 width = 0U;
	u16 height = 0U;
	std::vector<u32> pixels;
	std::vector<u8> coverage;
	std::vector<u8> hiddenCoverage;
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

constexpr size_t kExecutorSurfaceHistoryLimit = 6U;
constexpr u64 kExecutorVIHistorySelectionMaxAge = 2ULL;

bool expectedSurfaceSizeFromVIStatus(const rvk2::ExecutorConfig & _config, u8 & _outSurfaceSize)
{
	if (!_config.viRegistersValid)
		return false;
	const u8 viType = static_cast<u8>(_config.viStatus & 0x3U);
	if (viType == 2U) {
		_outSurfaceSize = 2U;
		return true;
	}
	if (viType == 3U) {
		_outSurfaceSize = 3U;
		return true;
	}
	return false;
}

template <typename SurfaceT>
bool originMatchesSurfaceRange(
	u32 _surfaceAddress,
	const SurfaceT & _surface,
	u32 _viOriginAddress,
	bool & _outExactMatch)
{
	_outExactMatch = false;
	if (_viOriginAddress == _surfaceAddress) {
		_outExactMatch = true;
		return true;
	}
	const u64 pixelCount = static_cast<u64>(_surface.width) * static_cast<u64>(_surface.height);
	const u64 bitCount = pixelCount * static_cast<u64>(bitsPerPixelFromSurfaceSize(_surface.size));
	const u64 byteCount = (bitCount + 7ULL) >> 3U;
	const u64 surfaceBegin = static_cast<u64>(_surfaceAddress);
	const u64 surfaceEnd = surfaceBegin + byteCount;
	const u64 origin = static_cast<u64>(_viOriginAddress);
	return origin >= surfaceBegin && origin < surfaceEnd;
}

template <typename SurfaceMapT>
bool chooseSurfaceForVIOriginGeneric(
	const SurfaceMapT & _surfaces,
	u32 _viOriginAddress,
	u32 & _outSurfaceAddress,
	bool & _outExactMatch,
	bool _preferSurfaceSize,
	u8 _preferredSurfaceSize,
	bool _preferSurfaceWidth,
	u16 _preferredSurfaceWidth)
{
	_outExactMatch = false;
	u32 bestAddress = 0U;
	u32 bestDelta = std::numeric_limits<u32>::max();
	bool bestSizeMatch = false;
	bool bestWidthMatch = false;
	bool found = false;
	std::vector<u32> surfaceAddresses;
	surfaceAddresses.reserve(_surfaces.size());
	for (const auto & entry : _surfaces)
		surfaceAddresses.push_back(entry.first);
	std::sort(surfaceAddresses.begin(), surfaceAddresses.end());
	for (u32 surfaceAddress : surfaceAddresses) {
		const auto it = _surfaces.find(surfaceAddress);
		if (it == _surfaces.end())
			continue;
		const auto & surface = it->second;
		bool exactMatch = false;
		if (!originMatchesSurfaceRange(surfaceAddress, surface, _viOriginAddress, exactMatch))
			continue;
		const bool sizeMatch =
			!_preferSurfaceSize || ((surface.size & 0x3U) == (_preferredSurfaceSize & 0x3U));
		const bool widthMatch =
			!_preferSurfaceWidth || surface.width == _preferredSurfaceWidth;
		const u32 delta = _viOriginAddress - surfaceAddress;
		if (!found
			|| (sizeMatch && !bestSizeMatch)
			|| (sizeMatch == bestSizeMatch && widthMatch && !bestWidthMatch)
			|| (sizeMatch == bestSizeMatch && widthMatch == bestWidthMatch && delta < bestDelta)
			|| (sizeMatch == bestSizeMatch && widthMatch == bestWidthMatch && delta == bestDelta && surfaceAddress < bestAddress)) {
			found = true;
			bestDelta = delta;
			bestAddress = surfaceAddress;
			bestSizeMatch = sizeMatch;
			bestWidthMatch = widthMatch;
			_outExactMatch = exactMatch;
		}
	}
	if (!found)
		return false;
	_outSurfaceAddress = bestAddress;
	return true;
}

template <typename SurfaceMapT>
bool chooseNearestSurfaceForVIOriginGeneric(
	const SurfaceMapT & _surfaces,
	u32 _viOriginAddress,
	u32 & _outSurfaceAddress,
	bool & _outExactMatch,
	bool _preferSurfaceSize,
	u8 _preferredSurfaceSize,
	bool _preferSurfaceWidth,
	u16 _preferredSurfaceWidth)
{
	_outExactMatch = false;
	u32 bestAddress = 0U;
	u64 bestDistance = std::numeric_limits<u64>::max();
	bool bestSizeMatch = false;
	bool bestWidthMatch = false;
	bool bestAhead = true;
	bool found = false;
	std::vector<u32> surfaceAddresses;
	surfaceAddresses.reserve(_surfaces.size());
	for (const auto & entry : _surfaces)
		surfaceAddresses.push_back(entry.first);
	std::sort(surfaceAddresses.begin(), surfaceAddresses.end());
	for (u32 surfaceAddress : surfaceAddresses) {
		const auto it = _surfaces.find(surfaceAddress);
		if (it == _surfaces.end())
			continue;
		const auto & surface = it->second;
		const bool sizeMatch =
			!_preferSurfaceSize || ((surface.size & 0x3U) == (_preferredSurfaceSize & 0x3U));
		const bool widthMatch =
			!_preferSurfaceWidth || surface.width == _preferredSurfaceWidth;
		const bool exactMatch = surfaceAddress == _viOriginAddress;
		const bool ahead = surfaceAddress > _viOriginAddress;
		const u64 distance = ahead
			? static_cast<u64>(surfaceAddress - _viOriginAddress)
			: static_cast<u64>(_viOriginAddress - surfaceAddress);
		if (!found
			|| (sizeMatch && !bestSizeMatch)
			|| (sizeMatch == bestSizeMatch && widthMatch && !bestWidthMatch)
			|| (sizeMatch == bestSizeMatch && widthMatch == bestWidthMatch && !ahead && bestAhead)
			|| (sizeMatch == bestSizeMatch && widthMatch == bestWidthMatch && ahead == bestAhead && distance < bestDistance)
			|| (sizeMatch == bestSizeMatch && widthMatch == bestWidthMatch && ahead == bestAhead && distance == bestDistance && surfaceAddress < bestAddress)) {
			found = true;
			bestAddress = surfaceAddress;
			bestDistance = distance;
			bestSizeMatch = sizeMatch;
			bestWidthMatch = widthMatch;
			bestAhead = ahead;
			_outExactMatch = exactMatch;
		}
	}
	if (!found)
		return false;
	_outSurfaceAddress = bestAddress;
	return true;
}

bool chooseSurfaceForVIOrigin(
	const std::unordered_map<u32, ColorSurface> & _surfaces,
	u32 _viOriginAddress,
	u32 & _outSurfaceAddress,
	bool & _outExactMatch,
	bool _preferSurfaceSize = false,
	u8 _preferredSurfaceSize = 0U,
	bool _preferSurfaceWidth = false,
	u16 _preferredSurfaceWidth = 0U)
{
	return chooseSurfaceForVIOriginGeneric(
		_surfaces,
		_viOriginAddress,
		_outSurfaceAddress,
		_outExactMatch,
		_preferSurfaceSize,
		_preferredSurfaceSize,
		_preferSurfaceWidth,
		_preferredSurfaceWidth);
}

bool chooseNearestSurfaceForVIOrigin(
	const std::unordered_map<u32, ColorSurface> & _surfaces,
	u32 _viOriginAddress,
	u32 & _outSurfaceAddress,
	bool & _outExactMatch,
	bool _preferSurfaceSize = false,
	u8 _preferredSurfaceSize = 0U,
	bool _preferSurfaceWidth = false,
	u16 _preferredSurfaceWidth = 0U)
{
	return chooseNearestSurfaceForVIOriginGeneric(
		_surfaces,
		_viOriginAddress,
		_outSurfaceAddress,
		_outExactMatch,
		_preferSurfaceSize,
		_preferredSurfaceSize,
		_preferSurfaceWidth,
		_preferredSurfaceWidth);
}

bool chooseHistorySurfaceForVIOrigin(
	const std::unordered_map<u32, rvk2::ExecutorCachedSurface> & _surfaces,
	u32 _viOriginAddress,
	u32 & _outSurfaceAddress,
	bool & _outExactMatch,
	bool _preferSurfaceSize = false,
	u8 _preferredSurfaceSize = 0U,
	bool _preferSurfaceWidth = false,
	u16 _preferredSurfaceWidth = 0U)
{
	return chooseSurfaceForVIOriginGeneric(
		_surfaces,
		_viOriginAddress,
		_outSurfaceAddress,
		_outExactMatch,
		_preferSurfaceSize,
		_preferredSurfaceSize,
		_preferSurfaceWidth,
		_preferredSurfaceWidth);
}

bool chooseNearestHistorySurfaceForVIOrigin(
	const std::unordered_map<u32, rvk2::ExecutorCachedSurface> & _surfaces,
	u32 _viOriginAddress,
	u32 & _outSurfaceAddress,
	bool & _outExactMatch,
	bool _preferSurfaceSize = false,
	u8 _preferredSurfaceSize = 0U,
	bool _preferSurfaceWidth = false,
	u16 _preferredSurfaceWidth = 0U)
{
	return chooseNearestSurfaceForVIOriginGeneric(
		_surfaces,
		_viOriginAddress,
		_outSurfaceAddress,
		_outExactMatch,
		_preferSurfaceSize,
		_preferredSurfaceSize,
		_preferSurfaceWidth,
		_preferredSurfaceWidth);
}

bool chooseMostWrittenSurfaceAddress(
	const std::unordered_map<u32, ColorSurface> & _surfaces,
	const std::unordered_map<u32, u64> & _surfaceWrites,
	const std::unordered_map<u32, u64> & _surfaceWorks,
	u32 & _outSurfaceAddress)
{
	if (_surfaces.empty())
		return false;

	u32 bestAddress = 0U;
	u64 bestWrites = 0ULL;
	u64 bestWorks = 0ULL;
	bool found = false;

	for (const auto & entry : _surfaces) {
		const u32 address = entry.first;
		const auto writeIt = _surfaceWrites.find(address);
		const auto workIt = _surfaceWorks.find(address);
		const u64 writes = writeIt != _surfaceWrites.end() ? writeIt->second : 0ULL;
		const u64 works = workIt != _surfaceWorks.end() ? workIt->second : 0ULL;
		if (!found
			|| writes > bestWrites
			|| (writes == bestWrites && works > bestWorks)
			|| (writes == bestWrites && works == bestWorks && address < bestAddress)) {
			found = true;
			bestAddress = address;
			bestWrites = writes;
			bestWorks = works;
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
	std::vector<u8> resizedCoverage(
		static_cast<size_t>(targetWidth) * static_cast<size_t>(targetHeight),
		0U);
	std::vector<u8> resizedHiddenCoverage(
		static_cast<size_t>(targetWidth) * static_cast<size_t>(targetHeight),
		0U);
	for (u16 y = 0U; y < _surface.height; ++y) {
		for (u16 x = 0U; x < _surface.width; ++x) {
			resized[pixelIndex(targetWidth, x, y)] = _surface.pixels[pixelIndex(_surface.width, x, y)];
			if (!_surface.coverage.empty())
				resizedCoverage[pixelIndex(targetWidth, x, y)] =
					_surface.coverage[pixelIndex(_surface.width, x, y)];
			if (!_surface.hiddenCoverage.empty())
				resizedHiddenCoverage[pixelIndex(targetWidth, x, y)] =
					_surface.hiddenCoverage[pixelIndex(_surface.width, x, y)];
		}
	}
	_surface.width = targetWidth;
	_surface.height = targetHeight;
	_surface.pixels.swap(resized);
	_surface.coverage.swap(resizedCoverage);
	_surface.hiddenCoverage.swap(resizedHiddenCoverage);
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
	const rvk2::RenderWorkPacket & _work,
	u16 _clipWidth,
	u16 _clipHeight,
	WriteBounds & _bounds)
{
	if (_clipWidth == 0U || _clipHeight == 0U)
		return false;
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
	const u32 maxX = static_cast<u32>(_clipWidth - 1U);
	const u32 maxY = static_cast<u32>(_clipHeight - 1U);

	const u32 writeX0 = std::max<u32>(ulx, clipX0);
	const u32 writeY0 = std::max<u32>(uly, clipY0);
	const u32 writeX1 = std::min<u32>(lrx, std::min<u32>(clipX1, maxX));
	const u32 writeY1 = std::min<u32>(lry, std::min<u32>(clipY1, maxY));
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

inline s32 signExtend14(u16 _value)
{
	const u32 raw = static_cast<u32>(_value) & 0x3FFFU;
	return static_cast<s32>((raw ^ 0x2000U) - 0x2000U);
}

inline u32 pseudoTriangleColor(
	const rvk2::RenderWorkPacket & _work,
	u32 _x,
	u32 _y)
{
	if (debugPseudoTriangleUsePrimColor())
		return _work.primColor;
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

inline u8 lumaFromRGBA(u32 _rgba)
{
	const u32 r = (_rgba >> 24U) & 0xFFU;
	const u32 g = (_rgba >> 16U) & 0xFFU;
	const u32 b = (_rgba >> 8U) & 0xFFU;
	return static_cast<u8>((r * 77U + g * 150U + b * 29U + 128U) >> 8U);
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
	s32 combined = 0;
	s32 texel0 = 0;
	s32 texel1 = 0;
	s32 primitive = 0;
	s32 shade = 0;
	s32 environment = 0;
	s32 center = 0;
	s32 scale = 0;
	s32 combinedAlpha = 0;
	s32 texel0Alpha = 0;
	s32 texel1Alpha = 0;
	s32 primitiveAlpha = 0;
	s32 shadeAlpha = 0;
	s32 environmentAlpha = 0;
	s32 lodFraction = 0;
	s32 primLodFrac = 0;
	s32 noise = 0;
	s32 k4 = 0;
	s32 k5 = 0;
	s32 one = 256;
	s32 zero = 0;
};

struct CombinerAlphaInputs
{
	s32 combined = 0;
	s32 texel0 = 0;
	s32 texel1 = 0;
	s32 primitive = 0;
	s32 shade = 0;
	s32 environment = 0;
	s32 lodFraction = 0;
	s32 primLodFrac = 0;
	s32 one = 256;
	s32 zero = 0;
};

inline s32 selectCombinerColorInputA(u8 _selector, const CombinerColorInputs & _in)
{
	switch (_selector & 0xFU) {
	case 0U: return _in.combined;
	case 1U: return _in.texel0;
	case 2U: return _in.texel1;
	case 3U: return _in.primitive;
	case 4U: return _in.shade;
	case 5U: return _in.environment;
	case 6U: return _in.one;
	case 7U: return _in.noise;
	default: return _in.zero;
	}
}

inline s32 selectCombinerColorInputB(u8 _selector, const CombinerColorInputs & _in)
{
	switch (_selector & 0xFU) {
	case 0U: return _in.combined;
	case 1U: return _in.texel0;
	case 2U: return _in.texel1;
	case 3U: return _in.primitive;
	case 4U: return _in.shade;
	case 5U: return _in.environment;
	case 6U: return _in.center;
	case 7U: return _in.k4;
	default: return _in.zero;
	}
}

inline s32 selectCombinerColorInputC(u8 _selector, const CombinerColorInputs & _in)
{
	switch (_selector & 0x1FU) {
	case 0U: return _in.combined;
	case 1U: return _in.texel0;
	case 2U: return _in.texel1;
	case 3U: return _in.primitive;
	case 4U: return _in.shade;
	case 5U: return _in.environment;
	case 6U: return _in.scale;
	case 7U: return _in.combinedAlpha;
	case 8U: return _in.texel0Alpha;
	case 9U: return _in.texel1Alpha;
	case 10U: return _in.primitiveAlpha;
	case 11U: return _in.shadeAlpha;
	case 12U: return _in.environmentAlpha;
	case 13U: return _in.lodFraction;
	case 14U: return _in.primLodFrac;
	case 15U: return _in.k5;
	default: return _in.zero;
	}
}

inline s32 selectCombinerColorInputD(u8 _selector, const CombinerColorInputs & _in)
{
	switch (_selector & 0x7U) {
	case 0U: return _in.combined;
	case 1U: return _in.texel0;
	case 2U: return _in.texel1;
	case 3U: return _in.primitive;
	case 4U: return _in.shade;
	case 5U: return _in.environment;
	case 6U: return _in.one;
	default: return _in.zero;
	}
}

inline s32 selectCombinerAlphaInputABorD(u8 _selector, const CombinerAlphaInputs & _in)
{
	switch (_selector & 0x7U) {
	case 0U: return _in.combined;
	case 1U: return _in.texel0;
	case 2U: return _in.texel1;
	case 3U: return _in.primitive;
	case 4U: return _in.shade;
	case 5U: return _in.environment;
	case 6U: return _in.one;
	default: return _in.zero;
	}
}

inline s32 selectCombinerAlphaInputC(u8 _selector, const CombinerAlphaInputs & _in)
{
	switch (_selector & 0x7U) {
	case 0U: return _in.lodFraction;
	case 1U: return _in.texel0;
	case 2U: return _in.texel1;
	case 3U: return _in.primitive;
	case 4U: return _in.shade;
	case 5U: return _in.environment;
	case 6U: return _in.primLodFrac;
	default: return _in.zero;
	}
}

inline u8 evalCombinerEquation(s32 _a, s32 _b, s32 _c, s32 _d)
{
	const s32 value = (((_a - _b) * _c + 128) >> 8) + _d;
	const u32 value9 = static_cast<u32>(value) & 0x1FFU;
	if (value9 < 0x100U)
		return static_cast<u8>(value9);
	if (value9 < 0x180U)
		return 0xFFU;
	return 0x00U;
}

inline u8 evalCombinerAlphaEquation(s32 _a, s32 _b, s32 _c, s32 _d)
{
	const s32 value = (((_a - _b) * _c + 128) >> 8) + _d;
	return clampU8FromS32(value);
}

inline u32 applySyntheticCombiner(
	const rvk2::RenderWorkPacket & _work,
	u32 _texel0Color,
	u32 _texel1Color,
	u32 _texel0NextColor,
	u32 _shadeColor,
	u32 _baseColor,
	u32 _dstColor,
	u32 _x,
	u32 _y,
	bool _cycle2Selectors = false,
	rvk2::ExecutorSummary * _summary = nullptr)
{
	(void)_dstColor;
	const bool useCycle2Selectors =
		_cycle2Selectors
		|| (_work.phase == static_cast<u8>(rvk2::RenderPhase::kCycle1)
			&& !debugCycle1CombinerUseCycle1Selectors());
	if (_summary != nullptr) {
		++_summary->combinerOpCount;
		if (useCycle2Selectors)
			++_summary->combinerCycle2SelectorOpCount;
	}
	const CombinerCycleSelectors selectors = decodeCombinerCycleSelectors(
		_work.combineMux,
		useCycle2Selectors);
	const ColorRGBA tex0Current = unpackRGBA(_texel0Color);
	const ColorRGBA tex1Current = unpackRGBA(_texel1Color);
	const ColorRGBA tex0Next = unpackRGBA(_texel0NextColor);
	const ColorRGBA shade = unpackRGBA(_shadeColor);
	const ColorRGBA combined = unpackRGBA(_baseColor);
	const ColorRGBA prim = unpackRGBA(_work.primColor);
	const ColorRGBA env = unpackRGBA(_work.envColor);

	u64 noiseSeed = _work.combineMux;
	noiseSeed ^= _work.sourcePacketId << 9U;
	noiseSeed ^= static_cast<u64>(_x) << 33U;
	noiseSeed ^= static_cast<u64>(_y) << 45U;
	noiseSeed ^= static_cast<u64>(_work.syncEpoch) << 17U;
	noiseSeed ^= _work.otherModes ^ _work.keyState ^ _work.convertState;
	noiseSeed *= 0xD6E8FEB86659FD93ULL;
	const s32 noiseInput = static_cast<s32>((((noiseSeed >> 8U) & 0x7ULL) << 6U) | 0x20ULL);
	const ColorRGBA noise{
		static_cast<u8>(noiseInput & 0xFF),
		static_cast<u8>(noiseInput & 0xFF),
		static_cast<u8>(noiseInput & 0xFF),
		static_cast<u8>(noiseInput & 0xFF)
	};

	const u8 colorASel = selectors.colorA;
	const u8 colorBSel = selectors.colorB;
	const u8 colorCSel = selectors.colorC;
	const u8 colorDSel = selectors.colorD;
	const u8 alphaASel = selectors.alphaA;
	const u8 alphaBSel = selectors.alphaB;
	const u8 alphaCSel = selectors.alphaC;
	const u8 alphaDSel = selectors.alphaD;
	const u8 lodFraction = _work.primColorMinLevel;
	const u8 primLodFrac = _work.primColorLodFrac;
	const u8 k4 = clampU8FromS32(static_cast<s32>(_work.convertK4));
	const u8 k5 = clampU8FromS32(static_cast<s32>(_work.convertK5));
	ColorRGBA texel0 = tex0Current;
	ColorRGBA texel1 = tex1Current;
	if (_work.phase == static_cast<u8>(rvk2::RenderPhase::kCycle1)) {
		// RH#001: in 1-cycle mode, TEX1 reads next pixel TEX0.
		texel1 = tex0Next;
	}
	if (useCycle2Selectors && _work.phase == static_cast<u8>(rvk2::RenderPhase::kCycle2)) {
		// RH#002: in cycle2 second pass, TEX0 aliases current TEX1 and
		// TEX1 aliases next pixel TEX0.
		texel0 = tex1Current;
		texel1 = tex0Next;
	}

	const auto makeColorInputs = [&](
		u8 _combined,
		u8 _tex0,
		u8 _tex1,
		u8 _primitive,
		u8 _shade,
		u8 _environment,
		u8 _center,
		u8 _scale,
		u8 _noise) -> CombinerColorInputs {
		CombinerColorInputs in{};
		in.combined = _combined;
		in.texel0 = _tex0;
		in.texel1 = _tex1;
		in.primitive = _primitive;
		in.shade = _shade;
		in.environment = _environment;
		in.center = _center;
		in.scale = _scale;
		in.combinedAlpha = combined.a;
		in.texel0Alpha = texel0.a;
		in.texel1Alpha = texel1.a;
		in.primitiveAlpha = prim.a;
		in.shadeAlpha = shade.a;
		in.environmentAlpha = env.a;
		in.lodFraction = lodFraction;
		in.primLodFrac = primLodFrac;
		in.noise = _noise;
		in.k4 = k4;
		in.k5 = k5;
		in.one = 256;
		in.zero = 0U;
		return in;
	};

	const auto makeAlphaInputs = [&]() -> CombinerAlphaInputs {
		CombinerAlphaInputs in{};
		in.combined = combined.a;
		in.texel0 = texel0.a;
		in.texel1 = texel1.a;
		in.primitive = prim.a;
		in.shade = shade.a;
		in.environment = env.a;
		in.lodFraction = lodFraction;
		in.primLodFrac = primLodFrac;
		in.one = 256;
		in.zero = 0U;
		return in;
	};
	const CombinerColorInputs colorInputsR =
		makeColorInputs(combined.r, texel0.r, texel1.r, prim.r, shade.r, env.r, _work.keyCenterR, _work.keyScaleR, noise.r);
	const CombinerColorInputs colorInputsG =
		makeColorInputs(combined.g, texel0.g, texel1.g, prim.g, shade.g, env.g, _work.keyCenterG, _work.keyScaleG, noise.g);
	const CombinerColorInputs colorInputsB =
		makeColorInputs(combined.b, texel0.b, texel1.b, prim.b, shade.b, env.b, _work.keyCenterB, _work.keyScaleB, noise.b);
	const CombinerAlphaInputs alphaInputs = makeAlphaInputs();

	const ColorRGBA out{
		evalCombinerEquation(
			selectCombinerColorInputA(colorASel, colorInputsR),
			selectCombinerColorInputB(colorBSel, colorInputsR),
			selectCombinerColorInputC(colorCSel, colorInputsR),
			selectCombinerColorInputD(colorDSel, colorInputsR)),
		evalCombinerEquation(
			selectCombinerColorInputA(colorASel, colorInputsG),
			selectCombinerColorInputB(colorBSel, colorInputsG),
			selectCombinerColorInputC(colorCSel, colorInputsG),
			selectCombinerColorInputD(colorDSel, colorInputsG)),
		evalCombinerEquation(
			selectCombinerColorInputA(colorASel, colorInputsB),
			selectCombinerColorInputB(colorBSel, colorInputsB),
			selectCombinerColorInputC(colorCSel, colorInputsB),
			selectCombinerColorInputD(colorDSel, colorInputsB)),
		evalCombinerAlphaEquation(
			selectCombinerAlphaInputABorD(alphaASel, alphaInputs),
			selectCombinerAlphaInputABorD(alphaBSel, alphaInputs),
			selectCombinerAlphaInputC(alphaCSel, alphaInputs),
			selectCombinerAlphaInputABorD(alphaDSel, alphaInputs))
	};
	return packRGBA(out);
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
	bool overflow = false;
};

inline u8 alphaToCoverage3(u8 _alpha)
{
	return static_cast<u8>(_alpha >> 5U);
}

inline u8 resolveCoverageDestination(
	u8 _input,
	u8 _destination,
	u8 _cvgDest,
	bool _imageReadEnabled,
	bool & _overflowOut)
{
	const u32 sum = static_cast<u32>(_input) + static_cast<u32>(_destination);
	_overflowOut = sum > 7U;
	switch (_cvgDest & 0x3U) {
	case 0U:
		return static_cast<u8>(std::min<u32>(7U, sum));
	case 1U:
		return static_cast<u8>(sum & 0x7U);
	case 2U:
		return 7U;
	default:
		return _imageReadEnabled ? _destination : 7U;
	}
}

inline SyntheticCoverageSample evaluateSyntheticCoverage(
	const rvk2::RenderWorkPacket & _work,
	u8 _coverageAlpha,
	u8 _dstCoverage,
	bool _imageReadEnabled,
	u32 _x,
	u32 _y)
{
	SyntheticCoverageSample sample{};
	sample.destination = static_cast<u8>(std::min<u32>(7U, static_cast<u32>(_dstCoverage & 0x7U)));
	u8 inputCoverage = 7U;
	if (_work.alphaCvgSel)
		inputCoverage = alphaToCoverage3(_coverageAlpha);
	if (_work.cvgXAlpha) {
		const u8 alphaCoverage = alphaToCoverage3(_coverageAlpha);
		inputCoverage = static_cast<u8>(
			(static_cast<u32>(inputCoverage) * static_cast<u32>(alphaCoverage) + 3U) / 7U);
	}
	sample.input = static_cast<u8>(std::min<u32>(7U, static_cast<u32>(inputCoverage)));
	sample.resolved = resolveCoverageDestination(
		sample.input,
		sample.destination,
		_work.cvgDest,
		_imageReadEnabled,
		sample.overflow);
	(void)_x;
	(void)_y;
	return sample;
}

inline u32 applySyntheticBlender(
	const rvk2::RenderWorkPacket & _work,
	u32 _srcColor,
	u32 _selector0Color,
	u32 _memoryColor,
	u8 _memoryCoverage,
	bool _memoryHiddenCoverage,
	u32 _x,
	u32 _y,
	bool _cycle2Selectors = false,
	u8 _shadeAlpha = 0xFFU,
	rvk2::ExecutorSummary * _summary = nullptr,
	u8 _shadeAlphaNext = 0xFFU)
{
	ColorRGBA src = unpackRGBA(_srcColor);
	const ColorRGBA selector0 = unpackRGBA(_selector0Color);
	const ColorRGBA memory = unpackRGBA(_memoryColor);
	const ColorRGBA blendState = unpackRGBA(_work.blendColor);
	const ColorRGBA fogState = unpackRGBA(_work.fogColor);
	const bool aaEnable = isAAEnabled(_work);
	const bool imageReadEnabled = isImageReadEnabled(_work);
	// Blender selector-bank behavior:
	// - 1-cycle pipeline uses cycle-1 blender selector fields.
	// - 2-cycle pipeline uses cycle-1 fields in pass 1 and cycle-2 fields in pass 2.
	const bool decodeCycle2Selectors =
		_cycle2Selectors && _work.phase == static_cast<u8>(rvk2::RenderPhase::kCycle2);
	const BlendMuxSelectors selectors = decodeBlendMuxSelectors(
		_work,
		decodeCycle2Selectors);
	if (_summary != nullptr) {
		++_summary->blenderOpCount;
		++_summary->blendAlphaASelectorCount[selectors.m1b & 0x3U];
		++_summary->blendAlphaBSelectorCount[selectors.m2b & 0x3U];
		++_summary->blendColorPSelectorCount[selectors.m1a & 0x3U];
		++_summary->blendColorMSelectorCount[selectors.m2a & 0x3U];
		if ((selectors.m1a & 0x3U) == 1U)
			++_summary->blenderColorPMemorySelectorCount;
		if ((selectors.m2a & 0x3U) == 1U)
			++_summary->blenderColorMMemorySelectorCount;
		if (_work.forceBlender)
			++_summary->blenderForceOpCount;
		if (aaEnable)
			++_summary->blenderAAOpCount;
	}
	const auto selectColorSource = [](
		u8 _selector,
		u8 _selector0,
		u8 _memory,
		u8 _blend,
		u8 _fog) -> u8 {
		switch (_selector & 0x3U) {
		case 0U: return _selector0;
		case 1U: return _memory;
		case 2U: return _blend;
		default: return _fog;
		}
	};
	const auto selectAlphaA = [](
		u8 _selector,
		u8 _srcA,
		u8 _fogA,
		u8 _shadeA) -> u8 {
		switch (_selector & 0x3U) {
		case 0U: return _srcA; // combiner alpha
		case 1U: return _fogA; // fog alpha
		case 2U: return _shadeA; // shade alpha
		default: return 0U;
		}
	};
	const auto selectAlphaB = [](
		u8 _selector,
		u8 _alphaA,
		u8 _memoryCoverage) -> u8 {
		switch (_selector & 0x3U) {
		case 0U: return static_cast<u8>(255U - _alphaA); // 1.0 - A
		case 1U: return _memoryCoverage; // framebuffer coverage from stored coverage + hidden bits
		case 2U: return 255U; // 1.0
		default: return 0U;   // 0.0
		}
	};
	const ColorRGBA p{
		selectColorSource(selectors.m1a, selector0.r, memory.r, blendState.r, fogState.r),
		selectColorSource(selectors.m1a, selector0.g, memory.g, blendState.g, fogState.g),
		selectColorSource(selectors.m1a, selector0.b, memory.b, blendState.b, fogState.b),
		src.a
	};
	const ColorRGBA m{
		selectColorSource(selectors.m2a, selector0.r, memory.r, blendState.r, fogState.r),
		selectColorSource(selectors.m2a, selector0.g, memory.g, blendState.g, fogState.g),
		selectColorSource(selectors.m2a, selector0.b, memory.b, blendState.b, fogState.b),
		memory.a
	};
	ColorRGBA mResolved = m;
	if (debugDisableBlendMemoryColorSource() && (selectors.m2a & 0x3U) == 1U) {
		mResolved.r = p.r;
		mResolved.g = p.g;
		mResolved.b = p.b;
	}
	const bool needsCoverageAsAlphaB = (selectors.m2b & 0x3U) == 1U;

	const bool useCoverageControls =
		!debugDisableCoverageControls() && (
		_work.colorOnCvg
		|| _work.cvgXAlpha
		|| _work.alphaCvgSel
		|| _work.cvgDest != 0U
		|| _work.blendMask != 0U
		|| needsCoverageAsAlphaB);
	SyntheticCoverageSample coverage{};
	coverage.destination = static_cast<u8>(std::min<u32>(7U, static_cast<u32>(_memoryCoverage & 0x7U)));
	coverage.resolved = coverage.destination;
	if (useCoverageControls) {
		coverage = evaluateSyntheticCoverage(
			_work,
			src.a,
			_memoryCoverage,
			imageReadEnabled,
			_x,
			_y);
	}
	const u8 inputCoverageAlpha = static_cast<u8>(
		(static_cast<u32>(coverage.input) * 255U + 3U) / 7U);
	const u8 shadeAlphaInput =
		(_cycle2Selectors
			&& _work.phase == static_cast<u8>(rvk2::RenderPhase::kCycle2)
			&& (selectors.m1b & 0x3U) == 2U)
		? _shadeAlphaNext
		: _shadeAlpha;
	u8 alphaA = selectAlphaA(
		selectors.m1b,
		src.a,
		fogState.a,
		shadeAlphaInput);
	if (_work.alphaCvgSel && (selectors.m1b & 0x3U) == 0U)
		alphaA = inputCoverageAlpha;
	const u8 memoryCoverageAlpha = [&]() -> u8 {
		const u32 coverage4 =
			std::min<u32>(
				15U,
				static_cast<u32>(coverage.destination)
					+ (_memoryHiddenCoverage ? 8U : 0U));
		return static_cast<u8>((coverage4 * 255U + 7U) / 15U);
	}();
	const u8 alphaB = selectAlphaB(selectors.m2b, alphaA, memoryCoverageAlpha);
	if (_summary != nullptr && useCoverageControls) {
		++_summary->blendCoverageEvalCount;
		if (coverage.resolved == 0U)
			++_summary->blendCoverageZeroCount;
		if (coverage.overflow)
			++_summary->blendCoverageOverflowCount;
	}
	ColorRGBA out = p;
	const bool blendEnabled = _work.forceBlender || aaEnable;
	const bool colorOnCvgInhibitColorWrite =
		_work.colorOnCvg
		&& !coverage.overflow
		&& !debugDisableColorOnCvgInhibit();
	if (_summary != nullptr && blendEnabled)
		++_summary->blenderEnabledOpCount;
	if (colorOnCvgInhibitColorWrite) {
		// color_on_cvg inhibit path writes blender M input (2B path) verbatim.
		out.r = mResolved.r;
		out.g = mResolved.g;
		out.b = mResolved.b;
	}
	else if (blendEnabled) {
		const u32 a5 = static_cast<u32>(alphaA >> 3U);
		const u32 b5 = static_cast<u32>(alphaB >> 3U);
		const bool useDivide =
			debugForceBlenderDivide()
			|| (aaEnable && !_work.forceBlender);
		if (_summary != nullptr) {
			if (useDivide)
				++_summary->blenderDivideOpCount;
			else
				++_summary->blenderNoDivideOpCount;
		}
		const auto blendChannelResolved = [&](u8 _p, u8 _m) -> u8 {
			const u32 numer =
				static_cast<u32>(_p) * a5
				+ static_cast<u32>(_m) * b5;
			if (useDivide) {
				const u32 denom = std::max<u32>(1U, a5 + b5);
				return static_cast<u8>((numer + (denom / 2U)) / denom);
			}
			return static_cast<u8>((numer + 16U) >> 5U);
		};
		out.r = blendChannelResolved(p.r, mResolved.r);
		out.g = blendChannelResolved(p.g, mResolved.g);
		out.b = blendChannelResolved(p.b, mResolved.b);
	}
	out.a = src.a;

	if (useCoverageControls && _work.alphaCvgSel)
		out.a = inputCoverageAlpha;
	else if (useCoverageControls && _work.cvgXAlpha) {
		out.a = static_cast<u8>(
			(static_cast<u32>(out.a) * static_cast<u32>(inputCoverageAlpha) + 127U) / 255U);
	}

	const bool disableDither = debugDisableBlenderDither();
	const u8 colorDitherMode = disableDither ? 0U : decodeColorDitherMode(_work);
	const u8 alphaDitherMode = disableDither ? 0U : decodeAlphaDitherMode(_work);
	if (colorDitherMode != 0U || alphaDitherMode != 0U) {
		// Under active scissoring, the Y dither index uses bits [2:1].
		const u32 ditherY = hasExplicitScissor(_work) ? (_y >> 1U) : _y;
		const s32 bayer = bayerDither4x4(_x, ditherY);
		if (colorDitherMode != 0U) {
			if (_summary != nullptr)
				++_summary->colorDitherApplyCount;
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
			if (_summary != nullptr)
				++_summary->alphaDitherApplyCount;
			out.a = applySyntheticDitherMode(
				out.a,
				alphaDitherMode,
				bayer,
				syntheticNoiseSigned8(_work, _x, _y, 3U));
		}
	}

	if (isTextureEdgeEnabled(_work) && out.a > 0U && out.a < 128U) {
		if (_summary != nullptr)
			++_summary->textureEdgeAlphaPromoteCount;
		out.a = 255U;
	}
	if (isConvertOneEnabled(_work)) {
		if (_summary != nullptr)
			++_summary->convertOneAlphaForceCount;
		out.a = 255U;
	}
	return packRGBA(out);
}

inline u32 forceOpaqueAlpha(u32 _rgba)
{
	return (_rgba & 0xFFFFFF00U) | 0x000000FFU;
}

inline u32 applySyntheticBlender(
	const rvk2::RenderWorkPacket & _work,
	u32 _srcColor,
	u32 _memoryColor,
	u8 _memoryCoverage,
	bool _memoryHiddenCoverage,
	u32 _x,
	u32 _y,
	rvk2::ExecutorSummary * _summary = nullptr)
{
	return applySyntheticBlender(
		_work,
		_srcColor,
		_srcColor,
		_memoryColor,
		_memoryCoverage,
		_memoryHiddenCoverage,
		_x,
		_y,
		false,
		static_cast<u8>(_srcColor & 0xFFU),
		_summary);
}

inline bool passesSyntheticAlphaCompare(
	const rvk2::RenderWorkPacket & _work,
	u32 _pixel,
	u32 _x,
	u32 _y,
	rvk2::ExecutorSummary * _summary = nullptr)
{
	if (_work.phase == static_cast<u8>(rvk2::RenderPhase::kCopy)
		|| _work.phase == static_cast<u8>(rvk2::RenderPhase::kFill))
		return true;

	const bool alphaCompareEnable = (_work.alphaCompare & 0x1U) != 0U;
	if (!alphaCompareEnable)
		return true;
	if (_summary != nullptr)
		++_summary->alphaCompareTestCount;

	const u8 alpha = static_cast<u8>(_pixel & 0xFFU);
	const bool ditherAlphaEnable = (_work.alphaCompare & 0x2U) != 0U;
	if (ditherAlphaEnable) {
		const u8 threshold = static_cast<u8>(
			(static_cast<u32>(_x) * 17U
				+ static_cast<u32>(_y) * 29U
				+ static_cast<u32>(_work.syncEpoch & 0xFFU)
				+ static_cast<u32>(_work.keyState & 0xFFULL))
			& 0xFFU);
		const bool pass = alpha >= threshold;
		if (!pass && _summary != nullptr)
			++_summary->alphaCompareRejectCount;
		return pass;
	}
	const u8 threshold = static_cast<u8>(_work.blendColor & 0xFFU);
	const bool pass = alpha >= threshold;
	if (!pass && _summary != nullptr)
		++_summary->alphaCompareRejectCount;
	return pass;
}

inline bool passesSyntheticCoverageWrite(
	const rvk2::RenderWorkPacket & _work,
	u32 _pixel,
	u8 _dstCoverage,
	u32 _x,
	u32 _y,
	u8 * _resolvedCoverage = nullptr,
	bool * _resolvedOverflow = nullptr,
	rvk2::ExecutorSummary * _summary = nullptr)
{
	if (_work.phase == static_cast<u8>(rvk2::RenderPhase::kCopy)
		|| _work.phase == static_cast<u8>(rvk2::RenderPhase::kFill)) {
		if (_resolvedCoverage != nullptr)
			*_resolvedCoverage = 7U;
		if (_resolvedOverflow != nullptr)
			*_resolvedOverflow = false;
		return true;
	}

	if (_summary != nullptr)
		++_summary->coverageWriteTestCount;

	const ColorRGBA src = unpackRGBA(_pixel);
	const SyntheticCoverageSample coverage =
		evaluateSyntheticCoverage(_work, src.a, _dstCoverage, isImageReadEnabled(_work), _x, _y);
	if (_resolvedCoverage != nullptr)
		*_resolvedCoverage = coverage.resolved;
	if (_resolvedOverflow != nullptr)
		*_resolvedOverflow = coverage.overflow;
	if (_summary != nullptr) {
		++_summary->coverageWriteEvalCount;
		if (coverage.resolved == 0U)
			++_summary->coverageWriteZeroCount;
		if (coverage.overflow)
			++_summary->coverageWriteOverflowCount;
	}
	const bool aaEnable = isAAEnabled(_work);
	const bool pass = aaEnable
		? (coverage.input != 0U)
		: ((coverage.input & 0x1U) != 0U);
	if (!pass && _summary != nullptr)
		++_summary->coverageWriteRejectCount;
	return pass;
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
	u32 _y,
	u32 * _sourceBits = nullptr,
	u8 _sampleSlot = rvk2::kExecutorTextureSampleSlotTexel0)
{
	const s32 dsdy = combineDYDerivative(_work.triangleTexDSDY, _work.triangleTexDSDE, _work.triangleLMajor);
	const s32 dtdy = combineDYDerivative(_work.triangleTexDTDY, _work.triangleTexDTDE, _work.triangleLMajor);
	const s32 dwdy = combineDYDerivative(_work.triangleTexDWDY, _work.triangleTexDWDE, _work.triangleLMajor);
	const s32 sRaw = evalCoefficientAtPixel(_work.triangleTexS, _work.triangleTexDSDX, dsdy, _x, _y);
	const s32 tRaw = evalCoefficientAtPixel(_work.triangleTexT, _work.triangleTexDTDX, dtdy, _x, _y);
	const s32 w = evalCoefficientAtPixel(_work.triangleTexW, _work.triangleTexDWDX, dwdy, _x, _y);
	s32 sCoordRaw = sRaw;
	s32 tCoordRaw = tRaw;
	s32 wCoordRaw = w;
	applyTextureCoordinateModes(_work, sCoordRaw, tCoordRaw, wCoordRaw, true);
	const bool clampAllowed =
		_work.phase != static_cast<u8>(rvk2::RenderPhase::kCopy);
	const TileAxisSampleCoord sCoord = applyTileAxisTransform(
		sCoordRaw,
		_work.tileShifts,
		_work.tileMasks,
		_work.tileCms,
		_work.tileULS,
		_work.tileLRS,
		clampAllowed);
	const TileAxisSampleCoord tCoord = applyTileAxisTransform(
		tCoordRaw,
		_work.tileShiftt,
		_work.tileMaskt,
		_work.tileCmt,
		_work.tileULT,
		_work.tileLRT,
		clampAllowed);
	const u8 filterMode = decodeTextureFilterMode(_work);
	if (filterMode == 0U)
		return samplePseudoTexelColor(
			_work,
			sCoord.texel,
			tCoord.texel,
			wCoordRaw,
			true,
			_x,
			_y,
			_sourceBits,
			_sampleSlot);

	const s32 sNextRaw = sCoordRaw + 32;
	const s32 tNextRaw = tCoordRaw + 32;
	const TileAxisSampleCoord sNextCoord = applyTileAxisTransform(
		sNextRaw,
		_work.tileShifts,
		_work.tileMasks,
		_work.tileCms,
		_work.tileULS,
		_work.tileLRS,
		clampAllowed);
	const TileAxisSampleCoord tNextCoord = applyTileAxisTransform(
		tNextRaw,
		_work.tileShiftt,
		_work.tileMaskt,
		_work.tileCmt,
		_work.tileULT,
		_work.tileLRT,
		clampAllowed);
	const u32 fracS = static_cast<u32>(sCoord.frac);
	const u32 fracT = static_cast<u32>(tCoord.frac);
	const u32 c00 = samplePseudoTexelColor(_work, sCoord.texel, tCoord.texel, wCoordRaw, true, _x, _y, _sourceBits, _sampleSlot);
	const u32 c10 = samplePseudoTexelColor(_work, sNextCoord.texel, tCoord.texel, wCoordRaw, true, _x, _y, _sourceBits, _sampleSlot);
	const u32 c01 = samplePseudoTexelColor(_work, sCoord.texel, tNextCoord.texel, wCoordRaw, true, _x, _y, _sourceBits, _sampleSlot);
	const u32 c11 = samplePseudoTexelColor(_work, sNextCoord.texel, tNextCoord.texel, wCoordRaw, true, _x, _y, _sourceBits, _sampleSlot);
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
	u32 _y,
	bool _texel1Slot = false,
	u32 * _sourceBits = nullptr,
	u8 _sampleSlot = rvk2::kExecutorTextureSampleSlotTexel0)
{
	rvk2::RenderWorkPacket sampleWork{};
	const rvk2::RenderWorkPacket & texelWork =
		selectTexelSlotWork(_work, _texel1Slot, sampleWork);
	const bool useTriangleTexture = texelWork.textured && texelWork.triangleTextureEnable;
	return useTriangleTexture
		? evaluateTriangleTextureColor(texelWork, _x, _y, _sourceBits, _sampleSlot)
		: (texelWork.textured ? pseudoTexel(texelWork, _x, _y, _sourceBits, _sampleSlot) : pseudoTriangleColor(texelWork, _x, _y));
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
	u32 _textureColor,
	u32 _shadeColor)
{
	const bool useTriangleShade = _work.triangleShadeEnable;
	if (!_work.textured && useTriangleShade)
		return _shadeColor;
	if (!useTriangleShade)
		return _textureColor;
	return modulateRGBA(_textureColor, _shadeColor);
}

inline u32 runSyntheticPhasePipeline(
	const rvk2::RenderWorkPacket & _work,
	u32 _texel0Color,
	u32 _texel1Color,
	u32 _texel0NextColor,
	u32 _shadeColor,
	u32 _shadeColorNext,
	u32 _baseColor,
	u32 _cycle1CombinedFeedbackColor,
	bool _hasCycle1CombinedFeedback,
	u32 _dstColor,
	u8 _dstCoverage,
	bool _dstHiddenCoverage,
	u32 _cycle2Cycle1DstColor,
	u8 _cycle2Cycle1DstCoverage,
	bool _cycle2Cycle1DstHiddenCoverage,
	u32 _x,
	u32 _y,
	u8 * _coverageDestination = nullptr,
	u32 * _combinerOutputColor = nullptr,
	u32 * _blenderOutputColor = nullptr,
	u32 * _cycle1CombinedColor = nullptr,
	rvk2::ExecutorSummary * _summary = nullptr)
{
	u32 finalColor = _baseColor;
	u32 combinerColor = _baseColor;
	u32 blenderColor = _baseColor;
	u32 cycle1CombinedColor =
		_hasCycle1CombinedFeedback ? _cycle1CombinedFeedbackColor : _baseColor;
	const u8 shadeAlpha = static_cast<u8>(_shadeColor & 0xFFU);
	const u8 shadeAlphaNext = static_cast<u8>(_shadeColorNext & 0xFFU);
	u8 coverageDestination = static_cast<u8>(std::min<u32>(7U, static_cast<u32>(_dstCoverage & 0x7U)));
	const u8 phase = _work.phase;
	if (phase == static_cast<u8>(rvk2::RenderPhase::kCopy)) {
		coverageDestination = 7U;
		finalColor = _work.textured ? _texel0Color : _baseColor;
		combinerColor = finalColor;
		blenderColor = finalColor;
		cycle1CombinedColor = finalColor;
	}
	else if (phase == static_cast<u8>(rvk2::RenderPhase::kFill)) {
		coverageDestination = 7U;
		finalColor = _baseColor;
		combinerColor = finalColor;
		blenderColor = finalColor;
		cycle1CombinedColor = finalColor;
	}
	else if (phase != static_cast<u8>(rvk2::RenderPhase::kCycle2)) {
		cycle1CombinedColor = applySyntheticCombiner(
			_work,
			_texel0Color,
			_texel1Color,
			_texel0NextColor,
			_shadeColor,
			cycle1CombinedColor,
			_dstColor,
			_x,
			_y,
			false,
			_summary);
		combinerColor = cycle1CombinedColor;
		blenderColor =
			debugBypassBlender()
				? combinerColor
				: applySyntheticBlender(
					_work,
					combinerColor,
					combinerColor,
					_dstColor,
					coverageDestination,
					_dstHiddenCoverage,
					_x,
					_y,
					false,
					shadeAlpha,
					_summary,
					shadeAlphaNext);
		finalColor = blenderColor;
	}
	else {
		cycle1CombinedColor = applySyntheticCombiner(
			_work,
			_texel0Color,
			_texel1Color,
			_texel0NextColor,
			_shadeColor,
			cycle1CombinedColor,
			_dstColor,
			_x,
			_y,
			false,
			_summary);
		const u32 cycle1Color =
			debugBypassBlender()
				? cycle1CombinedColor
				: applySyntheticBlender(
					_work,
					cycle1CombinedColor,
					cycle1CombinedColor,
					_cycle2Cycle1DstColor,
					_cycle2Cycle1DstCoverage,
					_cycle2Cycle1DstHiddenCoverage,
					_x,
					_y,
					false,
					shadeAlpha,
					_summary,
					shadeAlphaNext);
		combinerColor = applySyntheticCombiner(
			_work,
			_texel0Color,
			_texel1Color,
			_texel0NextColor,
			_shadeColor,
			cycle1CombinedColor,
			cycle1Color,
			_x,
			_y,
			true,
			_summary);
		blenderColor =
			debugBypassBlender()
				? combinerColor
				: applySyntheticBlender(
					_work,
					combinerColor,
					cycle1Color,
					debugCycle2SecondPassMemoryFromCycle1() ? cycle1Color : _dstColor,
					_dstCoverage,
					_dstHiddenCoverage,
					_x,
					_y,
					true,
					shadeAlpha,
					_summary,
					shadeAlphaNext);
		finalColor = blenderColor;
	}

	if (_coverageDestination != nullptr)
		*_coverageDestination = coverageDestination;
	if (_combinerOutputColor != nullptr)
		*_combinerOutputColor = combinerColor;
	if (_blenderOutputColor != nullptr)
		*_blenderOutputColor = blenderColor;
	if (_cycle1CombinedColor != nullptr)
		*_cycle1CombinedColor = cycle1CombinedColor;
	return finalColor;
}

inline u32 selectDebugStageRasterColor(
	DebugStageViewMode _mode,
	u32 _texelColor,
	u32 _combinerColor,
	u32 _blenderColor,
	u32 _finalColor)
{
	switch (_mode) {
	case DebugStageViewMode::kTexelRaw:
		return _texelColor;
	case DebugStageViewMode::kCombinerOut:
		return _combinerColor;
	case DebugStageViewMode::kBlenderOut:
		return _blenderColor;
	default:
		return _finalColor;
	}
}

inline bool phaseUsesDepth(u8 _phase)
{
	return _phase == static_cast<u8>(rvk2::RenderPhase::kCycle1)
		|| _phase == static_cast<u8>(rvk2::RenderPhase::kCycle2);
}

inline bool hasCoverageControls(const rvk2::RenderWorkPacket & _work)
{
	return _work.colorOnCvg
		|| _work.cvgXAlpha
		|| _work.alphaCvgSel
		|| _work.cvgDest != 0U
		|| _work.blendMask != 0U;
}

inline u32 classifyStageDeltaBucket(const rvk2::RenderWorkPacket & _work)
{
	u32 bucket = 0U;
	if (_work.phase == static_cast<u8>(rvk2::RenderPhase::kCycle2))
		bucket |= 0x1U;
	if (_work.forceBlender)
		bucket |= 0x2U;
	if ((_work.alphaCompare & 0x1U) != 0U)
		bucket |= 0x4U;
	if (hasCoverageControls(_work))
		bucket |= 0x8U;
	const bool depthActive =
		phaseUsesDepth(_work.phase)
		&& _work.depthTest
		&& _work.triangleZBufferEnable
		&& (_work.depthCompareEnable || _work.depthUpdateEnable);
	if (depthActive)
		bucket |= 0x10U;
	return bucket & (rvk2::kExecutorStageDeltaClassBuckets - 1U);
}

void writeRect(
	ColorSurface & _surface,
	const rvk2::RenderWorkPacket & _work,
	const rvk2::ExecutorConfig & _config,
	rvk2::ExecutorSummary & _summary)
{
	if (_work.opKind == static_cast<u8>(rvk2::RasterOpKind::kTexRect)
		&& debugDisableTexRectWrites()) {
		return;
	}
	const DebugStageViewMode stageViewMode = debugStageViewMode();
	const bool imageReadEnabledWork = isImageReadEnabled(_work);
	const bool cycle2Work = _work.phase == static_cast<u8>(rvk2::RenderPhase::kCycle2);
	const bool decodeCycle2BlendSelectors = cycle2Work;
	const BlendMuxSelectors stageBlendSelectors = decodeBlendMuxSelectors(_work, decodeCycle2BlendSelectors);
	WriteBounds bounds{};
	if (!computeWriteBounds(_work, _config.maxSurfaceWidth, _config.maxSurfaceHeight, bounds)) {
		return;
	}
	const u16 requiredWidth = static_cast<u16>(std::min<u32>(bounds.x1 + 1U, _config.maxSurfaceWidth));
	const u16 requiredHeight = static_cast<u16>(std::min<u32>(bounds.y1 + 1U, _config.maxSurfaceHeight));
	ensureSurfaceSize(_surface, requiredWidth, requiredHeight, _config.maxSurfaceWidth, _config.maxSurfaceHeight);

	for (u32 y = bounds.y0; y <= bounds.y1; ++y) {
		if (!passesScissorFieldFilter(_work, y))
			continue;
		bool hasPrevPixelForCycle2 = false;
		u32 prevMemoryColorForCycle2 = 0U;
		u8 prevMemoryCoverageForCycle2 = 7U;
		bool prevMemoryHiddenCoverageForCycle2 = false;
		bool hasPrevCycle1CombinedColor = false;
		u32 prevCycle1CombinedColor = 0U;
		for (u32 x = bounds.x0; x <= bounds.x1; ++x) {
			const size_t colorIdx = pixelIndex(_surface.width, static_cast<u16>(x), static_cast<u16>(y));
			const u32 dstColor = _surface.pixels[colorIdx];
			const u8 dstCoverage = !_surface.coverage.empty()
				? static_cast<u8>(_surface.coverage[colorIdx] & 0x7U)
				: 0U;
			const bool dstHiddenCoverage = !_surface.hiddenCoverage.empty()
				? (_surface.hiddenCoverage[colorIdx] & 0x1U) != 0U
				: false;
			const u32 pipelineDstColor = imageReadEnabledWork ? dstColor : 0x00000000U;
			const u8 pipelineDstCoverage = imageReadEnabledWork ? dstCoverage : 7U;
			const bool pipelineDstHiddenCoverage = imageReadEnabledWork ? dstHiddenCoverage : false;
			u8 coverageDestination = pipelineDstCoverage;
			u32 textureColor = 0U;
			u32 texel1Color = 0U;
			u32 texel0NextColor = 0U;
			u32 combinerColor = 0U;
			u32 blenderColor = 0U;
			u32 cycle1CombinedColor = 0U;
			u32 finalColor = 0U;
			u32 textureSourceBits = 0U;
			u32 cycle2Cycle1DstColor = pipelineDstColor;
			u8 cycle2Cycle1DstCoverage = pipelineDstCoverage;
			bool cycle2Cycle1DstHiddenCoverage = pipelineDstHiddenCoverage;
			if (cycle2Work
				&& hasPrevPixelForCycle2
				&& !debugDisableCycle2PrevMemoryColor()) {
				cycle2Cycle1DstColor = prevMemoryColorForCycle2;
				cycle2Cycle1DstCoverage = prevMemoryCoverageForCycle2;
				cycle2Cycle1DstHiddenCoverage = prevMemoryHiddenCoverageForCycle2;
			}
			if (_work.opKind == static_cast<u8>(rvk2::RasterOpKind::kFillRect)) {
				finalColor = decodeFillColor(_work.fillColor, _work.colorImageSize);
				textureColor = finalColor;
				texel1Color = finalColor;
				texel0NextColor = finalColor;
				combinerColor = finalColor;
				blenderColor = finalColor;
				cycle1CombinedColor = finalColor;
			}
			else {
				textureColor = pseudoTexelForSlot(
					_work,
					x,
					y,
					false,
					&textureSourceBits,
					rvk2::kExecutorTextureSampleSlotTexel0);
				texel1Color = pseudoTexelForSlot(
					_work,
					x,
					y,
					true,
					&textureSourceBits,
					rvk2::kExecutorTextureSampleSlotTexel1);
				texel0NextColor = pseudoTexelForSlot(
					_work,
					std::min<u32>(x + 1U, bounds.x1),
					y,
					false,
					&textureSourceBits,
					rvk2::kExecutorTextureSampleSlotTexel0Next);
				if (debugForceTexelAlphaOpaque() && _work.textured) {
					textureColor = forceOpaqueAlpha(textureColor);
					texel1Color = forceOpaqueAlpha(texel1Color);
					texel0NextColor = forceOpaqueAlpha(texel0NextColor);
				}
				finalColor = runSyntheticPhasePipeline(
					_work,
					textureColor,
					texel1Color,
					texel0NextColor,
					0xFFFFFFFFU,
					0xFFFFFFFFU,
					textureColor,
					prevCycle1CombinedColor,
					hasPrevCycle1CombinedColor,
					pipelineDstColor,
					pipelineDstCoverage,
					pipelineDstHiddenCoverage,
					cycle2Cycle1DstColor,
					cycle2Cycle1DstCoverage,
					cycle2Cycle1DstHiddenCoverage,
					x,
					y,
					&coverageDestination,
					&combinerColor,
					&blenderColor,
					&cycle1CombinedColor,
					&_summary);
			}
			prevMemoryColorForCycle2 = pipelineDstColor;
			prevMemoryCoverageForCycle2 = pipelineDstCoverage;
			prevMemoryHiddenCoverageForCycle2 = pipelineDstHiddenCoverage;
			hasPrevPixelForCycle2 = true;
			prevCycle1CombinedColor = cycle1CombinedColor;
			hasPrevCycle1CombinedColor = true;
			u32 alphaCompareColor = finalColor;
			if (cycle2Work
				&& (_work.alphaCompare & 0x1U) != 0U
				&& _work.opKind != static_cast<u8>(rvk2::RasterOpKind::kFillRect)
				&& x < bounds.x1) {
				const u32 nextX = x + 1U;
				const size_t nextColorIdx = pixelIndex(
					_surface.width,
					static_cast<u16>(nextX),
					static_cast<u16>(y));
				const u32 nextDstColor = _surface.pixels[nextColorIdx];
				const u32 nextPipelineDstColor = imageReadEnabledWork ? nextDstColor : 0x00000000U;
				const u32 nextTexel0Color = pseudoTexelForSlot(
					_work,
					nextX,
					y,
					false,
					nullptr,
					rvk2::kExecutorTextureSampleSlotTexel0Next);
				const u32 nextTexel1Color = pseudoTexelForSlot(
					_work,
					nextX,
					y,
					true,
					nullptr,
					rvk2::kExecutorTextureSampleSlotTexel1);
				alphaCompareColor = applySyntheticCombiner(
					_work,
					nextTexel0Color,
					nextTexel1Color,
					nextTexel0Color,
					0xFFFFFFFFU,
					cycle1CombinedColor,
					nextPipelineDstColor,
					nextX,
					y,
					false,
					nullptr);
			}
			if (!passesSyntheticAlphaCompare(_work, alphaCompareColor, x, y, &_summary))
				continue;
			u8 resolvedCoverage = coverageDestination;
			bool coverageOverflow = false;
			if (!passesSyntheticCoverageWrite(
					_work,
					finalColor,
					coverageDestination,
					x,
					y,
					&resolvedCoverage,
					&coverageOverflow,
					&_summary))
				continue;
			bool resolvedHiddenCoverage = false;
			if (_work.phase != static_cast<u8>(rvk2::RenderPhase::kCopy)
				&& _work.phase != static_cast<u8>(rvk2::RenderPhase::kFill)) {
				if (_work.cvgDest == 3U)
					resolvedHiddenCoverage = pipelineDstHiddenCoverage;
				else
					resolvedHiddenCoverage = coverageOverflow;
			}
			const u32 stageBucket = classifyStageDeltaBucket(_work);
			++_summary.stageWriteClassCount[stageBucket];
			if ((stageBlendSelectors.m1a & 0x3U) == 1U)
				++_summary.stageBlendPUsesMemoryClassCount[stageBucket];
			if ((stageBlendSelectors.m2a & 0x3U) == 1U)
				++_summary.stageBlendMUsesMemoryClassCount[stageBucket];
			if (imageReadEnabledWork) {
				++_summary.stageImageReadWriteCount;
				++_summary.stageImageReadClassCount[stageBucket];
			}
			if (_work.textured) {
				++_summary.stageTexturedWriteCount;
				++_summary.stageTexturedRectWriteCount;
			}
			if ((textureSourceBits & kTexelSourceReplacementBit) != 0U) {
				++_summary.stageTexelSourceReplacementWriteCount;
				++_summary.stageTexelSourceReplacementClassCount[stageBucket];
			}
			if ((textureSourceBits & kTexelSourceTMEMBit) != 0U) {
				++_summary.stageTexelSourceTMEMWriteCount;
				++_summary.stageTexelSourceTMEMClassCount[stageBucket];
			}
			if ((textureSourceBits & kTexelSourceRdramBit) != 0U) {
				++_summary.stageTexelSourceRdramWriteCount;
				++_summary.stageTexelSourceRdramClassCount[stageBucket];
			}
			if ((textureSourceBits & kTexelSourceSyntheticBit) != 0U) {
				++_summary.stageTexelSourceSyntheticWriteCount;
				++_summary.stageTexelSourceSyntheticClassCount[stageBucket];
			}
			if (_work.opKind == static_cast<u8>(rvk2::RasterOpKind::kFillRect))
				++_summary.writeKindFillCount;
			else
				++_summary.writeKindTexRectCount;
			if (textureColor != combinerColor)
				++_summary.stageTexelToCombinerDeltaCount;
			if (combinerColor != blenderColor) {
				++_summary.stageCombinerToBlenderDeltaCount;
				++_summary.stageCombinerToBlenderDeltaClassCount[stageBucket];
			}
			if (blenderColor != finalColor)
				++_summary.stageBlenderToFinalDeltaCount;
			if (textureColor != finalColor) {
				++_summary.stageTexelToFinalDeltaCount;
				++_summary.stageTexelToFinalDeltaClassCount[stageBucket];
			}
			const u32 writeColor = selectDebugStageRasterColor(
				stageViewMode,
				textureColor,
				combinerColor,
				blenderColor,
				finalColor);
			const u64 writeLuma = static_cast<u64>(lumaFromRGBA(writeColor));
			_surface.pixels[colorIdx] = encodeSurfaceColor(writeColor, _work.colorImageSize);
			if (!_surface.coverage.empty())
				_surface.coverage[colorIdx] = static_cast<u8>(resolvedCoverage & 0x7U);
			if (!_surface.hiddenCoverage.empty())
				_surface.hiddenCoverage[colorIdx] = resolvedHiddenCoverage ? 1U : 0U;
			_summary.outputLumaSum += writeLuma;
			if (_work.opKind == static_cast<u8>(rvk2::RasterOpKind::kTexRect)) {
				_summary.writeTexRectLumaSum += writeLuma;
				if ((writeColor & 0x00FFFFFFU) != 0U)
					++_summary.writeTexRectNonBlackCount;
			}
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
	if (debugDisableTriangleWrites())
		return;
	const DebugStageViewMode stageViewMode = debugStageViewMode();
	const bool imageReadEnabledWork = isImageReadEnabled(_work);
	const bool cycle2Work = _work.phase == static_cast<u8>(rvk2::RenderPhase::kCycle2);
	const bool decodeCycle2BlendSelectors = cycle2Work;
	const BlendMuxSelectors stageBlendSelectors = decodeBlendMuxSelectors(_work, decodeCycle2BlendSelectors);
	WriteBounds bounds{};
	if (!computeWriteBounds(_work, _config.maxSurfaceWidth, _config.maxSurfaceHeight, bounds)) {
		++_summary.triangleBoundsRejectCount;
		return;
	}
	const u16 requiredWidth = static_cast<u16>(std::min<u32>(bounds.x1 + 1U, _config.maxSurfaceWidth));
	const u16 requiredHeight = static_cast<u16>(std::min<u32>(bounds.y1 + 1U, _config.maxSurfaceHeight));
	ensureSurfaceSize(_surface, requiredWidth, requiredHeight, _config.maxSurfaceWidth, _config.maxSurfaceHeight);
	if (_depthSurface != nullptr)
		ensureDepthSurfaceSize(*_depthSurface, requiredWidth, requiredHeight, _config.maxSurfaceWidth, _config.maxSurfaceHeight);

	// Triangle Y edges are signed 14-bit s10.2 values.
	const s32 yhSigned = signExtend14(_work.triangleYH);
	const s32 ymSigned = signExtend14(_work.triangleYM);
	const s32 ylSigned = signExtend14(_work.triangleYL);
	const double yh = static_cast<double>(yhSigned) * 0.25;
	const double ym = static_cast<double>(ymSigned) * 0.25;
	const double yl = static_cast<double>(ylSigned) * 0.25;
	const double xh = static_cast<double>(_work.triangleXH) / 65536.0;
	const double xl = static_cast<double>(_work.triangleXL) / 65536.0;
	const double xLongAtYL =
		(static_cast<double>(_work.triangleXH)
			+ static_cast<double>(_work.triangleDxHDY) * static_cast<double>(ylSigned - yhSigned))
		/ 65536.0;

	const double ax = xh;
	const double ay = yh;
	const double bx = xl;
	const double by = ym;
	const double cx = xLongAtYL;
	const double cy = yl;
	const double area = edgeFunction(ax, ay, bx, by, cx, cy);
	if (area == 0.0) {
		++_summary.triangleDegenerateRejectCount;
		return;
	}

	for (u32 y = bounds.y0; y <= bounds.y1; ++y) {
		if (!passesScissorFieldFilter(_work, y)) {
			++_summary.triangleScissorFieldRejectCount;
			continue;
		}
		bool hasPrevPixelForCycle2 = false;
		u32 prevMemoryColorForCycle2 = 0U;
		u8 prevMemoryCoverageForCycle2 = 7U;
		bool prevMemoryHiddenCoverageForCycle2 = false;
		bool hasPrevCycle1CombinedColor = false;
		u32 prevCycle1CombinedColor = 0U;
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
			++_summary.triangleSampleCandidateCount;

			const size_t colorIdx = pixelIndex(_surface.width, static_cast<u16>(x), static_cast<u16>(y));
			const u32 dstColor = _surface.pixels[colorIdx];
			const u8 dstCoverage = !_surface.coverage.empty()
				? static_cast<u8>(_surface.coverage[colorIdx] & 0x7U)
				: 0U;
			const bool dstHiddenCoverage = !_surface.hiddenCoverage.empty()
				? (_surface.hiddenCoverage[colorIdx] & 0x1U) != 0U
				: false;
			const u32 pipelineDstColor = imageReadEnabledWork ? dstColor : 0x00000000U;
			const u8 pipelineDstCoverage = imageReadEnabledWork ? dstCoverage : 7U;
			const bool pipelineDstHiddenCoverage = imageReadEnabledWork ? dstHiddenCoverage : false;
			u8 coverageDestination = pipelineDstCoverage;
			u32 textureSourceBits = 0U;
			const u32 textureColor = chooseTriangleTextureSourceColor(
				_work,
				x,
				y,
				false,
				&textureSourceBits,
				rvk2::kExecutorTextureSampleSlotTexel0);
			const u32 texel1Color = chooseTriangleTextureSourceColor(
				_work,
				x,
				y,
				true,
				&textureSourceBits,
				rvk2::kExecutorTextureSampleSlotTexel1);
			const u32 texel0NextColor = chooseTriangleTextureSourceColor(
				_work,
				std::min<u32>(x + 1U, bounds.x1),
				y,
				false,
				&textureSourceBits,
				rvk2::kExecutorTextureSampleSlotTexel0Next);
			const u32 shadeColor = chooseTriangleShadeSourceColor(_work, x, y);
			const u32 shadeColorNext = chooseTriangleShadeSourceColor(
				_work,
				std::min<u32>(x + 1U, bounds.x1),
				y);
			const u32 baseColor = chooseTriangleBaseColor(_work, textureColor, shadeColor);
			u32 combinerColor = 0U;
			u32 blenderColor = 0U;
			u32 cycle1CombinedColor = 0U;
			u32 cycle2Cycle1DstColor = pipelineDstColor;
			u8 cycle2Cycle1DstCoverage = pipelineDstCoverage;
			bool cycle2Cycle1DstHiddenCoverage = pipelineDstHiddenCoverage;
			if (cycle2Work
				&& hasPrevPixelForCycle2
				&& !debugDisableCycle2PrevMemoryColor()) {
				cycle2Cycle1DstColor = prevMemoryColorForCycle2;
				cycle2Cycle1DstCoverage = prevMemoryCoverageForCycle2;
				cycle2Cycle1DstHiddenCoverage = prevMemoryHiddenCoverageForCycle2;
			}
			const u32 finalColor = runSyntheticPhasePipeline(
				_work,
				textureColor,
				texel1Color,
				texel0NextColor,
				shadeColor,
				shadeColorNext,
				baseColor,
				prevCycle1CombinedColor,
				hasPrevCycle1CombinedColor,
				pipelineDstColor,
				pipelineDstCoverage,
				pipelineDstHiddenCoverage,
				cycle2Cycle1DstColor,
				cycle2Cycle1DstCoverage,
				cycle2Cycle1DstHiddenCoverage,
				x,
				y,
				&coverageDestination,
				&combinerColor,
				&blenderColor,
				&cycle1CombinedColor,
				&_summary);
			prevMemoryColorForCycle2 = pipelineDstColor;
			prevMemoryCoverageForCycle2 = pipelineDstCoverage;
			prevMemoryHiddenCoverageForCycle2 = pipelineDstHiddenCoverage;
			hasPrevPixelForCycle2 = true;
			prevCycle1CombinedColor = cycle1CombinedColor;
			hasPrevCycle1CombinedColor = true;
			u32 alphaCompareColor = finalColor;
			if (cycle2Work
				&& (_work.alphaCompare & 0x1U) != 0U
				&& x < bounds.x1) {
				const u32 nextX = x + 1U;
				const size_t nextColorIdx = pixelIndex(
					_surface.width,
					static_cast<u16>(nextX),
					static_cast<u16>(y));
				const u32 nextDstColor = _surface.pixels[nextColorIdx];
				const u32 nextPipelineDstColor = imageReadEnabledWork ? nextDstColor : 0x00000000U;
				const u32 nextTexel0Color = chooseTriangleTextureSourceColor(
					_work,
					nextX,
					y,
					false,
					nullptr,
					rvk2::kExecutorTextureSampleSlotTexel0Next);
				const u32 nextTexel1Color = chooseTriangleTextureSourceColor(
					_work,
					nextX,
					y,
					true,
					nullptr,
					rvk2::kExecutorTextureSampleSlotTexel1);
				const u32 nextShadeColor = chooseTriangleShadeSourceColor(_work, nextX, y);
				alphaCompareColor = applySyntheticCombiner(
					_work,
					nextTexel0Color,
					nextTexel1Color,
					nextTexel0Color,
					nextShadeColor,
					cycle1CombinedColor,
					nextPipelineDstColor,
					nextX,
					y,
					false,
					nullptr);
			}
			if (!passesSyntheticAlphaCompare(_work, alphaCompareColor, x, y, &_summary)) {
				++_summary.triangleAlphaRejectCount;
				continue;
			}
			u8 resolvedCoverage = coverageDestination;
			bool coverageOverflow = false;
			if (!passesSyntheticCoverageWrite(
					_work,
					finalColor,
					coverageDestination,
					x,
					y,
					&resolvedCoverage,
					&coverageOverflow,
					&_summary)) {
				++_summary.triangleCoverageRejectCount;
				continue;
			}
			bool resolvedHiddenCoverage = false;
			if (_work.phase != static_cast<u8>(rvk2::RenderPhase::kCopy)
				&& _work.phase != static_cast<u8>(rvk2::RenderPhase::kFill)) {
				if (_work.cvgDest == 3U)
					resolvedHiddenCoverage = pipelineDstHiddenCoverage;
				else
					resolvedHiddenCoverage = coverageOverflow;
			}

			if (_depthSurface != nullptr
				&& phaseUsesDepth(_work.phase)
				&& _work.depthTest
				&& _work.triangleZBufferEnable
				&& (_work.depthCompareEnable || _work.depthUpdateEnable)) {
				++_summary.depthEvalCount;
				const s32 z = evaluateTriangleDepth(_work, x, y);
				const size_t depthIdx = pixelIndex(_depthSurface->width, static_cast<u16>(x), static_cast<u16>(y));
				if (depthIdx >= _depthSurface->values.size())
					continue;
				const s32 depthValue = _depthSurface->values[depthIdx];
				if (!passesSyntheticDepthCompare(_work, z, depthValue)) {
					++_summary.depthRejectCount;
					++_summary.triangleDepthRejectCount;
					continue;
				}
				if (shouldUpdateSyntheticDepth(_work, z, depthValue)) {
					_depthSurface->values[depthIdx] = z;
					++_summary.depthUpdateCount;
				}
			}
			const u32 stageBucket = classifyStageDeltaBucket(_work);
			++_summary.stageWriteClassCount[stageBucket];
			if ((stageBlendSelectors.m1a & 0x3U) == 1U)
				++_summary.stageBlendPUsesMemoryClassCount[stageBucket];
			if ((stageBlendSelectors.m2a & 0x3U) == 1U)
				++_summary.stageBlendMUsesMemoryClassCount[stageBucket];
			if (imageReadEnabledWork) {
				++_summary.stageImageReadWriteCount;
				++_summary.stageImageReadClassCount[stageBucket];
			}
			if (_work.textured) {
				++_summary.stageTexturedWriteCount;
				++_summary.stageTexturedTriangleWriteCount;
			}
			if ((textureSourceBits & kTexelSourceReplacementBit) != 0U) {
				++_summary.stageTexelSourceReplacementWriteCount;
				++_summary.stageTexelSourceReplacementClassCount[stageBucket];
			}
			if ((textureSourceBits & kTexelSourceTMEMBit) != 0U) {
				++_summary.stageTexelSourceTMEMWriteCount;
				++_summary.stageTexelSourceTMEMClassCount[stageBucket];
			}
			if ((textureSourceBits & kTexelSourceRdramBit) != 0U) {
				++_summary.stageTexelSourceRdramWriteCount;
				++_summary.stageTexelSourceRdramClassCount[stageBucket];
			}
			if ((textureSourceBits & kTexelSourceSyntheticBit) != 0U) {
				++_summary.stageTexelSourceSyntheticWriteCount;
				++_summary.stageTexelSourceSyntheticClassCount[stageBucket];
			}
			++_summary.writeKindTriangleCount;
			if (textureColor != combinerColor)
				++_summary.stageTexelToCombinerDeltaCount;
			if (combinerColor != blenderColor) {
				++_summary.stageCombinerToBlenderDeltaCount;
				++_summary.stageCombinerToBlenderDeltaClassCount[stageBucket];
			}
			if (blenderColor != finalColor)
				++_summary.stageBlenderToFinalDeltaCount;
			if (textureColor != finalColor) {
				++_summary.stageTexelToFinalDeltaCount;
				++_summary.stageTexelToFinalDeltaClassCount[stageBucket];
			}

			const u32 writeColor = selectDebugStageRasterColor(
				stageViewMode,
				textureColor,
				combinerColor,
				blenderColor,
				finalColor);
			const u64 writeLuma = static_cast<u64>(lumaFromRGBA(writeColor));
			_surface.pixels[colorIdx] = encodeSurfaceColor(writeColor, _work.colorImageSize);
			if (!_surface.coverage.empty())
				_surface.coverage[colorIdx] = static_cast<u8>(resolvedCoverage & 0x7U);
			if (!_surface.hiddenCoverage.empty())
				_surface.hiddenCoverage[colorIdx] = resolvedHiddenCoverage ? 1U : 0U;
			_summary.outputLumaSum += writeLuma;
			_summary.writeTriangleLumaSum += writeLuma;
			if ((writeColor & 0x00FFFFFFU) != 0U)
				++_summary.writeTriangleNonBlackCount;
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
	const char * txCachePath = std::getenv("REALITYVK_RVK2_TX_HTS_PATH");
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
	if (invalidateTokenChanged) {
		m_lastSelectedSurface = ExecutorCachedSurface{};
		m_surfaceHistory.clear();
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
		rvk2::loadTextureReplacementHTS(
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
	const std::vector<SubmissionBatchPacket> & _batches,
	const std::vector<ExecutorTMEMSnapshot> * _tmemSnapshots,
	const std::vector<u32> * _workTMEMSnapshotIndices)
{
	ensureTextureReplacementLoaded();
	const TextureReplacementStore * previousReplacementStore = gActiveTextureReplacementStore;
	gActiveTextureReplacementStore =
		(!m_textureReplacementStore.empty() && m_config.textureReplacementEnable)
		? &m_textureReplacementStore
		: nullptr;
	const u64 * previousTMEMWords = gActiveExecutorTMEMWords;

	ExecutorOutput output{};
	ExecutorSummary & summary = output.summary;
	const u64 frameStamp = ++m_surfaceHistoryStamp;
	summary.presentAspectX = m_config.presentAspectX;
	summary.presentAspectY = m_config.presentAspectY;
	summary.viRegistersValid = m_config.viRegistersValid ? 1U : 0U;
	summary.viOriginAddress = m_config.viOrigin & 0x00FFFFFFU;
	summary.viStatus = m_config.viStatus;
	summary.viWidth = m_config.viWidth;
	summary.viVCurrentLine = m_config.viVCurrentLine;
	summary.viVSync = m_config.viVSync;
	summary.viHStart = m_config.viHStart;
	summary.viVStart = m_config.viVStart;
	summary.viXScale = m_config.viXScale;
	summary.viYScale = m_config.viYScale;
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
	const DebugStageViewMode stageViewMode = debugStageViewMode();
	const auto presentSelectedFrame = [&](const VIFrameInput & _presentInput) {
		u64 selectedSurfaceHash = kFnvOffset;
		if (_presentInput.sourcePixels != nullptr
			&& _presentInput.sourceWidth != 0U
			&& _presentInput.sourceHeight != 0U) {
			const size_t requiredPixelCount =
				static_cast<size_t>(_presentInput.sourceWidth)
				* static_cast<size_t>(_presentInput.sourceHeight);
			const size_t availablePixelCount = _presentInput.sourcePixels->size();
			const size_t pixelCount = std::min(requiredPixelCount, availablePixelCount);
			for (size_t i = 0; i < pixelCount; ++i) {
				const u32 pixel = (*_presentInput.sourcePixels)[i];
				updateHashByte(selectedSurfaceHash, static_cast<u8>((pixel >> 0U) & 0xFFU));
				updateHashByte(selectedSurfaceHash, static_cast<u8>((pixel >> 8U) & 0xFFU));
				updateHashByte(selectedSurfaceHash, static_cast<u8>((pixel >> 16U) & 0xFFU));
				updateHashByte(selectedSurfaceHash, static_cast<u8>((pixel >> 24U) & 0xFFU));
			}
		}
		summary.selectedPresentSurfaceHash = selectedSurfaceHash;
		const bool useDirectSource =
			stageViewMode == DebugStageViewMode::kVISource
			&& _presentInput.sourcePixels != nullptr
			&& _presentInput.sourceWidth != 0U
			&& _presentInput.sourceHeight != 0U;
		if (useDirectSource) {
			const size_t requiredPixelCount =
				static_cast<size_t>(_presentInput.sourceWidth)
				* static_cast<size_t>(_presentInput.sourceHeight);
			if (_presentInput.sourcePixels->size() >= requiredPixelCount) {
				output.presentFrame.width = _presentInput.sourceWidth;
				output.presentFrame.height = _presentInput.sourceHeight;
				output.presentFrame.pixels.assign(
					_presentInput.sourcePixels->begin(),
					_presentInput.sourcePixels->begin() + requiredPixelCount);
				u64 hash = kFnvOffset;
				u64 lumaSum = 0ULL;
				u64 nonBlackCount = 0ULL;
				for (u32 pixel : output.presentFrame.pixels) {
					const u8 luma = lumaFromRGBA(pixel);
					lumaSum += static_cast<u64>(luma);
					if (((pixel >> 8U) & 0x00FFFFFFU) != 0U)
						++nonBlackCount;
					updateHashByte(hash, static_cast<u8>((pixel >> 0U) & 0xFFU));
					updateHashByte(hash, static_cast<u8>((pixel >> 8U) & 0xFFU));
					updateHashByte(hash, static_cast<u8>((pixel >> 16U) & 0xFFU));
					updateHashByte(hash, static_cast<u8>((pixel >> 24U) & 0xFFU));
				}
				summary.presentHash = hash;
				summary.presentWidth = _presentInput.sourceWidth;
				summary.presentHeight = _presentInput.sourceHeight;
				summary.viRejectReason = kVIRejectNone;
				summary.viResolvedSourceWidth = _presentInput.sourceWidth;
				summary.viResolvedSourceHeight = _presentInput.sourceHeight;
				summary.viResolvedOutputWidth = _presentInput.sourceWidth;
				summary.viResolvedOutputHeight = _presentInput.sourceHeight;
				summary.viResolvedLineStride = _presentInput.sourceWidth;
				summary.viSourceSampleCount = static_cast<u64>(requiredPixelCount);
				summary.viSourceInvalidSampleCount = 0ULL;
				summary.viSourceLumaSum = lumaSum;
				summary.viOutputLumaSum = lumaSum;
				summary.viOutputNonBlackCount = nonBlackCount;
				summary.viHashDecode = hash;
				summary.viHashFilter = hash;
				summary.viHashGammaDither = hash;
				summary.viResolvedType = _presentInput.sourceSize;
				summary.viResolvedUsesRegisters = 0U;
				return;
			}
		}
		const VIFrameSummary viSummary = viRenderer.present(_presentInput, &output.presentFrame.pixels);
		summary.presentHash = viSummary.presentHash;
		summary.presentWidth = viSummary.presentWidth;
		summary.presentHeight = viSummary.presentHeight;
		summary.viRejectReason = viSummary.rejectReason;
		summary.viResolvedSourceWidth = viSummary.resolvedSourceWidth;
		summary.viResolvedSourceHeight = viSummary.resolvedSourceHeight;
		summary.viResolvedOutputWidth = viSummary.resolvedOutputWidth;
		summary.viResolvedOutputHeight = viSummary.resolvedOutputHeight;
		summary.viResolvedLineStride = viSummary.resolvedLineStride;
		summary.viSourceSampleCount = viSummary.sourceSampleCount;
		summary.viSourceInvalidSampleCount = viSummary.sourceInvalidSampleCount;
		summary.viSourceLumaSum = viSummary.sourceLumaSum;
		summary.viOutputLumaSum = viSummary.outputLumaSum;
		summary.viOutputNonBlackCount = viSummary.outputNonBlackCount;
		summary.viHashDecode = viSummary.hashDecode;
		summary.viHashFilter = viSummary.hashFilter;
		summary.viHashGammaDither = viSummary.hashGammaDither;
		summary.viResolvedType = viSummary.resolvedType;
		summary.viResolvedUsesRegisters = viSummary.usesRegisters;
		output.presentFrame.width = viSummary.presentWidth;
		output.presentFrame.height = viSummary.presentHeight;
	};

	std::unordered_map<u32, ColorSurface> surfaces;
	std::unordered_map<u32, DepthSurface> depthSurfaces;
	std::unordered_map<u32, u64> surfaceColorWrites;
	std::unordered_map<u32, u64> surfaceWorkCounts;
	u32 lastSurfaceAddress = 0U;
	bool hasColorImageAddress = false;
	u32 prevColorImageAddress = 0U;
	const auto recordColorImageEvent = [&](u32 _address, u64 _workOrdinal) {
		if (summary.colorImageEventCount >= kExecutorColorImageEventSlots)
			return;
		const u32 index = static_cast<u32>(summary.colorImageEventCount);
		summary.colorImageEventAddress[index] = _address;
		summary.colorImageEventWorkOrdinal[index] = _workOrdinal;
		++summary.colorImageEventCount;
	};

	for (const SubmissionBatchPacket & batch : _batches) {
		++summary.executedBatchCount;
		if (batch.firstWorkIndex >= _workPackets.size())
			continue;
		const u32 lastIndex = std::min<u32>(
			batch.lastWorkIndex,
			static_cast<u32>(_workPackets.size() - 1U));

		for (u32 workIndex = batch.firstWorkIndex; workIndex <= lastIndex; ++workIndex) {
			const RenderWorkPacket & work = _workPackets[workIndex];
			const u64 * workTMEMWords = nullptr;
			if (_tmemSnapshots != nullptr
				&& _workTMEMSnapshotIndices != nullptr
				&& workIndex < _workTMEMSnapshotIndices->size()) {
				const u32 snapshotIndex = (*_workTMEMSnapshotIndices)[workIndex];
				if (snapshotIndex < _tmemSnapshots->size())
					workTMEMWords = (*_tmemSnapshots)[snapshotIndex].data();
			}
			gActiveExecutorTMEMWords = workTMEMWords;
			++summary.executedWorkCount;
			if (work.opKind != static_cast<u8>(RasterOpKind::kFillRect)
				&& work.opKind != static_cast<u8>(RasterOpKind::kTexRect)
				&& work.opKind != static_cast<u8>(RasterOpKind::kTriangle))
				continue;
			if (work.opKind == static_cast<u8>(RasterOpKind::kFillRect))
				++summary.workKindFillCount;
			else if (work.opKind == static_cast<u8>(RasterOpKind::kTexRect))
				++summary.workKindTexRectCount;
			else if (work.opKind == static_cast<u8>(RasterOpKind::kTriangle))
				++summary.workKindTriangleCount;
			if (work.textured)
				++summary.workTexturedCount;
			if (!hasColorImageAddress) {
				hasColorImageAddress = true;
				prevColorImageAddress = work.colorImageAddress;
				summary.colorImageFirstAddress = work.colorImageAddress;
				recordColorImageEvent(work.colorImageAddress, summary.executedWorkCount);
			}
			else if (work.colorImageAddress != prevColorImageAddress) {
				++summary.colorImageSwitchCount;
				prevColorImageAddress = work.colorImageAddress;
				recordColorImageEvent(work.colorImageAddress, summary.executedWorkCount);
			}
			summary.colorImageLastAddress = work.colorImageAddress;
			++surfaceWorkCounts[work.colorImageAddress];

			ColorSurface & surface = surfaces[work.colorImageAddress];
			surface.format = work.colorImageFormat;
			surface.size = work.colorImageSize;
			if (surface.width == 0U)
				surface.width = std::max<u16>(1U, std::min<u16>(work.colorImageWidth, m_config.maxSurfaceWidth));
			if (surface.height == 0U)
				surface.height = 1U;
			if (surface.pixels.empty())
				surface.pixels.resize(static_cast<size_t>(surface.width) * static_cast<size_t>(surface.height), 0U);
			if (surface.coverage.empty())
				surface.coverage.resize(static_cast<size_t>(surface.width) * static_cast<size_t>(surface.height), 0U);
			if (surface.hiddenCoverage.empty())
				surface.hiddenCoverage.resize(static_cast<size_t>(surface.width) * static_cast<size_t>(surface.height), 0U);

			const u64 writesBefore = summary.colorWriteCount;
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
			const u64 writesAfter = summary.colorWriteCount;
			if (writesAfter > writesBefore)
				surfaceColorWrites[work.colorImageAddress] += (writesAfter - writesBefore);
			lastSurfaceAddress = work.colorImageAddress;
		}
	}

	summary.surfaceCount = static_cast<u64>(surfaces.size());
	struct SurfaceRank
	{
		u32 address = 0U;
		u64 writes = 0ULL;
		u64 works = 0ULL;
	};
	std::vector<SurfaceRank> rankedSurfaces;
	rankedSurfaces.reserve(surfaces.size());
	for (const auto & entry : surfaces) {
		const u32 address = entry.first;
		const auto writeIt = surfaceColorWrites.find(address);
		const auto workIt = surfaceWorkCounts.find(address);
		rankedSurfaces.push_back(SurfaceRank{
			address,
			writeIt != surfaceColorWrites.end() ? writeIt->second : 0ULL,
			workIt != surfaceWorkCounts.end() ? workIt->second : 0ULL
		});
	}
	std::sort(
		rankedSurfaces.begin(),
		rankedSurfaces.end(),
		[](const SurfaceRank & _a, const SurfaceRank & _b) {
			if (_a.writes != _b.writes)
				return _a.writes > _b.writes;
			if (_a.works != _b.works)
				return _a.works > _b.works;
			return _a.address < _b.address;
		});
	summary.debugSurfaceSlotCount = static_cast<u8>(
		std::min<size_t>(rankedSurfaces.size(), static_cast<size_t>(kExecutorDebugSurfaceSlots)));
	for (u32 i = 0U; i < summary.debugSurfaceSlotCount; ++i) {
		summary.debugSurfaceAddress[i] = rankedSurfaces[i].address;
		summary.debugSurfaceWriteCount[i] = rankedSurfaces[i].writes;
		summary.debugSurfaceWorkCount[i] = rankedSurfaces[i].works;
		const auto surfaceIt = surfaces.find(rankedSurfaces[i].address);
		if (surfaceIt != surfaces.end())
			summary.debugSurfaceHash[i] = hashSurfacePixels(surfaceIt->second.pixels);
	}

	u32 presentSurfaceAddress = lastSurfaceAddress;
	if (presentSurfaceAddress != 0U && surfaces.find(presentSurfaceAddress) != surfaces.end())
		summary.presentSelectionReason = kExecutorPresentSelectionLastSurface;
	const bool frameHasLiveSurfaceWrites = !surfaceColorWrites.empty();
	bool viOriginMatchedSurface = false;
	if (m_config.viRegistersValid) {
		const u32 viOriginAddress = m_config.viOrigin & 0x00FFFFFFU;
		u8 preferredSurfaceSize = 0U;
		const bool preferSurfaceSize =
			expectedSurfaceSizeFromVIStatus(m_config, preferredSurfaceSize);
		const bool preferSurfaceWidth = m_config.viWidth != 0U;
		const u16 preferredSurfaceWidth = static_cast<u16>(std::min<u32>(
			std::max<u32>(1U, m_config.viWidth),
			static_cast<u32>(m_config.maxSurfaceWidth)));
		const auto tryHistoryVIOriginSelection = [&](bool _requireRecentHistory) {
			if (m_surfaceHistory.empty())
				return false;
			u32 historyMatchedAddress = presentSurfaceAddress;
			bool historyExact = false;
			if (!chooseHistorySurfaceForVIOrigin(
					m_surfaceHistory,
					viOriginAddress,
					historyMatchedAddress,
					historyExact,
					preferSurfaceSize,
					preferredSurfaceSize,
					preferSurfaceWidth,
					preferredSurfaceWidth))
				return false;
			const auto historyMatchedIt = m_surfaceHistory.find(historyMatchedAddress);
			if (historyMatchedIt == m_surfaceHistory.end())
				return false;
			const u64 historyAge = frameStamp >= historyMatchedIt->second.lastTouched
				? (frameStamp - historyMatchedIt->second.lastTouched)
				: 0ULL;
			if (_requireRecentHistory && historyAge > kExecutorVIHistorySelectionMaxAge)
				return false;
			presentSurfaceAddress = historyMatchedAddress;
			viOriginMatchedSurface = true;
			summary.selectedPresentSurfaceHistoryAge = historyAge;
			summary.presentSelectionReason =
				historyExact
					? kExecutorPresentSelectionVIOriginExact
					: kExecutorPresentSelectionVIOriginRange;
			return true;
		};
		u32 matchedSurfaceAddress = presentSurfaceAddress;
		bool exactMatch = false;
		if (chooseSurfaceForVIOrigin(
				surfaces,
				viOriginAddress,
				matchedSurfaceAddress,
				exactMatch,
				preferSurfaceSize,
				preferredSurfaceSize,
				preferSurfaceWidth,
				preferredSurfaceWidth)) {
			presentSurfaceAddress = matchedSurfaceAddress;
			viOriginMatchedSurface = true;
			summary.presentSelectionReason =
				exactMatch
					? kExecutorPresentSelectionVIOriginExact
					: kExecutorPresentSelectionVIOriginRange;
		}
		else if (frameHasLiveSurfaceWrites) {
			if (!tryHistoryVIOriginSelection(true)) {
				if (presentSurfaceAddress == 0U || surfaces.find(presentSurfaceAddress) == surfaces.end()) {
					u32 fallbackAddress = 0U;
					if (chooseMostWrittenSurfaceAddress(
							surfaces,
							surfaceColorWrites,
							surfaceWorkCounts,
							fallbackAddress)) {
						presentSurfaceAddress = fallbackAddress;
						summary.presentSelectionReason = kExecutorPresentSelectionMostWrittenFallback;
					}
					else {
						u32 nearestAddress = presentSurfaceAddress;
						bool nearestExact = false;
						if (chooseNearestSurfaceForVIOrigin(
								surfaces,
								viOriginAddress,
								nearestAddress,
								nearestExact,
								preferSurfaceSize,
								preferredSurfaceSize,
								preferSurfaceWidth,
								preferredSurfaceWidth)) {
							presentSurfaceAddress = nearestAddress;
							summary.presentSelectionReason = kExecutorPresentSelectionVIOriginNearest;
						}
					}
				}
			}
		}
		else if (!tryHistoryVIOriginSelection(false)) {
			if (!m_surfaceHistory.empty()) {
				u32 nearestAddress = presentSurfaceAddress;
				bool nearestExact = false;
				if (chooseNearestSurfaceForVIOrigin(
						surfaces,
						viOriginAddress,
						nearestAddress,
						nearestExact,
						preferSurfaceSize,
						preferredSurfaceSize,
						preferSurfaceWidth,
						preferredSurfaceWidth)) {
					presentSurfaceAddress = nearestAddress;
					summary.presentSelectionReason = kExecutorPresentSelectionVIOriginNearest;
				}
				else {
					u32 nearestHistoryAddress = presentSurfaceAddress;
					if (chooseNearestHistorySurfaceForVIOrigin(
							m_surfaceHistory,
							viOriginAddress,
							nearestHistoryAddress,
							nearestExact,
							preferSurfaceSize,
							preferredSurfaceSize,
							preferSurfaceWidth,
							preferredSurfaceWidth)) {
						presentSurfaceAddress = nearestHistoryAddress;
						summary.presentSelectionReason = kExecutorPresentSelectionVIOriginNearest;
					}
				}
			}
			else {
				u32 nearestAddress = presentSurfaceAddress;
				bool nearestExact = false;
				if (chooseNearestSurfaceForVIOrigin(
						surfaces,
						viOriginAddress,
						nearestAddress,
						nearestExact,
						preferSurfaceSize,
						preferredSurfaceSize,
						preferSurfaceWidth,
						preferredSurfaceWidth)) {
					presentSurfaceAddress = nearestAddress;
					summary.presentSelectionReason = kExecutorPresentSelectionVIOriginNearest;
				}
			}
		}
	}
	summary.viOriginMatchedSurface = viOriginMatchedSurface ? 1U : 0U;
	auto it = surfaces.find(presentSurfaceAddress);
	if (it == surfaces.end() && m_surfaceHistory.find(presentSurfaceAddress) == m_surfaceHistory.end()) {
		u32 fallbackAddress = 0U;
		if (chooseMostWrittenSurfaceAddress(
				surfaces,
				surfaceColorWrites,
				surfaceWorkCounts,
				fallbackAddress)) {
			presentSurfaceAddress = fallbackAddress;
			summary.presentSelectionReason = kExecutorPresentSelectionMostWrittenFallback;
			it = surfaces.find(presentSurfaceAddress);
		}
	}
	const auto historyIt = m_surfaceHistory.find(presentSurfaceAddress);
	if (it == surfaces.end() && historyIt == m_surfaceHistory.end())
		summary.presentSelectionReason = kExecutorPresentSelectionNoSurface;
	summary.selectedPresentSurfaceAddress = presentSurfaceAddress;
	const auto selectedWriteIt = surfaceColorWrites.find(presentSurfaceAddress);
	if (selectedWriteIt != surfaceColorWrites.end()) {
		summary.selectedPresentSurfaceLiveWriteCount = selectedWriteIt->second;
		summary.selectedPresentSurfaceWriteCount = selectedWriteIt->second;
	}
	const auto selectedWorkIt = surfaceWorkCounts.find(presentSurfaceAddress);
	if (selectedWorkIt != surfaceWorkCounts.end()) {
		summary.selectedPresentSurfaceLiveWorkCount = selectedWorkIt->second;
		summary.selectedPresentSurfaceWorkCount = selectedWorkIt->second;
	}
	if (summary.selectedPresentSurfaceWriteCount == 0ULL
		&& historyIt != m_surfaceHistory.end())
		summary.selectedPresentSurfaceWriteCount = historyIt->second.writeCount;
	if (summary.selectedPresentSurfaceWorkCount == 0ULL
		&& historyIt != m_surfaceHistory.end())
		summary.selectedPresentSurfaceWorkCount = historyIt->second.workCount;

	if (it != surfaces.end()) {
		summary.selectedPresentSurfaceFromHistory = 0U;
		summary.selectedPresentSurfaceHistoryAge = 0ULL;
		summary.selectedPresentSurfaceWidth = it->second.width;
		summary.selectedPresentSurfaceHeight = it->second.height;
		summary.selectedPresentSurfaceSize = it->second.size;
		VIFrameInput presentInput{};
		presentInput.sourceAddressValid = viOriginMatchedSurface;
		presentInput.sourceAddress = presentSurfaceAddress;
		presentInput.sourceWidth = it->second.width;
		presentInput.sourceHeight = it->second.height;
		presentInput.sourceSize = it->second.size;
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
		presentSelectedFrame(presentInput);
		m_lastSelectedSurface.valid = true;
		m_lastSelectedSurface.address = presentSurfaceAddress;
		m_lastSelectedSurface.format = it->second.format;
		m_lastSelectedSurface.size = it->second.size;
		m_lastSelectedSurface.width = it->second.width;
		m_lastSelectedSurface.height = it->second.height;
		m_lastSelectedSurface.writeCount = summary.selectedPresentSurfaceWriteCount;
		m_lastSelectedSurface.workCount = summary.selectedPresentSurfaceWorkCount;
		m_lastSelectedSurface.lastTouched = frameStamp;
		m_lastSelectedSurface.pixels = it->second.pixels;
	}
	else if (historyIt != m_surfaceHistory.end()) {
		summary.selectedPresentSurfaceFromHistory = 1U;
		const ExecutorCachedSurface & cached = historyIt->second;
		summary.selectedPresentSurfaceHistoryAge = frameStamp >= cached.lastTouched
			? (frameStamp - cached.lastTouched)
			: 0ULL;
		summary.selectedPresentSurfaceWidth = cached.width;
		summary.selectedPresentSurfaceHeight = cached.height;
		summary.selectedPresentSurfaceSize = cached.size;
		bool cacheExactMatch = false;
		const bool cacheOriginMatch =
			m_config.viRegistersValid
			&& originMatchesSurfaceRange(
				cached.address,
				cached,
				m_config.viOrigin & 0x00FFFFFFU,
				cacheExactMatch);
		if (cacheOriginMatch)
			summary.viOriginMatchedSurface = 1U;
		else if (summary.presentSelectionReason == kExecutorPresentSelectionNoSurface)
			summary.presentSelectionReason = kExecutorPresentSelectionPreviousSurface;

		VIFrameInput presentInput{};
		presentInput.sourceAddressValid = cacheOriginMatch;
		presentInput.sourceAddress = cached.address;
		presentInput.sourceWidth = cached.width;
		presentInput.sourceHeight = cached.height;
		presentInput.sourceSize = cached.size;
		presentInput.sourcePixels = &cached.pixels;
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
		presentSelectedFrame(presentInput);
		m_lastSelectedSurface = cached;
		m_lastSelectedSurface.lastTouched = frameStamp;
	}
	else if (m_lastSelectedSurface.valid) {
		summary.selectedPresentSurfaceHistoryAge = frameStamp >= m_lastSelectedSurface.lastTouched
			? (frameStamp - m_lastSelectedSurface.lastTouched)
			: 0ULL;
		summary.selectedPresentSurfaceWidth = m_lastSelectedSurface.width;
		summary.selectedPresentSurfaceHeight = m_lastSelectedSurface.height;
		summary.selectedPresentSurfaceSize = m_lastSelectedSurface.size;
		bool cacheExactMatch = false;
		const bool cacheOriginMatch =
			m_config.viRegistersValid
			&& originMatchesSurfaceRange(
				m_lastSelectedSurface.address,
				m_lastSelectedSurface,
				m_config.viOrigin & 0x00FFFFFFU,
				cacheExactMatch);
		if (cacheOriginMatch) {
			summary.viOriginMatchedSurface = 1U;
			summary.presentSelectionReason =
				cacheExactMatch
					? kExecutorPresentSelectionVIOriginExact
					: kExecutorPresentSelectionVIOriginRange;
		}
		else
			summary.presentSelectionReason = kExecutorPresentSelectionPreviousSurface;
		summary.selectedPresentSurfaceAddress = m_lastSelectedSurface.address;
		summary.selectedPresentSurfaceWriteCount = m_lastSelectedSurface.writeCount;
		summary.selectedPresentSurfaceWorkCount = m_lastSelectedSurface.workCount;

		VIFrameInput presentInput{};
		presentInput.sourceAddressValid = cacheOriginMatch;
		presentInput.sourceAddress = m_lastSelectedSurface.address;
		presentInput.sourceWidth = m_lastSelectedSurface.width;
		presentInput.sourceHeight = m_lastSelectedSurface.height;
		presentInput.sourceSize = m_lastSelectedSurface.size;
		presentInput.sourcePixels = &m_lastSelectedSurface.pixels;
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
		presentSelectedFrame(presentInput);
	}
	else {
		summary.presentSelectionReason = kExecutorPresentSelectionNoSurface;
		summary.viRejectReason = kVIRejectMissingSource;
	}

	for (const auto & entry : surfaces) {
		const u32 address = entry.first;
		const ColorSurface & surface = entry.second;
		ExecutorCachedSurface & cached = m_surfaceHistory[address];
		cached.valid = true;
		cached.address = address;
		cached.format = surface.format;
		cached.size = surface.size;
		cached.width = surface.width;
		cached.height = surface.height;
		const auto writeIt = surfaceColorWrites.find(address);
		const auto workIt = surfaceWorkCounts.find(address);
		cached.writeCount = writeIt != surfaceColorWrites.end() ? writeIt->second : 0ULL;
		cached.workCount = workIt != surfaceWorkCounts.end() ? workIt->second : 0ULL;
		cached.lastTouched = frameStamp;
		cached.pixels = surface.pixels;
	}
	while (m_surfaceHistory.size() > kExecutorSurfaceHistoryLimit) {
		u32 oldestAddress = 0U;
		u64 oldestTouched = std::numeric_limits<u64>::max();
		bool foundOldest = false;
		for (const auto & historyEntry : m_surfaceHistory) {
			if (!foundOldest
				|| historyEntry.second.lastTouched < oldestTouched
				|| (historyEntry.second.lastTouched == oldestTouched
					&& historyEntry.first < oldestAddress)) {
				foundOldest = true;
				oldestAddress = historyEntry.first;
				oldestTouched = historyEntry.second.lastTouched;
			}
		}
		if (!foundOldest)
			break;
		m_surfaceHistory.erase(oldestAddress);
	}

	gActiveExecutorSummary = previousExecutorSummary;
	gActiveTextureReplacementStore = previousReplacementStore;
	gActiveExecutorTMEMWords = previousTMEMWords;
	return output;
}

ExecutorSummary Executor::execute(
	const std::vector<RenderWorkPacket> & _workPackets,
	const std::vector<SubmissionBatchPacket> & _batches,
	const std::vector<ExecutorTMEMSnapshot> * _tmemSnapshots,
	const std::vector<u32> * _workTMEMSnapshotIndices)
{
	return executeWithOutput(
		_workPackets,
		_batches,
		_tmemSnapshots,
		_workTMEMSnapshotIndices).summary;
}

} // namespace rvk2
