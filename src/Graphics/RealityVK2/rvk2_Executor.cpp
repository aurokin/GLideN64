#include "rvk2_Executor.h"

#include <array>
#include <algorithm>
#include <atomic>
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
#include "rvk2_Env.h"
#include "rvk2_VIRenderer.h"

namespace {

thread_local const rvk2::TextureReplacementStore * gActiveTextureReplacementStore = nullptr;
thread_local rvk2::ExecutorSummary * gActiveExecutorSummary = nullptr;
thread_local const u64 * gActiveExecutorTMEMWords = nullptr;
thread_local u64 gActiveExecutorFrameId = 0ULL;

enum : u8
{
	kDebugTexSampleSourceNone = 0U,
	kDebugTexSampleSourceReplacement = 1U,
	kDebugTexSampleSourceTMEM = 2U,
	kDebugTexSampleSourceRDRAM = 3U,
	kDebugTexSampleSourceSynthetic = 4U,
};

enum : u8
{
	kDebugTexFetchVariantNone = 0U,
	kDebugTexFetchVariantTMEM4 = 1U,
	kDebugTexFetchVariantTMEM8 = 2U,
	kDebugTexFetchVariantTMEM16 = 3U,
	kDebugTexFetchVariantTMEM32Split = 4U,
	kDebugTexFetchVariantTMEM32Direct = 5U,
	kDebugTexFetchVariantTMEM32Canonical = 6U,
	kDebugTexFetchVariantRDRAM4 = 7U,
	kDebugTexFetchVariantRDRAM8 = 8U,
	kDebugTexFetchVariantRDRAM16 = 9U,
	kDebugTexFetchVariantRDRAM32 = 10U,
};

struct DebugTextureSampleLogEntry
{
	bool valid = false;
	u8 sourceKind = kDebugTexSampleSourceNone;
	u8 sampleSlot = 0U;
	u8 format = 0U;
	u8 size = 0U;
	u8 filterMode = 0U;
	u8 lutMode = 0U;
	u8 includeW = 0U;
	u8 needsLUT = 0U;
	u8 tmemReject = 0U;
	u8 rdramReject = 0U;
	u8 tmemLoadKind = 0U;
	u8 tmemFetchVariant = kDebugTexFetchVariantNone;
	u8 reserved0 = 0U;
	u8 tmemLoadTile = 0U;
	u16 tmemLoadULS = 0U;
	u16 tmemLoadULT = 0U;
	u16 tmemLoadLRS = 0U;
	u16 tmemLoadLRT = 0U;
	u16 tmemLoadDXT = 0U;
	u16 tmemLoadSpanTexels = 0U;
	u16 tmemLoadQwords = 0U;
	u16 tmemLoadEstimatedWordsPerLine = 0U;
	u16 tlutLookupAddress = 0U;
	u16 tlutRawEntry = 0U;
	u16 tlutDecodedEntry = 0U;
	u8 tlutApplied = 0U;
	u16 tile = 0U;
	u16 tileLine = 0U;
	u16 tileTmem = 0U;
	u16 tilePalette = 0U;
	u16 textureImageWidth = 0U;
	u16 tmemOffset = 0U;
	u16 tmemS = 0U;
	u16 tmemT = 0U;
	u16 tmemI = 0U;
	u32 tmemRowXor = 0U;
	s32 tmemLineStride = 0;
	u32 tmemIndexA = 0U;
	u32 tmemIndexB = 0U;
	u32 tmemRawA = 0U;
	u32 tmemRawB = 0U;
	u32 tmemPacked32 = 0U;
	u32 rdramBaseAddress = 0U;
	u64 rdramTexelIndex = 0ULL;
	u32 rdramWordAddress = 0U;
	u32 rdramRaw = 0U;
	s32 requestedS = 0;
	s32 requestedT = 0;
	s32 requestedW = 0;
	u32 pixelX = 0U;
	u32 pixelY = 0U;
	u32 sampledColor = 0U;
	u32 finalColor = 0U;
	u32 sourceBits = 0U;
	u8 rdramProbeValid = 0U;
	u8 rdramProbeNeedsLUT = 0U;
	u8 rdramProbeReject = 0U;
	u8 reserved1 = 0U;
	u32 rdramProbeSampledColor = 0U;
	u32 rdramProbeFinalColor = 0U;
	u32 rdramProbeWordAddress = 0U;
	u32 rdramProbeRaw = 0U;
};

thread_local std::array<DebugTextureSampleLogEntry, rvk2::kExecutorTextureSampleSlotBuckets>
	gDebugTextureSampleLogSlots{};

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
	uint64_t parsed = 0ULL;
	if (!rvk2::parseEnvUnsignedValue(_value, parsed))
		return false;
	_out = static_cast<u64>(parsed);
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

inline bool parseUnsignedToken(const std::string & _token, u64 & _out);

enum class DebugStageViewMode : u8
{
	kFinal = 0U,
	kTexelRaw,
	kCombinerOut,
	kBlenderOut,
	kVISource,
	kWriteMask,
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
		if (token == "write_mask" || token == "writes")
			return DebugStageViewMode::kWriteMask;
		return DebugStageViewMode::kFinal;
	}();
	return mode;
}

#define RVK2_EXECUTOR_DEBUG_BOOL_OPTIONS(_X) \
	_X(debugDisableCycle2PrevMemoryColor, "REALITYVK_RVK2_DEBUG_DISABLE_CYCLE2_PREV_MEMORY", false) \
	_X(debugForceBlenderDivide, "REALITYVK_RVK2_DEBUG_FORCE_BLEND_DIVIDE", false) \
	_X(debugEnableLegacyMemoryAlphaBlend, "REALITYVK_RVK2_DEBUG_ENABLE_LEGACY_MEMORY_ALPHA_BLEND", false) \
	_X(debugSwapTmem16Samples, "REALITYVK_RVK2_DEBUG_SWAP_TMEM16", false) \
	_X(debugDisableTriangleWrites, "REALITYVK_RVK2_DEBUG_DISABLE_TRIANGLE_WRITES", false) \
	_X(debugPreserveTriangleNonBlackOverwrites, "REALITYVK_RVK2_DEBUG_TRIANGLE_PRESERVE_NON_BLACK", false) \
	_X(debugDisableTexRectWrites, "REALITYVK_RVK2_DEBUG_DISABLE_TEXRECT_WRITES", false) \
	_X(debugPreserveTexRectNonBlackOverwrites, "REALITYVK_RVK2_DEBUG_TEXRECT_PRESERVE_NON_BLACK", false) \
	_X(debugSwapTmem4Nibbles, "REALITYVK_RVK2_DEBUG_SWAP_TMEM4_NIBBLES", false) \
	_X(debugAltTmem8OddXor, "REALITYVK_RVK2_DEBUG_ALT_TMEM8_XOR", false) \
	_X(debugTmem8UseLoadKindAwareXor, "REALITYVK_RVK2_DEBUG_TMEM8_LOADKIND_XOR", false) \
	_X(debugTmem8UseXor13, "REALITYVK_RVK2_DEBUG_TMEM8_XOR13", false) \
	_X(debugDisableTexturePerspCoord, "REALITYVK_RVK2_DEBUG_DISABLE_TEXTURE_PERSP_COORD", false) \
	_X(debugDisableTextureLodCoord, "REALITYVK_RVK2_DEBUG_DISABLE_TEXTURE_LOD_COORD", false) \
	_X(debugDisableCoverageControls, "REALITYVK_RVK2_DEBUG_DISABLE_COVERAGE_CONTROLS", false) \
	_X(debugDisableColorOnCvgInhibit, "REALITYVK_RVK2_DEBUG_DISABLE_COLOR_ON_CVG_INHIBIT", false) \
	_X(debugDisableBlendEnGating, "REALITYVK_RVK2_DEBUG_DISABLE_BLEND_EN_GATING", false) \
	_X(debugDisableLegacyMemoryAlphaShift, "REALITYVK_RVK2_DEBUG_DISABLE_LEGACY_MEMORY_ALPHA_SHIFT", false) \
	_X(debugUseWideMemoryAlphaModel, "REALITYVK_RVK2_DEBUG_USE_WIDE_MEMORY_ALPHA", false) \
	_X(debugBypassBlender, "REALITYVK_RVK2_DEBUG_BYPASS_BLENDER", false) \
	_X(debugDisableBlenderDither, "REALITYVK_RVK2_DEBUG_DISABLE_BLENDER_DITHER", false) \
	_X(debugDisableFillWrites, "REALITYVK_RVK2_DEBUG_DISABLE_FILL_WRITES", false) \
	_X(debugDisableBlendMemoryColorSource, "REALITYVK_RVK2_DEBUG_DISABLE_BLEND_MEMORY_COLOR_SOURCE", false) \
	_X(debugDisableImageRead, "REALITYVK_RVK2_DEBUG_DISABLE_IMAGE_READ", false) \
	_X(debugForceTexelAlphaOpaque, "REALITYVK_RVK2_DEBUG_FORCE_TEXEL_ALPHA_OPAQUE", false) \
	_X(debugCycle2SecondPassMemoryFromCycle1, "REALITYVK_RVK2_DEBUG_CYCLE2_SECOND_PASS_MEMORY_FROM_CYCLE1", true) \
	_X(debugDisableTextureLUTApply, "REALITYVK_RVK2_DEBUG_DISABLE_TEXTURE_LUT_APPLY", false) \
	_X(debugDisableTLUTRGBA16Swap, "REALITYVK_RVK2_DEBUG_DISABLE_TLUT_RGBA16_SWAP", false) \
	_X(debugDisableCopyModeDSDXDiv4, "REALITYVK_RVK2_DEBUG_DISABLE_COPY_DSDX_DIV4", false) \
	_X(debugForceTextureRdramPrimary, "REALITYVK_RVK2_DEBUG_FORCE_TEXTURE_RDRAM_PRIMARY", false) \
	_X(debugTexelFallbackRdramIfTmemBlack, "REALITYVK_RVK2_DEBUG_TEXEL_FALLBACK_RDRAM_IF_TMEM_BLACK", false) \
	_X(debugForceCopyCI8RdramPrimary, "REALITYVK_RVK2_DEBUG_FORCE_COPY_CI8_RDRAM_PRIMARY", false) \
	_X(debugCycle1CombinerUseCycle1Selectors, "REALITYVK_RVK2_DEBUG_CYCLE1_COMBINER_USE_CYCLE1_SELECTORS", false) \
	_X(debugTmem32UseDirectLinearFetch, "REALITYVK_RVK2_DEBUG_TMEM32_DIRECT_LINEAR", false) \
	_X(debugTmem32UseXor02, "REALITYVK_RVK2_DEBUG_TMEM32_XOR02", false) \
	_X(debugTmem32PackHighToLowRGBA, "REALITYVK_RVK2_DEBUG_TMEM32_PACK_HIGH_TO_LOW", false) \
	_X(debugTmem32UseLoadKindAwareXor, "REALITYVK_RVK2_DEBUG_TMEM32_LOADKIND_XOR", false) \
	_X(debugTmem32UseCanonicalFetch, "REALITYVK_RVK2_DEBUG_TMEM32_CANONICAL_FETCH", false) \
	_X(debugDisableVIHistoryPresentSelection, "REALITYVK_RVK2_DEBUG_DISABLE_VI_HISTORY_PRESENT", false) \
	_X(debugPreferLiveSurfaceOverHistory, "REALITYVK_RVK2_DEBUG_PREFER_LIVE_SURFACE_OVER_HISTORY", true) \
	_X(debugKeepVIMatchedHistorySelection, "REALITYVK_RVK2_DEBUG_KEEP_VI_MATCHED_HISTORY_SELECTION", true) \
	_X(debugEnableSurfaceHistoryBootstrap, "REALITYVK_RVK2_DEBUG_ENABLE_SURFACE_HISTORY_BOOTSTRAP", false) \
	_X(debugEnableCrossSurfaceBootstrap, "REALITYVK_RVK2_DEBUG_ENABLE_CROSS_SURFACE_BOOTSTRAP", false) \
	_X(debugCrossSurfaceBootstrapCopyAllFromLastSurface, "REALITYVK_RVK2_DEBUG_CROSS_SURFACE_BOOTSTRAP_COPY_ALL", false) \
	_X(debugEnableUntouchedPresentCarry, "REALITYVK_RVK2_DEBUG_ENABLE_UNTOUCHED_PRESENT_CARRY", false) \
	_X(debugHistoryMergeCopyNonBlack, "REALITYVK_RVK2_DEBUG_HISTORY_MERGE_COPY_NONBLACK", false) \
	_X(debugAllowDepthAliasHistoryCarry, "REALITYVK_RVK2_DEBUG_ALLOW_DEPTH_ALIAS_HISTORY_CARRY", false) \
	_X(debugOverwriteLogIncludeBlackWrites, "REALITYVK_RVK2_DEBUG_OVERWRITE_LOG_INCLUDE_BLACK_WRITES", false) \
	_X(debugOverwriteLogIncludeAllWrites, "REALITYVK_RVK2_DEBUG_OVERWRITE_LOG_INCLUDE_ALL_WRITES", false) \
	_X(debugOverwriteLogIncludeTexelDetail, "REALITYVK_RVK2_DEBUG_OVERWRITE_LOG_INCLUDE_TEXEL_DETAIL", false) \
	_X(debugTextureFilterStrictPrimary, "REALITYVK_RVK2_DEBUG_TEXTURE_FILTER_STRICT_PRIMARY", false) \
	_X(debugTextureFilterMode3UsesBilerp, "REALITYVK_RVK2_DEBUG_TEXTURE_FILTER_MODE3_BILERP", false) \
	_X(debugTextureFilterMode2UsesAverage, "REALITYVK_RVK2_DEBUG_TEXTURE_FILTER_MODE2_AVERAGE", false) \
	_X(debugPseudoTriangleUsePrimColor, "REALITYVK_RVK2_DEBUG_PSEUDO_TRIANGLE_USE_PRIM_COLOR", false) \
	_X(debugForceAllTexelAlphaOpaque, "REALITYVK_RVK2_DEBUG_FORCE_ALL_TEXEL_ALPHA_OPAQUE", false) \
	_X(debugTmem32CompareAlternates, "REALITYVK_RVK2_DEBUG_TMEM32_COMPARE", false) \
	_X(debugInvertTriangleLMajor, "REALITYVK_RVK2_DEBUG_TRIANGLE_INVERT_LMAJOR", false) \
	_X(debugForceTexel1UsesTile0, "REALITYVK_RVK2_DEBUG_FORCE_TEXEL1_TILE0", false) \
	_X(debugCopyPhaseUseTexel1, "REALITYVK_RVK2_DEBUG_COPY_USE_TEXEL1", false) \
	_X(debugForcePipelineModeOn, "REALITYVK_RVK2_DEBUG_FORCE_PIPELINE_MODE_ON", false) \
	_X(debugForcePipelineModeOff, "REALITYVK_RVK2_DEBUG_FORCE_PIPELINE_MODE_OFF", false) \
	_X(debugDisableColorImage16Quantize, "REALITYVK_RVK2_DEBUG_DISABLE_COLOR_IMAGE_16_QUANTIZE", false) \
	_X(debugForceColorImage16Quantize, "REALITYVK_RVK2_DEBUG_FORCE_COLOR_IMAGE_16_QUANTIZE", false)

#define RVK2_DEFINE_EXECUTOR_DEBUG_BOOL(_name, _key, _defaultValue) \
	bool _name() \
	{ \
		static const bool enabled = rvk2::envFlagEnabled((_key), (_defaultValue)); \
		return enabled; \
	}

RVK2_EXECUTOR_DEBUG_BOOL_OPTIONS(RVK2_DEFINE_EXECUTOR_DEBUG_BOOL)

#undef RVK2_DEFINE_EXECUTOR_DEBUG_BOOL

const char * debugStageViewModeName(DebugStageViewMode _mode)
{
	switch (_mode) {
	case DebugStageViewMode::kTexelRaw:
		return "texel_raw";
	case DebugStageViewMode::kCombinerOut:
		return "combiner_out";
	case DebugStageViewMode::kBlenderOut:
		return "blender_out";
	case DebugStageViewMode::kVISource:
		return "vi_source";
	case DebugStageViewMode::kWriteMask:
		return "write_mask";
	case DebugStageViewMode::kFinal:
	default:
		return "final";
	}
}

void appendDebugEntry(std::vector<std::string> & _out, const char * _key, const std::string & _value)
{
	std::string entry(_key);
	entry += "=";
	entry += _value;
	_out.push_back(std::move(entry));
}

inline void appendBoolDebugEntry(
	std::vector<std::string> & _out,
	const char * _key,
	bool _value,
	bool _defaultValue)
{
	if (_value != _defaultValue)
		appendDebugEntry(_out, _key, _value ? "1" : "0");
}

bool shouldLogActiveDebugToggles()
{
	static const bool enabled = rvk2::envFlagEnabled("REALITYVK_RVK2_DEBUG_LOG_ACTIVE", true);
	return enabled;
}

void collectActiveDebugToggles(std::vector<std::string> & _out)
{
#define RVK2_COLLECT_ACTIVE_BOOL(_name, _key, _defaultValue) \
	{ \
		const bool value = _name(); \
		if (value != (_defaultValue)) \
			appendDebugEntry(_out, (_key), value ? "1" : "0"); \
	}

	RVK2_EXECUTOR_DEBUG_BOOL_OPTIONS(RVK2_COLLECT_ACTIVE_BOOL)

#undef RVK2_COLLECT_ACTIVE_BOOL

	const DebugStageViewMode stageView = debugStageViewMode();
	if (stageView != DebugStageViewMode::kFinal)
		appendDebugEntry(_out, "REALITYVK_RVK2_DEBUG_STAGE_VIEW", debugStageViewModeName(stageView));

	if (const char * path = rvk2::envStringOrNull("REALITYVK_RVK2_DEBUG_HISTORY_MERGE_LOG"))
		appendDebugEntry(_out, "REALITYVK_RVK2_DEBUG_HISTORY_MERGE_LOG", path);
	if (const char * path = rvk2::envStringOrNull("REALITYVK_RVK2_DEBUG_OVERWRITE_LOG"))
		appendDebugEntry(_out, "REALITYVK_RVK2_DEBUG_OVERWRITE_LOG", path);
	if (const char * path = rvk2::envStringOrNull("REALITYVK_RVK2_DEBUG_TRIANGLE_PACKET_LOG"))
		appendDebugEntry(_out, "REALITYVK_RVK2_DEBUG_TRIANGLE_PACKET_LOG", path);
	if (const char * raw = rvk2::envStringOrNull("REALITYVK_RVK2_DEBUG_OVERWRITE_LOG_LIMIT"))
		appendDebugEntry(_out, "REALITYVK_RVK2_DEBUG_OVERWRITE_LOG_LIMIT", raw);
	if (const char * raw = rvk2::envStringOrNull("REALITYVK_RVK2_DEBUG_TRIANGLE_PACKET_LOG_LIMIT"))
		appendDebugEntry(_out, "REALITYVK_RVK2_DEBUG_TRIANGLE_PACKET_LOG_LIMIT", raw);
	if (const char * raw = rvk2::envStringOrNull("REALITYVK_RVK2_DEBUG_FORCE_PRESENT_SURFACE"))
		appendDebugEntry(_out, "REALITYVK_RVK2_DEBUG_FORCE_PRESENT_SURFACE", raw);
	if (const char * raw = rvk2::envStringOrNull("REALITYVK_RVK2_DEBUG_TLUT_LOOKUP_OFFSET"))
		appendDebugEntry(_out, "REALITYVK_RVK2_DEBUG_TLUT_LOOKUP_OFFSET", raw);
	if (const char * raw = rvk2::envStringOrNull("REALITYVK_RVK2_DEBUG_TRIANGLE_SAMPLE_Y_SUBPIXEL_BIAS"))
		appendDebugEntry(_out, "REALITYVK_RVK2_DEBUG_TRIANGLE_SAMPLE_Y_SUBPIXEL_BIAS", raw);
	if (const char * raw = rvk2::envStringOrNull("REALITYVK_RVK2_DEBUG_TRIANGLE_SAMPLE_X_SUBPIXEL_BIAS"))
		appendDebugEntry(_out, "REALITYVK_RVK2_DEBUG_TRIANGLE_SAMPLE_X_SUBPIXEL_BIAS", raw);
	if (const char * raw = rvk2::envStringOrNull("REALITYVK_RVK2_DEBUG_TEXTURE_BUCKET_MASK"))
		appendDebugEntry(_out, "REALITYVK_RVK2_DEBUG_TEXTURE_BUCKET_MASK", raw);

	appendBoolDebugEntry(
		_out,
		"REALITYVK_RVK2_PRESENT_FLIP_Y",
		rvk2::envFlagEnabled("REALITYVK_RVK2_PRESENT_FLIP_Y", true),
		true);
	appendBoolDebugEntry(
		_out,
		"REALITYVK_RVK2_SHADOW_DRAW",
		rvk2::envFlagEnabled("REALITYVK_RVK2_SHADOW_DRAW", false),
		false);
	appendBoolDebugEntry(
		_out,
		"REALITYVK_RVK2_SHADOW_PRESENT",
		rvk2::envFlagEnabled("REALITYVK_RVK2_SHADOW_PRESENT", false),
		false);
	appendBoolDebugEntry(
		_out,
		"REALITYVK_RVK2_DEBUG_DISABLE_SAME_FRAME_PRESENT_ACCUM",
		rvk2::envFlagEnabled("REALITYVK_RVK2_DEBUG_DISABLE_SAME_FRAME_PRESENT_ACCUM", false),
		false);

	appendBoolDebugEntry(
		_out,
		"REALITYVK_RVK2_DEBUG_DISABLE_VI_ORIGIN_OFFSET",
		rvk2::envFlagEnabled("REALITYVK_RVK2_DEBUG_DISABLE_VI_ORIGIN_OFFSET", false),
		false);
	const bool enableVIPixelAdvance =
		rvk2::envFlagEnabled("REALITYVK_RVK2_DEBUG_ENABLE_VI_PIXEL_ADVANCE", false);
	const bool disableVIPixelAdvance =
		rvk2::envFlagEnabled("REALITYVK_RVK2_DEBUG_DISABLE_VI_PIXEL_ADVANCE", false);
	appendBoolDebugEntry(
		_out,
		"REALITYVK_RVK2_DEBUG_ENABLE_VI_PIXEL_ADVANCE",
		enableVIPixelAdvance,
		false);
	appendBoolDebugEntry(
		_out,
		"REALITYVK_RVK2_DEBUG_DISABLE_VI_PIXEL_ADVANCE",
		disableVIPixelAdvance,
		false);
	if (enableVIPixelAdvance || disableVIPixelAdvance) {
		appendDebugEntry(
			_out,
			"REALITYVK_RVK2_DEBUG_VI_PIXEL_ADVANCE_EFFECTIVE",
			(enableVIPixelAdvance && !disableVIPixelAdvance) ? "1" : "0");
	}
	if (const char * viAspect = rvk2::envStringOrNull("REALITYVK_RVK2_VI_ASPECT")) {
		appendDebugEntry(_out, "REALITYVK_RVK2_VI_ASPECT", viAspect);
	}

	if (const char * tracePath = rvk2::envStringOrNull("REALITYVK_RVK2_TRACE_FILE")) {
		appendDebugEntry(_out, "REALITYVK_RVK2_TRACE_FILE", tracePath);
	}
	if (const char * packetTracePath = rvk2::envStringOrNull("REALITYVK_RVK2_PACKET_TRACE_FILE")) {
		appendDebugEntry(_out, "REALITYVK_RVK2_PACKET_TRACE_FILE", packetTracePath);
	}
	if (const char * forensicsPath = rvk2::envStringOrNull("REALITYVK_RVK2_FRAME_FORENSICS_FILE")) {
		appendDebugEntry(_out, "REALITYVK_RVK2_FRAME_FORENSICS_FILE", forensicsPath);
	}
	appendBoolDebugEntry(
		_out,
		"REALITYVK_RVK2_TRACE_LOG_SUMMARY",
		rvk2::envFlagEnabled("REALITYVK_RVK2_TRACE_LOG_SUMMARY", false),
		false);

	appendBoolDebugEntry(
		_out,
		"REALITYVK_VK_TRACE_FBO",
		rvk2::envFlagEnabled("REALITYVK_VK_TRACE_FBO", false),
		false);
	appendBoolDebugEntry(
		_out,
		"REALITYVK_VK_DEBUG_READBACK",
		rvk2::envFlagEnabled("REALITYVK_VK_DEBUG_READBACK", false),
		false);
	if (const char * raw = rvk2::envStringOrNull("REALITYVK_VK_TRACE_FBO_LIMIT"))
		appendDebugEntry(_out, "REALITYVK_VK_TRACE_FBO_LIMIT", raw);
	if (const char * raw = rvk2::envStringOrNull("REALITYVK_VK_DEBUG_READBACK_LIMIT"))
		appendDebugEntry(_out, "REALITYVK_VK_DEBUG_READBACK_LIMIT", raw);
}

void logActiveDebugTogglesOnce()
{
	static std::atomic<bool> logged{false};
	if (logged.exchange(true, std::memory_order_relaxed))
		return;
	if (!shouldLogActiveDebugToggles())
		return;

	std::vector<std::string> active;
	collectActiveDebugToggles(active);
	if (active.empty())
		return;
	std::sort(active.begin(), active.end());
	std::fprintf(stderr, "rvk2 debug toggles active (%u):\n", static_cast<unsigned>(active.size()));
	for (const std::string & entry : active)
		std::fprintf(stderr, "  %s\n", entry.c_str());
}

#undef RVK2_EXECUTOR_DEBUG_BOOL_OPTIONS

u32 debugTLUTLookupOffset()
{
	static const u32 offset = []() -> u32 {
		return rvk2::envU32Clamped("REALITYVK_RVK2_DEBUG_TLUT_LOOKUP_OFFSET", 0U, 3U, 10);
	}();
	return offset;
}

const char * debugHistoryMergeLogPath()
{
	static const char * path = []() -> const char * {
		return rvk2::envStringOrNull("REALITYVK_RVK2_DEBUG_HISTORY_MERGE_LOG");
	}();
	return path;
}

bool debugForcedPresentSurfaceAddress(u32 & _outAddress)
{
	static bool hasAddress = false;
	static u32 address = 0U;
	static bool parsed = false;
	if (!parsed) {
		parsed = true;
		const char * raw = rvk2::envStringOrNull("REALITYVK_RVK2_DEBUG_FORCE_PRESENT_SURFACE");
		uint64_t parsedAddress = 0ULL;
		if (rvk2::parseEnvUnsignedValue(raw, parsedAddress)) {
			hasAddress = true;
			address = static_cast<u32>(parsedAddress & 0x00FFFFFFULL);
		}
	}
	if (!hasAddress)
		return false;
	_outAddress = address;
	return true;
}

void appendHistoryMergeLog(
	u64 _frameStamp,
	u32 _presentAddress,
	u32 _candidateAddress,
	u64 _potentialBlackFill,
	u64 _potentialNonBlackDiff,
	u64 _copiedPixels)
{
	const char * logPath = debugHistoryMergeLogPath();
	if (logPath == nullptr)
		return;
	std::FILE * file = std::fopen(logPath, "ab");
	if (file == nullptr)
		return;
	std::fprintf(
		file,
		"frame=%llu\tpresent=0x%08X\tcandidate=0x%08X\tpotential_black_fill=%llu\tpotential_nonblack_diff=%llu\tcopied=%llu\n",
		static_cast<unsigned long long>(_frameStamp),
		_presentAddress,
		_candidateAddress,
		static_cast<unsigned long long>(_potentialBlackFill),
		static_cast<unsigned long long>(_potentialNonBlackDiff),
		static_cast<unsigned long long>(_copiedPixels));
	std::fclose(file);
}

const char * debugOverwriteLogPath()
{
	static const char * path = []() -> const char * {
		return rvk2::envStringOrNull("REALITYVK_RVK2_DEBUG_OVERWRITE_LOG");
	}();
	return path;
}

u32 debugOverwriteLogLimit()
{
	static const u32 limit = []() -> u32 {
		return rvk2::envU32Clamped(
			"REALITYVK_RVK2_DEBUG_OVERWRITE_LOG_LIMIT",
			200000U,
			std::numeric_limits<u32>::max(),
			10);
	}();
	return limit;
}

const char * debugTrianglePacketLogPath()
{
	static const char * path = []() -> const char * {
		return rvk2::envStringOrNull("REALITYVK_RVK2_DEBUG_TRIANGLE_PACKET_LOG");
	}();
	return path;
}

u32 debugTrianglePacketLogLimit()
{
	static const u32 limit = []() -> u32 {
		return rvk2::envU32Clamped(
			"REALITYVK_RVK2_DEBUG_TRIANGLE_PACKET_LOG_LIMIT",
			200000U,
			std::numeric_limits<u32>::max(),
			10);
	}();
	return limit;
}

inline void resetDebugTextureSampleLogSlots()
{
	if (!debugOverwriteLogIncludeTexelDetail())
		return;
	for (DebugTextureSampleLogEntry & entry : gDebugTextureSampleLogSlots)
		entry = DebugTextureSampleLogEntry{};
}

struct DebugOverwriteLogFilterConfig
{
	bool hasWorkOrdinalMin = false;
	u64 workOrdinalMin = 0ULL;
	bool hasWorkOrdinalMax = false;
	u64 workOrdinalMax = 0ULL;
	bool hasPacketMin = false;
	u64 packetMin = 0ULL;
	bool hasPacketMax = false;
	u64 packetMax = 0ULL;
	std::vector<u64> packetIds;
};

void parseOverwriteLogPacketIdList(const char * _raw, std::vector<u64> & _outPacketIds)
{
	if (_raw == nullptr || _raw[0] == '\0')
		return;

	const std::string value(_raw);
	size_t begin = 0U;
	while (begin <= value.size()) {
		const size_t comma = value.find(',', begin);
		const std::string token = trimAsciiWhitespace(
			comma == std::string::npos
				? value.substr(begin)
				: value.substr(begin, comma - begin));
		if (!token.empty()) {
			u64 parsed = 0ULL;
			if (parseUnsignedToken(token, parsed))
				_outPacketIds.push_back(parsed);
		}
		if (comma == std::string::npos)
			break;
		begin = comma + 1U;
	}
}

const DebugOverwriteLogFilterConfig & debugOverwriteLogFilterConfig()
{
	static const DebugOverwriteLogFilterConfig config = []() -> DebugOverwriteLogFilterConfig {
		DebugOverwriteLogFilterConfig parsed{};
		u64 value = 0ULL;
		if (parseEnvUnsigned(std::getenv("REALITYVK_RVK2_DEBUG_OVERWRITE_LOG_WORK_MIN"), value)) {
			parsed.hasWorkOrdinalMin = true;
			parsed.workOrdinalMin = value;
		}
		if (parseEnvUnsigned(std::getenv("REALITYVK_RVK2_DEBUG_OVERWRITE_LOG_WORK_MAX"), value)) {
			parsed.hasWorkOrdinalMax = true;
			parsed.workOrdinalMax = value;
		}
		if (parseEnvUnsigned(std::getenv("REALITYVK_RVK2_DEBUG_OVERWRITE_LOG_PACKET_MIN"), value)) {
			parsed.hasPacketMin = true;
			parsed.packetMin = value;
		}
		if (parseEnvUnsigned(std::getenv("REALITYVK_RVK2_DEBUG_OVERWRITE_LOG_PACKET_MAX"), value)) {
			parsed.hasPacketMax = true;
			parsed.packetMax = value;
		}
		parseOverwriteLogPacketIdList(
			std::getenv("REALITYVK_RVK2_DEBUG_OVERWRITE_LOG_PACKET_IDS"),
			parsed.packetIds);
		if (!parsed.packetIds.empty()) {
			std::sort(parsed.packetIds.begin(), parsed.packetIds.end());
			parsed.packetIds.erase(
				std::unique(parsed.packetIds.begin(), parsed.packetIds.end()),
				parsed.packetIds.end());
		}
		return parsed;
	}();
	return config;
}

bool debugOverwriteLogPassesFilters(u64 _workOrdinal, u64 _sourcePacketId)
{
	const DebugOverwriteLogFilterConfig & config = debugOverwriteLogFilterConfig();
	if (config.hasWorkOrdinalMin && (_workOrdinal < config.workOrdinalMin))
		return false;
	if (config.hasWorkOrdinalMax && (_workOrdinal > config.workOrdinalMax))
		return false;
	if (config.hasPacketMin && (_sourcePacketId < config.packetMin))
		return false;
	if (config.hasPacketMax && (_sourcePacketId > config.packetMax))
		return false;
	if (!config.packetIds.empty()
		&& !std::binary_search(config.packetIds.begin(), config.packetIds.end(), _sourcePacketId))
		return false;
	return true;
}

constexpr u8 kOverwriteFocusClusterNone = 0U;
constexpr u8 kOverwriteFocusClusterFill = 1U;
constexpr u8 kOverwriteFocusClusterTexRect = 2U;

inline bool isFocusFillStateCluster(const rvk2::RenderWorkPacket & _work)
{
	if (_work.opKind != static_cast<u8>(rvk2::RasterOpKind::kFillRect))
		return false;
	const u64 combine = static_cast<u64>(_work.combineMux);
	const u64 other = static_cast<u64>(_work.otherModes);
	return (combine == 0x00FFFFFFFFFE793CULL && other == 0x00380C7F00000000ULL)
		|| (combine == 0x00FFFFFFFFFCF87CULL && other == 0x00308C7F00000000ULL);
}

inline bool isFocusTexRectStateCluster(const rvk2::RenderWorkPacket & _work)
{
	if (_work.opKind != static_cast<u8>(rvk2::RasterOpKind::kTexRect))
		return false;
	const u64 combine = static_cast<u64>(_work.combineMux);
	const u64 other = static_cast<u64>(_work.otherModes);
	return (combine == 0x00FFFFFFFFFCF87CULL && other == 0x00208C7F00000000ULL)
		|| (combine == 0x00FFFFFFFFFCF87CULL && other == 0x00308C7F00000000ULL);
}

void appendOverwriteLog(
	u64 _workOrdinal,
	u64 _sourcePacketId,
	u8 _opKind,
	u32 _colorImageAddress,
	u32 _x,
	u32 _y,
	u32 _previousEncodedColor,
	u32 _newEncodedColor,
	u32 _textureColor,
	u32 _combinerColor,
	u32 _blenderColor,
	u32 _finalColor,
	u32 _textureSourceBits,
	bool _overwriteToBlack,
	bool _quantizedToBlack,
	bool _preservedNonBlack,
	bool _previousWriteMaskSet,
	u8 _focusCluster,
	bool _focusTexelRepeat,
	bool _focusTLUTRepeat,
	const rvk2::RenderWorkPacket & _work)
{
	const char * logPath = debugOverwriteLogPath();
	if (logPath == nullptr)
		return;

	static u32 emitted = 0U;
	const u32 limit = debugOverwriteLogLimit();
	if (limit != 0U && emitted >= limit)
		return;
	if (!debugOverwriteLogPassesFilters(_workOrdinal, _sourcePacketId))
		return;

	std::FILE * file = std::fopen(logPath, "ab");
	if (file == nullptr)
		return;

	const char * opName =
		_opKind == static_cast<u8>(rvk2::RasterOpKind::kTriangle)
			? "triangle"
			: (_opKind == static_cast<u8>(rvk2::RasterOpKind::kTexRect)
				? "texrect"
				: (_opKind == static_cast<u8>(rvk2::RasterOpKind::kFillRect) ? "fill" : "other"));
	std::fprintf(
		file,
		"frame=%llu\twork_ordinal=%llu\tsource_packet_id=%llu\top_kind=%u\top_name=%s\tphase=%u\tcolor_image=0x%08X\tx=%u\ty=%u\tprev=0x%08X\tnew=0x%08X\ttexel=0x%08X\tcombiner=0x%08X\tblender=0x%08X\tfinal=0x%08X\ttexture_source_bits=0x%08X\toverwrite_to_black=%u\tquantized_to_black=%u\tpreserved=%u\tprev_write_mask=%u\tfocus_cluster=%u\tfocus_texel_repeat=%u\tfocus_tlut_repeat=%u\tcombine_mux=0x%016llX\tother_modes=0x%016llX\tblend_params=0x%08X\ttile=%u\ttile_format=%u\ttile_size=%u\ttile_line=%u\ttile_tmem=%u\ttexture_image_width=%u\ttexture_image_address=0x%08X",
		static_cast<unsigned long long>(gActiveExecutorFrameId),
		static_cast<unsigned long long>(_workOrdinal),
		static_cast<unsigned long long>(_sourcePacketId),
		static_cast<unsigned>(_opKind),
		opName,
		static_cast<unsigned>(_work.phase),
		_colorImageAddress,
		_x,
		_y,
		_previousEncodedColor,
		_newEncodedColor,
		_textureColor,
		_combinerColor,
		_blenderColor,
		_finalColor,
		_textureSourceBits,
		_overwriteToBlack ? 1U : 0U,
		_quantizedToBlack ? 1U : 0U,
		_preservedNonBlack ? 1U : 0U,
		_previousWriteMaskSet ? 1U : 0U,
		static_cast<unsigned>(_focusCluster),
		_focusTexelRepeat ? 1U : 0U,
		_focusTLUTRepeat ? 1U : 0U,
		static_cast<unsigned long long>(_work.combineMux),
		static_cast<unsigned long long>(_work.otherModes),
		_work.blendParams,
		static_cast<unsigned>(_work.tile),
		static_cast<unsigned>(_work.tileFormat),
		static_cast<unsigned>(_work.tileSize),
		static_cast<unsigned>(_work.tileLine),
		static_cast<unsigned>(_work.tileTmem),
		static_cast<unsigned>(_work.textureImageWidth),
		_work.textureImageAddress);
	if (debugOverwriteLogIncludeTexelDetail()) {
		const auto appendSampleSlot = [&](const char * _prefix, const DebugTextureSampleLogEntry & _entry) {
			std::fprintf(
				file,
				"\t%s_valid=%u\t%s_source_kind=%u\t%s_sample_slot=%u\t%s_format=%u\t%s_size=%u\t%s_filter_mode=%u\t%s_lut_mode=%u\t%s_include_w=%u\t%s_needs_lut=%u\t%s_tmem_reject=%u\t%s_rdram_reject=%u\t%s_tmem_load_kind=%u\t%s_tmem_fetch_variant=%u\t%s_tile=%u\t%s_tile_line=%u\t%s_tile_tmem=%u\t%s_tile_palette=%u\t%s_texture_image_width=%u\t%s_tmem_offset=%u\t%s_tmem_s=%u\t%s_tmem_t=%u\t%s_tmem_i=%u\t%s_tmem_row_xor=%u\t%s_tmem_line_stride=%d\t%s_tmem_index_a=%u\t%s_tmem_index_b=%u\t%s_tmem_raw_a=0x%08X\t%s_tmem_raw_b=0x%08X\t%s_tmem_packed32=0x%08X\t%s_rdram_base=0x%08X\t%s_rdram_texel_index=%llu\t%s_rdram_word_address=0x%08X\t%s_rdram_raw=0x%08X\t%s_requested_s=%d\t%s_requested_t=%d\t%s_requested_w=%d\t%s_pixel_x=%u\t%s_pixel_y=%u\t%s_sampled_color=0x%08X\t%s_final_color=0x%08X\t%s_source_bits=0x%08X\t%s_rdram_probe_valid=%u\t%s_rdram_probe_needs_lut=%u\t%s_rdram_probe_reject=%u\t%s_rdram_probe_sampled_color=0x%08X\t%s_rdram_probe_final_color=0x%08X\t%s_rdram_probe_word_address=0x%08X\t%s_rdram_probe_raw=0x%08X",
				_prefix,
				_entry.valid ? 1U : 0U,
				_prefix,
				static_cast<unsigned>(_entry.sourceKind),
				_prefix,
				static_cast<unsigned>(_entry.sampleSlot),
				_prefix,
				static_cast<unsigned>(_entry.format),
				_prefix,
				static_cast<unsigned>(_entry.size),
				_prefix,
				static_cast<unsigned>(_entry.filterMode),
				_prefix,
				static_cast<unsigned>(_entry.lutMode),
				_prefix,
				static_cast<unsigned>(_entry.includeW),
				_prefix,
				static_cast<unsigned>(_entry.needsLUT),
				_prefix,
				static_cast<unsigned>(_entry.tmemReject),
				_prefix,
				static_cast<unsigned>(_entry.rdramReject),
				_prefix,
				static_cast<unsigned>(_entry.tmemLoadKind),
				_prefix,
				static_cast<unsigned>(_entry.tmemFetchVariant),
				_prefix,
				static_cast<unsigned>(_entry.tile),
				_prefix,
				static_cast<unsigned>(_entry.tileLine),
				_prefix,
				static_cast<unsigned>(_entry.tileTmem),
				_prefix,
				static_cast<unsigned>(_entry.tilePalette),
				_prefix,
				static_cast<unsigned>(_entry.textureImageWidth),
				_prefix,
				static_cast<unsigned>(_entry.tmemOffset),
				_prefix,
				static_cast<unsigned>(_entry.tmemS),
				_prefix,
				static_cast<unsigned>(_entry.tmemT),
				_prefix,
				static_cast<unsigned>(_entry.tmemI),
				_prefix,
				_entry.tmemRowXor,
				_prefix,
				_entry.tmemLineStride,
				_prefix,
				_entry.tmemIndexA,
				_prefix,
				_entry.tmemIndexB,
				_prefix,
				_entry.tmemRawA,
				_prefix,
				_entry.tmemRawB,
				_prefix,
				_entry.tmemPacked32,
				_prefix,
				_entry.rdramBaseAddress,
				_prefix,
				static_cast<unsigned long long>(_entry.rdramTexelIndex),
				_prefix,
				_entry.rdramWordAddress,
				_prefix,
				_entry.rdramRaw,
				_prefix,
				_entry.requestedS,
				_prefix,
				_entry.requestedT,
				_prefix,
				_entry.requestedW,
				_prefix,
				_entry.pixelX,
				_prefix,
				_entry.pixelY,
				_prefix,
					_entry.sampledColor,
					_prefix,
					_entry.finalColor,
					_prefix,
					_entry.sourceBits,
					_prefix,
					static_cast<unsigned>(_entry.rdramProbeValid),
					_prefix,
					static_cast<unsigned>(_entry.rdramProbeNeedsLUT),
					_prefix,
					static_cast<unsigned>(_entry.rdramProbeReject),
					_prefix,
					_entry.rdramProbeSampledColor,
					_prefix,
					_entry.rdramProbeFinalColor,
					_prefix,
					_entry.rdramProbeWordAddress,
					_prefix,
					_entry.rdramProbeRaw);
			std::fprintf(
				file,
				"\t%s_tmem_load_tile=%u\t%s_tmem_load_uls=%u\t%s_tmem_load_ult=%u\t%s_tmem_load_lrs=%u\t%s_tmem_load_lrt=%u\t%s_tmem_load_dxt=%u\t%s_tmem_load_span_texels=%u\t%s_tmem_load_qwords=%u\t%s_tmem_load_est_words_per_line=%u\t%s_tlut_applied=%u\t%s_tlut_addr=%u\t%s_tlut_raw=0x%04X\t%s_tlut_decoded=0x%04X",
				_prefix,
				static_cast<unsigned>(_entry.tmemLoadTile),
				_prefix,
				static_cast<unsigned>(_entry.tmemLoadULS),
				_prefix,
				static_cast<unsigned>(_entry.tmemLoadULT),
				_prefix,
				static_cast<unsigned>(_entry.tmemLoadLRS),
				_prefix,
				static_cast<unsigned>(_entry.tmemLoadLRT),
				_prefix,
				static_cast<unsigned>(_entry.tmemLoadDXT),
				_prefix,
				static_cast<unsigned>(_entry.tmemLoadSpanTexels),
				_prefix,
				static_cast<unsigned>(_entry.tmemLoadQwords),
				_prefix,
				static_cast<unsigned>(_entry.tmemLoadEstimatedWordsPerLine),
				_prefix,
				static_cast<unsigned>(_entry.tlutApplied),
				_prefix,
				static_cast<unsigned>(_entry.tlutLookupAddress),
				_prefix,
				static_cast<unsigned>(_entry.tlutRawEntry),
				_prefix,
				static_cast<unsigned>(_entry.tlutDecodedEntry));
		};
		appendSampleSlot("tex0", gDebugTextureSampleLogSlots[rvk2::kExecutorTextureSampleSlotTexel0]);
		appendSampleSlot("tex1", gDebugTextureSampleLogSlots[rvk2::kExecutorTextureSampleSlotTexel1]);
		appendSampleSlot("tex0_next", gDebugTextureSampleLogSlots[rvk2::kExecutorTextureSampleSlotTexel0Next]);
	}
	std::fputc('\n', file);
	std::fclose(file);
	++emitted;
}

u32 debugTriangleSampleYSubpixelBias()
{
	static const u32 bias = []() -> u32 {
		return rvk2::envU32Clamped("REALITYVK_RVK2_DEBUG_TRIANGLE_SAMPLE_Y_SUBPIXEL_BIAS", 2U, 3U, 10);
	}();
	return bias;
}

u32 debugTriangleSampleXSubpixelBias()
{
	static const u32 bias = []() -> u32 {
		return rvk2::envU32Clamped(
			"REALITYVK_RVK2_DEBUG_TRIANGLE_SAMPLE_X_SUBPIXEL_BIAS",
			0x8000U,
			0xFFFFU,
			10);
	}();
	return bias;
}

bool shouldQuantizeColorImage16Surface()
{
	return debugForceColorImage16Quantize()
		&& !debugDisableColorImage16Quantize();
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
	if (_colorImageSize != 2U || !shouldQuantizeColorImage16Surface())
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

inline u16 encodeSurfaceColor16Raw(u32 _rgba)
{
	const u8 r = static_cast<u8>((_rgba >> 24U) & 0xFFU);
	const u8 g = static_cast<u8>((_rgba >> 16U) & 0xFFU);
	const u8 b = static_cast<u8>((_rgba >> 8U) & 0xFFU);
	const u8 a = static_cast<u8>(_rgba & 0xFFU);
	const u16 r5 = static_cast<u16>((static_cast<u32>(r) * 31U + 127U) / 255U);
	const u16 g5 = static_cast<u16>((static_cast<u32>(g) * 31U + 127U) / 255U);
	const u16 b5 = static_cast<u16>((static_cast<u32>(b) * 31U + 127U) / 255U);
	const u16 a1 = a >= 128U ? 1U : 0U;
	return static_cast<u16>((r5 << 11U) | (g5 << 6U) | (b5 << 1U) | a1);
}

inline bool pixelHasVisibleColor(u32 _encodedColor)
{
	return ((_encodedColor >> 8U) & 0x00FFFFFFU) != 0U;
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

inline u32 dzCompress16(u32 _value)
{
	u32 j = 0U;
	if ((_value & 0xFF00U) != 0U)
		j |= 8U;
	if ((_value & 0xF0F0U) != 0U)
		j |= 4U;
	if ((_value & 0xCCCCU) != 0U)
		j |= 2U;
	if ((_value & 0xAAAAU) != 0U)
		j |= 1U;
	return j;
}

inline u32 estimateNoZDepthDeltaEncoding(const rvk2::RenderWorkPacket & _work)
{
	u32 dz = static_cast<u32>(_work.primDepthDelta);
	if (dz == 0U)
		dz = 1U;
	dz = std::min<u32>(dz, 0xFFFFU);
	return dzCompress16(dz);
}

inline u32 noZBlendMemoryAlphaShiftB(const rvk2::RenderWorkPacket & _work)
{
	const u32 dzpixenc = estimateNoZDepthDeltaEncoding(_work);
	return dzpixenc < 0xBU ? 4U : (0xFU - dzpixenc);
}

inline u8 memoryAlphaFromCoverage3(u8 _coverage3)
{
	return static_cast<u8>((static_cast<u32>(_coverage3 & 0x7U) << 5U) & 0xE0U);
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

struct TileLocalTexelCoord
{
	s32 s = 0;
	s32 t = 0;
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
	// mask=0 with clamp disabled should free-run coordinates in TMEM address space.

	mapped.texel = tileBase + localTexel;
	return mapped;
}

inline s32 decodeTileTexelBase(u16 _lo, u16 _hi)
{
	return static_cast<s32>(std::min<u16>(_lo, _hi) >> 2U);
}

inline TileLocalTexelCoord computeTileLocalTexelCoord(
	const rvk2::RenderWorkPacket & _work,
	s32 _s,
	s32 _t)
{
	TileLocalTexelCoord local{};
	local.s = _s - decodeTileTexelBase(_work.tileULS, _work.tileLRS);
	local.t = _t - decodeTileTexelBase(_work.tileULT, _work.tileLRT);
	return local;
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
	u32 _rgba,
	DebugTextureSampleLogEntry * _debug = nullptr)
{
	(void)_seed;
	if (debugDisableTextureLUTApply())
		return _rgba;
	const u8 lutMode = decodeTextureLUTMode(_work);
	if (lutMode == 0U)
		return _rgba;

	// CI decode stores index in RGB lanes.
	const u8 index = static_cast<u8>((_rgba >> 24U) & 0xFFU);
	const u16 * tmem16 = reinterpret_cast<const u16 *>(activeTMEMWords());
	const u32 tlutAddr =
		(0x400U
			+ ((static_cast<u32>(index) << 2U) + debugTLUTLookupOffset()))
		& 0x7FFU;
	const u16 tlut = tmem16[tlutAddr];
	if (_debug != nullptr) {
		_debug->tlutApplied = 1U;
		_debug->tlutLookupAddress = static_cast<u16>(tlutAddr);
		_debug->tlutRawEntry = tlut;
	}
	if (lutMode == 3U) {
		// IA16 TLUT entries follow A:I byte order.
		const u8 a = static_cast<u8>((tlut >> 8U) & 0xFFU);
		const u8 i = static_cast<u8>(tlut & 0xFFU);
		if (_debug != nullptr)
			_debug->tlutDecodedEntry = tlut;
		return (static_cast<u32>(i) << 24U)
			| (static_cast<u32>(i) << 16U)
			| (static_cast<u32>(i) << 8U)
			| static_cast<u32>(a);
	}

	// RGBA16 TLUT entries in TMEM use swapped 16-bit word order.
	u16 tlutRgba = static_cast<u16>((tlut << 8U) | (tlut >> 8U));
	if (debugDisableTLUTRGBA16Swap())
		tlutRgba = tlut;
	if (_debug != nullptr)
		_debug->tlutDecodedEntry = tlutRgba;
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

inline void writeRdramByteWrapped(u32 _address, u8 _value)
{
	if (!rdramReadable())
		return;
	RDRAM[(_address & RDRAMSize) ^ 3U] = _value;
}

inline u16 readRdramU16Wrapped(u32 _address)
{
	const u8 hi = readRdramByteWrapped(_address);
	const u8 lo = readRdramByteWrapped(_address + 1U);
	return static_cast<u16>((static_cast<u16>(hi) << 8U) | static_cast<u16>(lo));
}

inline void writeRdramU16Wrapped(u32 _address, u16 _value)
{
	writeRdramByteWrapped(_address + 0U, static_cast<u8>((_value >> 8U) & 0xFFU));
	writeRdramByteWrapped(_address + 1U, static_cast<u8>(_value & 0xFFU));
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

inline void writeRdramU32Wrapped(u32 _address, u32 _value)
{
	writeRdramByteWrapped(_address + 0U, static_cast<u8>((_value >> 24U) & 0xFFU));
	writeRdramByteWrapped(_address + 1U, static_cast<u8>((_value >> 16U) & 0xFFU));
	writeRdramByteWrapped(_address + 2U, static_cast<u8>((_value >> 8U) & 0xFFU));
	writeRdramByteWrapped(_address + 3U, static_cast<u8>(_value & 0xFFU));
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

inline void writeColorImagePixelToRdram(
	const rvk2::RenderWorkPacket & _work,
	u32 _x,
	u32 _y,
	u32 _encodedColor)
{
	if (!rdramReadable())
		return;

	const u8 colorImageSize = static_cast<u8>(_work.colorImageSize & 0x3U);
	const u32 colorImageWidth = _work.colorImageWidth != 0U
		? static_cast<u32>(_work.colorImageWidth)
		: 1U;
	const u32 colorImageBase = _work.colorImageAddress & 0x00FFFFFFU;
	const u64 pixelOffset =
		static_cast<u64>(_y) * static_cast<u64>(colorImageWidth)
		+ static_cast<u64>(_x);

	if (colorImageSize == 2U) {
		const u32 byteOffset = static_cast<u32>((pixelOffset & 0xFFFFFFFFULL) << 1U);
		const u16 packed16 = encodeSurfaceColor16Raw(_encodedColor);
		writeRdramU16Wrapped(colorImageBase + byteOffset, packed16);
		return;
	}

	if (colorImageSize == 3U) {
		const u32 byteOffset = static_cast<u32>((pixelOffset & 0xFFFFFFFFULL) << 2U);
		writeRdramU32Wrapped(colorImageBase + byteOffset, _encodedColor);
	}
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
	u8 & _outRejectReason,
	DebugTextureSampleLogEntry * _debug = nullptr)
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
	if (_debug != nullptr) {
		_debug->format = format;
		_debug->size = size;
		_debug->lutMode = lutMode;
		_debug->textureImageWidth = imageWidth;
		_debug->rdramBaseAddress = baseAddress;
		_debug->rdramTexelIndex = texelIndex;
	}

	_outNeedsLUT = false;
	switch (size) {
	case 0U: { // 4b
		const u32 byteAddress = baseAddress + static_cast<u32>(texelIndex >> 1U);
		const u8 packed = readRdramByteWrapped(byteAddress);
		const bool lowNibble = (texelIndex & 1ULL) != 0ULL;
		const u8 value4 = lowNibble
			? static_cast<u8>(packed & 0x0FU)
			: static_cast<u8>((packed >> 4U) & 0x0FU);
		if (_debug != nullptr) {
			_debug->tmemFetchVariant = kDebugTexFetchVariantRDRAM4;
			_debug->rdramWordAddress = byteAddress;
			_debug->rdramRaw = static_cast<u32>(packed);
		}
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
		const u32 byteAddress = baseAddress + static_cast<u32>(texelIndex);
		const u8 value8 = readRdramByteWrapped(byteAddress);
		if (_debug != nullptr) {
			_debug->tmemFetchVariant = kDebugTexFetchVariantRDRAM8;
			_debug->rdramWordAddress = byteAddress;
			_debug->rdramRaw = static_cast<u32>(value8);
		}
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
		const u32 texelAddress = baseAddress + static_cast<u32>(texelIndex << 1U);
		const u16 value16 = readRdramU16Wrapped(texelAddress);
		if (_debug != nullptr) {
			_debug->tmemFetchVariant = kDebugTexFetchVariantRDRAM16;
			_debug->rdramWordAddress = texelAddress;
			_debug->rdramRaw = static_cast<u32>(value16);
		}
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
			if (_debug != nullptr) {
				_debug->rdramWordAddress = pairAddress;
				_debug->rdramRaw =
					(static_cast<u32>(u) << 24U)
					| (static_cast<u32>(y0) << 16U)
					| (static_cast<u32>(v) << 8U)
					| static_cast<u32>(y1);
			}
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
		const u32 texelAddress = baseAddress + static_cast<u32>(texelIndex << 2U);
		const u32 value32 = readRdramU32Wrapped(texelAddress);
		if (_debug != nullptr) {
			_debug->tmemFetchVariant = kDebugTexFetchVariantRDRAM32;
			_debug->rdramWordAddress = texelAddress;
			_debug->rdramRaw = value32;
		}
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

inline u8 readTmem8BitColorWithXor(u16 _offset, u16 _x, u32 _rowXor)
{
	const u8 * tmem8 = reinterpret_cast<const u8 *>(activeTMEMWords());
	return tmem8[((static_cast<u32>(_offset) << 3U) + (static_cast<u32>(_x) ^ _rowXor)) & 0xFFFU];
}

inline u8 readTmem8BitColor(u16 _offset, u16 _x, u16 _i)
{
	const u32 oddRowXor =
		debugAltTmem8OddXor()
			? static_cast<u32>(_i)
			: (static_cast<u32>(_i) << 1U);
	return readTmem8BitColorWithXor(_offset, _x, oddRowXor);
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

inline u16 estimateLoadBlockWordsPerLine(u16 _dxt)
{
	if (_dxt == 0U)
		return 0U;
	return static_cast<u16>((2048U + static_cast<u32>(_dxt) - 1U) / static_cast<u32>(_dxt));
}

inline u16 computeLoadSpanTexels(const rvk2::RenderWorkPacket & _work)
{
	const u32 lrs = static_cast<u32>(_work.tmemLoadLRS);
	const u32 uls = static_cast<u32>(_work.tmemLoadULS);
	return static_cast<u16>(((lrs - uls + 1U) & 0x0FFFU));
}

inline u16 computeLoadQwordsEstimate(const rvk2::RenderWorkPacket & _work)
{
	if (_work.tmemLoadKind != static_cast<u8>(rvk2::TmemLoadKind::kBlock))
		return 0U;
	const u16 spanTexels = computeLoadSpanTexels(_work);
	if (spanTexels == 0U)
		return 0U;
	const u8 loadSize = _work.textureImageSize & 0x3U;
	u32 bytes = (static_cast<u32>(spanTexels) << loadSize) >> 1U;
	if ((bytes & 7U) != 0U)
		bytes = (bytes & ~7U) + 8U;
	return static_cast<u16>((bytes >> 3U) & 0xFFFFU);
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

inline u32 tmem8RowXorFromLoadKind(
	const rvk2::RenderWorkPacket & _work,
	u16 _t,
	u16 _i)
{
	// Match GLideN64 TMEM fetch rules: row parity drives the byte-lane XOR for
	// 8b samples regardless of which load opcode last touched TMEM.
	(void)_work;
	(void)_t;
	return static_cast<u32>(_i) << 1U;
}

inline u32 tmem32RowXorFromLoadKind(
	const rvk2::RenderWorkPacket & _work,
	u16 _t)
{
	// 32b TMEM split fetches also use row parity XOR independent of load kind.
	(void)_work;
	return xorForTmem32T(_t);
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

inline u32 readTmem32CanonicalPacked(const rvk2::RenderWorkPacket & _work, u16 _s, u16 _t, u32 _xor)
{
	const u16 * tmem16 = reinterpret_cast<const u16 *>(activeTMEMWords());
	const u32 tbase =
		(static_cast<u32>(_work.tileLine) * static_cast<u32>(_t & 0x00FFU))
		+ static_cast<u32>(_work.tileTmem & 0x1FFU);
	const u32 taddr = (((tbase << 2U) + static_cast<u32>(_s)) ^ _xor) & 0x3FFU;
	const u16 gr = tmem16[taddr];
	const u16 ab = tmem16[taddr | 0x400U];
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
	u16 _t,
	DebugTextureSampleLogEntry * _debug = nullptr)
{
	const bool highToLowRGBA = debugTmem32PackHighToLowRGBA();
	u32 rowXor = tmem32RowXorFromLoadKind(_work, _t);
	if (debugTmem32UseLoadKindAwareXor()) {
		rowXor = tmem32RowXorFromLoadKind(_work, _t);
	}
	if (_debug != nullptr)
		_debug->tmemRowXor = rowXor;

	if (debugTmem32UseDirectLinearFetch()) {
		const u32 * tmem32 = reinterpret_cast<const u32 *>(activeTMEMWords());
		const u16 i = static_cast<u16>((_t & 1U) << 1U);
		const u16 tmemOffset = static_cast<u16>(
			(_work.tileTmem + static_cast<u16>(_work.tileLine * _t)) & 0x1FFU);
		const u32 taddr = ((static_cast<u32>(tmemOffset) << 1U)
			+ (static_cast<u32>(_s) ^ static_cast<u32>(i)))
			& 0x3FFU;
		const u32 packed = tmem32[taddr];
		if (_debug != nullptr) {
			_debug->tmemFetchVariant = kDebugTexFetchVariantTMEM32Direct;
			_debug->tmemOffset = tmemOffset;
			_debug->tmemI = i;
			_debug->tmemIndexA = taddr;
			_debug->tmemPacked32 = packed;
			_debug->tmemRawA = packed;
		}
		return packSplit32ToRGBA(packed, highToLowRGBA);
	}

	if (debugTmem32UseCanonicalFetch()) {
		const u16 * tmem16 = reinterpret_cast<const u16 *>(activeTMEMWords());
		const u32 tbase =
			(static_cast<u32>(_work.tileLine) * static_cast<u32>(_t & 0x00FFU))
			+ static_cast<u32>(_work.tileTmem & 0x1FFU);
		const u32 taddr = (((tbase << 2U) + static_cast<u32>(_s)) ^ rowXor) & 0x3FFU;
		const u16 gr = tmem16[taddr];
		const u16 ab = tmem16[taddr | 0x400U];
		const u32 packed = (static_cast<u32>(ab) << 16U) | static_cast<u32>(gr);
		if (_debug != nullptr) {
			_debug->tmemFetchVariant = kDebugTexFetchVariantTMEM32Canonical;
			_debug->tmemIndexA = taddr;
			_debug->tmemIndexB = taddr | 0x400U;
			_debug->tmemRawA = static_cast<u32>(gr);
			_debug->tmemRawB = static_cast<u32>(ab);
			_debug->tmemPacked32 = packed;
		}
		return packSplit32ToRGBA(packed, highToLowRGBA);
	}

	// Default 32b TMEM path follows the legacy loader addressing:
	// split GR/AB words, odd/even row XOR, and line32 stride derived from tile span.
	const s32 lineStride = computeLegacySplit32LineStride(_work);
	if (_debug != nullptr)
		_debug->tmemLineStride = lineStride;
	const u16 * tmem16 = reinterpret_cast<const u16 *>(activeTMEMWords());
	const s32 tline =
		(static_cast<s32>(_work.tileTmem & 0x1FFU) << 2)
		+ lineStride * static_cast<s32>(_t);
	const u32 taddr = (static_cast<u32>(tline + static_cast<s32>(_s)) ^ rowXor) & 0x3FFU;
	const u16 grRaw = tmem16[taddr];
	const u16 abRaw = tmem16[taddr | 0x400U];
	const u16 gr = swapU16(grRaw);
	const u16 ab = swapU16(abRaw);
	const u32 packed = (static_cast<u32>(ab) << 16U) | static_cast<u32>(gr);
	if (_debug != nullptr) {
		_debug->tmemFetchVariant = kDebugTexFetchVariantTMEM32Split;
		_debug->tmemIndexA = taddr;
		_debug->tmemIndexB = taddr | 0x400U;
		_debug->tmemRawA = static_cast<u32>(grRaw);
		_debug->tmemRawB = static_cast<u32>(abRaw);
		_debug->tmemPacked32 = packed;
	}
	return packSplit32ToRGBA(packed, highToLowRGBA);
}

inline u32 decodeTMEM32SplitVariantColor(
	const rvk2::RenderWorkPacket & _work,
	u16 _s,
	u16 _t,
	u32 _xor,
	bool _highToLowRGBA)
{
	const s32 lineStride = computeLegacySplit32LineStride(_work);
	return packSplit32ToRGBA(
		readTmem32SplitPacked(_work, _s, _t, lineStride, _xor),
		_highToLowRGBA);
}

inline u32 decodeTMEM32DirectVariantColor(
	const rvk2::RenderWorkPacket & _work,
	u16 _s,
	u16 _t,
	u16 _rowXor,
	bool _highToLowRGBA)
{
	const u32 * tmem32 = reinterpret_cast<const u32 *>(activeTMEMWords());
	const u16 tmemOffset = static_cast<u16>(
		(_work.tileTmem + static_cast<u16>(_work.tileLine * _t)) & 0x1FFU);
	const u32 packed = tmem32[
		((static_cast<u32>(tmemOffset) << 1U)
			+ (static_cast<u32>(_s) ^ static_cast<u32>(_rowXor)))
		& 0x3FFU];
	return packSplit32ToRGBA(packed, _highToLowRGBA);
}

inline void recordTMEM32AlternateComparisons(
	const rvk2::RenderWorkPacket & _work,
	s32 _sRaw,
	s32 _tRaw,
	u32 _authoritativeRGBA)
{
	if (gActiveExecutorSummary == nullptr || !debugTmem32CompareAlternates())
		return;

	rvk2::ExecutorSummary & summary = *gActiveExecutorSummary;
	++summary.textureTmem32CompareCount;

	const bool highToLowRGBA = debugTmem32PackHighToLowRGBA();
	const u16 s = static_cast<u16>(_sRaw & 0xFFFF);
	const u16 t = static_cast<u16>(_tRaw & 0xFFFF);
	const u16 sAbs = static_cast<u16>(std::min<s64>(absS64(static_cast<s64>(_sRaw)), 0xFFFF));
	const u16 tAbs = static_cast<u16>(std::min<s64>(absS64(static_cast<s64>(_tRaw)), 0xFFFF));

	bool anyMismatch = false;
	const auto checkVariant = [&](u32 _altColor, u64 & _counter) {
		if (_altColor == _authoritativeRGBA)
			return;
		++_counter;
		anyMismatch = true;
	};

	checkVariant(
		decodeTMEM32SplitVariantColor(_work, s, t, 0U, highToLowRGBA),
		summary.textureTmem32AltNoXorMismatchCount);
	checkVariant(
		decodeTMEM32SplitVariantColor(_work, sAbs, tAbs, xorForTmem32T(tAbs), highToLowRGBA),
		summary.textureTmem32AltAbsCoordMismatchCount);
	checkVariant(
		decodeTMEM32DirectVariantColor(_work, s, t, static_cast<u16>((t & 1U) << 1U), highToLowRGBA),
		summary.textureTmem32AltDirectMismatchCount);
	checkVariant(
		decodeTMEM32DirectVariantColor(_work, s, t, static_cast<u16>((t & 1U) << 1U), !highToLowRGBA),
		summary.textureTmem32AltDirectSwappedMismatchCount);
	const u32 tileLineXor = (_work.tileLine & 1U) != 0U
		? ((t & 1U) != 0U ? 1U : 3U)
		: xorForTmem32T(t);
	checkVariant(
		decodeTMEM32SplitVariantColor(_work, s, t, tileLineXor, highToLowRGBA),
		summary.textureTmem32AltTileLineXorMismatchCount);
	const u16 tileLineEvenOddXor = (_work.tileLine & 1U) != 0U
		? static_cast<u16>(t & 1U)
		: static_cast<u16>((t & 1U) << 1U);
	checkVariant(
		decodeTMEM32DirectVariantColor(_work, s, t, tileLineEvenOddXor, highToLowRGBA),
		summary.textureTmem32AltTileLineEvenOddMismatchCount);
	checkVariant(
		decodeTMEM32DirectVariantColor(_work, s, t, static_cast<u16>(t & 1U), highToLowRGBA),
		summary.textureTmem32AltDirectEvenOddMismatchCount);
	const u32 loadKindAwareXor =
		_work.tmemLoadKind == static_cast<u8>(rvk2::TmemLoadKind::kBlock)
			? xorForTmem32T(t)
			: ((_work.tmemLoadKind == static_cast<u8>(rvk2::TmemLoadKind::kTile))
				? 0U
				: xor13ForT(t));
	checkVariant(
		decodeTMEM32SplitVariantColor(_work, s, t, loadKindAwareXor, highToLowRGBA),
		summary.textureTmem32AltLoadKindAwareMismatchCount);

	if (anyMismatch)
		++summary.textureTmem32CompareMismatchCount;
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
	u8 & _outRejectReason,
	DebugTextureSampleLogEntry * _debug = nullptr)
{
	_outRejectReason = 0U;
	const u8 format = (_work.tileFormat & 0x7U) <= 4U
		? (_work.tileFormat & 0x7U)
		: (_work.textureImageFormat & 0x7U);
	const u8 size = _work.tileSize & 0x3U;
	// TMEM addressing is relative to tile-local coordinates. Using absolute
	// tile-space texels here skews row stride and odd/even row selection.
	const TileLocalTexelCoord localTexel = computeTileLocalTexelCoord(_work, _s, _t);
	const s32 texelS = localTexel.s;
	const s32 texelT = localTexel.t;

	const u8 lutMode = decodeTextureLUTMode(_work);
	const u32 tMemMask = lutMode == 0U ? 0x1FFU : 0xFFU;
	const u16 t = static_cast<u16>(texelT & 0xFFFF);
	const u16 s = static_cast<u16>(texelS & 0xFFFF);
	const u16 tmemOffset = static_cast<u16>(
		(_work.tileTmem + static_cast<u16>(_work.tileLine * t)) & tMemMask);
	const u16 i = static_cast<u16>((t & 1U) << 1U);
	if (_debug != nullptr) {
		_debug->format = format;
		_debug->size = size;
		_debug->lutMode = lutMode;
		_debug->tile = _work.tile;
		_debug->tileLine = _work.tileLine;
		_debug->tileTmem = _work.tileTmem;
		_debug->tilePalette = _work.tilePalette;
		_debug->textureImageWidth = _work.textureImageWidth;
		_debug->tmemOffset = tmemOffset;
		_debug->tmemS = s;
		_debug->tmemT = t;
		_debug->tmemI = i;
		_debug->tmemLoadKind = _work.tmemLoadKind;
	}

	_outNeedsLUT = false;
	switch (size) {
		case 0U: { // 4b
			const u8 * tmem8 = reinterpret_cast<const u8 *>(activeTMEMWords());
			const u32 byteIndex = ((static_cast<u32>(tmemOffset) << 3U)
				+ (((static_cast<u32>(s) >> 1U) ^ (static_cast<u32>(i) << 1U))))
				& 0xFFFU;
			const u8 packed = tmem8[byteIndex];
			if (_debug != nullptr) {
				_debug->tmemFetchVariant = kDebugTexFetchVariantTMEM4;
				_debug->tmemIndexA = byteIndex;
				_debug->tmemRawA = static_cast<u32>(packed);
			}
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
		u32 oddRowXor = tmem8RowXorFromLoadKind(_work, t, i);
		if (debugAltTmem8OddXor())
			oddRowXor = static_cast<u32>(i);
		if (debugTmem8UseXor13())
			oddRowXor = (t & 1U) != 0U ? 3U : 1U;
		if (debugTmem8UseLoadKindAwareXor())
			oddRowXor = tmem8RowXorFromLoadKind(_work, t, i);
			const u8 * tmem8 = reinterpret_cast<const u8 *>(activeTMEMWords());
			const u32 byteIndex =
				((static_cast<u32>(tmemOffset) << 3U) + (static_cast<u32>(s) ^ oddRowXor)) & 0xFFFU;
			const u8 value8 = tmem8[byteIndex];
			if (_debug != nullptr) {
				_debug->tmemFetchVariant = kDebugTexFetchVariantTMEM8;
				_debug->tmemRowXor = oddRowXor;
				_debug->tmemIndexA = byteIndex;
				_debug->tmemRawA = static_cast<u32>(value8);
			}
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
			const u16 * tmem16 = reinterpret_cast<const u16 *>(activeTMEMWords());
			const u32 index16 =
				((static_cast<u32>(tmemOffset) << 2U) + (static_cast<u32>(s) ^ static_cast<u32>(i))) & 0x7FFU;
			const u16 raw16 = tmem16[index16];
			u16 value16 = raw16;
			if (debugSwapTmem16Samples())
				value16 = static_cast<u16>((value16 << 8U) | (value16 >> 8U));
			if (_debug != nullptr) {
				_debug->tmemFetchVariant = kDebugTexFetchVariantTMEM16;
				_debug->tmemIndexA = index16;
				_debug->tmemRawA = static_cast<u32>(raw16);
				_debug->tmemRawB = static_cast<u32>(value16);
			}
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
			_outRgba = decodeAuthoritativeTMEM32Color(_work, s, t, _debug);
			recordTMEM32AlternateComparisons(_work, texelS, texelT, _outRgba);
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
	const bool captureSampleDetail =
		debugOverwriteLogIncludeTexelDetail()
		&& _sampleSlot < rvk2::kExecutorTextureSampleSlotBuckets;
	DebugTextureSampleLogEntry * sampleDetail =
		captureSampleDetail ? &gDebugTextureSampleLogSlots[_sampleSlot] : nullptr;
	if (sampleDetail != nullptr) {
		*sampleDetail = DebugTextureSampleLogEntry{};
		sampleDetail->sampleSlot = _sampleSlot;
		sampleDetail->requestedS = _s;
		sampleDetail->requestedT = _t;
		sampleDetail->requestedW = _w;
		sampleDetail->pixelX = _x;
		sampleDetail->pixelY = _y;
		sampleDetail->includeW = _includeW ? 1U : 0U;
		sampleDetail->lutMode = decodeTextureLUTMode(_work);
		sampleDetail->format = effectiveTextureFormat(_work);
		sampleDetail->size = effectiveTextureSize(_work);
		sampleDetail->filterMode =
			_work.phase == static_cast<u8>(rvk2::RenderPhase::kCopy)
				? 0U
				: static_cast<u8>(decodeTextureFilterMode(_work) & 0x3U);
		sampleDetail->tile = _work.tile;
		sampleDetail->tileLine = _work.tileLine;
		sampleDetail->tileTmem = _work.tileTmem;
		sampleDetail->tilePalette = _work.tilePalette;
		sampleDetail->textureImageWidth = _work.textureImageWidth;
		sampleDetail->tmemLoadKind = _work.tmemLoadKind;
		sampleDetail->tmemLoadTile = _work.tmemLoadTile;
		sampleDetail->tmemLoadULS = _work.tmemLoadULS;
		sampleDetail->tmemLoadULT = _work.tmemLoadULT;
		sampleDetail->tmemLoadLRS = _work.tmemLoadLRS;
		sampleDetail->tmemLoadLRT = _work.tmemLoadLRT;
		sampleDetail->tmemLoadDXT = _work.tmemLoadDXT;
		sampleDetail->tmemLoadSpanTexels = computeLoadSpanTexels(_work);
		sampleDetail->tmemLoadQwords = computeLoadQwordsEstimate(_work);
		sampleDetail->tmemLoadEstimatedWordsPerLine =
			estimateLoadBlockWordsPerLine(_work.tmemLoadDXT);
	}

	const auto commitSampleDetail = [&](
		u8 _sourceKind,
		u32 _sampledColor,
		u32 _finalColor,
		bool _needsLUT,
		u8 _tmemReject,
		u8 _rdramReject) {
		if (sampleDetail == nullptr)
			return;
		sampleDetail->valid = true;
		sampleDetail->sourceKind = _sourceKind;
		sampleDetail->sampledColor = _sampledColor;
		sampleDetail->finalColor = _finalColor;
		sampleDetail->needsLUT = _needsLUT ? 1U : 0U;
		sampleDetail->tmemReject = _tmemReject;
		sampleDetail->rdramReject = _rdramReject;
		if (_sourceBits != nullptr)
			sampleDetail->sourceBits = *_sourceBits;
		else if (_sourceKind == kDebugTexSampleSourceReplacement)
			sampleDetail->sourceBits = kTexelSourceReplacementBit;
		else if (_sourceKind == kDebugTexSampleSourceTMEM)
			sampleDetail->sourceBits = kTexelSourceTMEMBit;
		else if (_sourceKind == kDebugTexSampleSourceRDRAM)
			sampleDetail->sourceBits = kTexelSourceRdramBit;
		else if (_sourceKind == kDebugTexSampleSourceSynthetic)
			sampleDetail->sourceBits = kTexelSourceSyntheticBit;
	};

	if (const rvk2::TextureReplacementImage * replacement =
			findTextureReplacementImage(_work, _s, _t, _w, _includeW)) {
		if (_sourceBits != nullptr)
			*_sourceBits |= kTexelSourceReplacementBit;
		const u32 replacementColor =
			rvk2::sampleTextureReplacementImage(*replacement, _s, _t);
		const u32 finalReplacementColor =
			debugTextureBucketMaskAllowsSample(_work, lutModeEnabled)
				? replacementColor
				: 0x000000FFU;
		commitSampleDetail(
			kDebugTexSampleSourceReplacement,
			replacementColor,
			finalReplacementColor,
			false,
			0U,
			0U);
		return finalReplacementColor;
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
	u8 rdramReject = 0U;
	const bool copyCI8RdramProbe =
		debugForceCopyCI8RdramPrimary()
		&& _work.phase == static_cast<u8>(rvk2::RenderPhase::kCopy)
		&& effectiveTextureFormat(_work) == 2U
		&& effectiveTextureSize(_work) == 1U;
	// CI+TLUT path: prefer TMEM as the canonical source, but keep an automatic
	// RDRAM fallback when TMEM resolves to black so we preserve bring-up safety.
	const bool ciLutHybridFallback =
		decodeTextureLUTMode(_work) != 0U
		&& effectiveTextureFormat(_work) == 2U
		&& effectiveTextureSize(_work) <= 1U;
	const bool forceRdramPrimary =
		debugForceTextureRdramPrimary()
		|| copyCI8RdramProbe;
	const bool fallbackRdramIfTmemBlack =
		(debugTexelFallbackRdramIfTmemBlack() || ciLutHybridFallback)
		&& !forceRdramPrimary;

	const auto finalizeTextureSample = [&](u32 _rawColor, bool _needsLUT, u32 _sourceBit, u8 _sourceKind) -> u32 {
		recordTextureSampleMode(_work, _needsLUT, _sampleSlot);
		if (_sourceBits != nullptr)
			*_sourceBits |= _sourceBit;
		u32 color = _rawColor;
		if (_needsLUT) {
			if (gActiveExecutorSummary != nullptr)
				++gActiveExecutorSummary->textureLUTSampleCount;
			color = applyTextureLUTModeColor(_work, seed, color, sampleDetail);
		}
		if (debugForceAllTexelAlphaOpaque())
			color = (color & 0xFFFFFF00U) | 0x000000FFU;
		color = applyTextureDetailModeColor(_work, seed, color);
		if (!debugTextureBucketMaskAllowsSample(_work, _needsLUT))
			color = 0x000000FFU;
		commitSampleDetail(_sourceKind, _rawColor, color, _needsLUT, tmemReject, rdramReject);
		return color;
	};

	const auto captureRdramProbeForTMEM = [&]() {
		if (sampleDetail == nullptr)
			return;
		u32 probeColor = 0U;
		bool probeNeedsLUT = false;
		u8 probeReject = 0U;
		DebugTextureSampleLogEntry probeDetail{};
		if (!sampleTextureFromRDRAM(
				_work,
				_s,
				_t,
				probeColor,
				probeNeedsLUT,
				probeReject,
				&probeDetail)) {
			sampleDetail->rdramProbeValid = 0U;
			sampleDetail->rdramProbeNeedsLUT = 0U;
			sampleDetail->rdramProbeReject = probeReject;
			sampleDetail->rdramProbeSampledColor = 0U;
			sampleDetail->rdramProbeFinalColor = 0U;
			sampleDetail->rdramProbeWordAddress = probeDetail.rdramWordAddress;
			sampleDetail->rdramProbeRaw = probeDetail.rdramRaw;
			return;
		}

		u32 probeFinal = probeColor;
		if (probeNeedsLUT)
			probeFinal = applyTextureLUTModeColor(_work, seed, probeFinal);
		if (debugForceAllTexelAlphaOpaque())
			probeFinal = (probeFinal & 0xFFFFFF00U) | 0x000000FFU;
		probeFinal = applyTextureDetailModeColor(_work, seed, probeFinal);
		sampleDetail->rdramProbeValid = 1U;
		sampleDetail->rdramProbeNeedsLUT = probeNeedsLUT ? 1U : 0U;
		sampleDetail->rdramProbeReject = 0U;
		sampleDetail->rdramProbeSampledColor = probeColor;
		sampleDetail->rdramProbeFinalColor = probeFinal;
		sampleDetail->rdramProbeWordAddress = probeDetail.rdramWordAddress;
		sampleDetail->rdramProbeRaw = probeDetail.rdramRaw;
	};

	const auto tryTMEMSample = [&]() -> bool {
		bool localNeedsLUT = false;
		u8 localReject = 0U;
		if (gActiveExecutorSummary != nullptr)
			++gActiveExecutorSummary->textureTmemAttemptCount;
		if (!sampleCITextureFromTMEM(
				_work,
				_s,
				_t,
				sampledColor,
				localNeedsLUT,
				localReject,
				sampleDetail)) {
			tmemReject = localReject;
			if (sampleDetail != nullptr)
				sampleDetail->tmemReject = tmemReject;
			if (gActiveExecutorSummary != nullptr) {
				if (tmemReject == 1U)
					++gActiveExecutorSummary->textureTmemRejectFormatCount;
				else if (tmemReject == 2U)
					++gActiveExecutorSummary->textureTmemRejectSizeCount;
				else if (tmemReject == 3U)
					++gActiveExecutorSummary->textureTmemRejectCoordCount;
			}
			return false;
		}
		needsLUT = localNeedsLUT;
		if (gActiveExecutorSummary != nullptr)
			++gActiveExecutorSummary->textureTmemSampleCount;
			sampledColor = finalizeTextureSample(
				sampledColor,
				needsLUT,
				kTexelSourceTMEMBit,
				kDebugTexSampleSourceTMEM);
			captureRdramProbeForTMEM();
			return true;
		};

	const auto tryRdramSample = [&]() -> bool {
		bool localNeedsLUT = false;
		u8 localReject = 0U;
		if (!sampleTextureFromRDRAM(
				_work,
				_s,
				_t,
				sampledColor,
				localNeedsLUT,
				localReject,
				sampleDetail)) {
			rdramReject = localReject;
			if (sampleDetail != nullptr)
				sampleDetail->rdramReject = rdramReject;
			return false;
		}
		(void)rdramReject;
		needsLUT = localNeedsLUT;
		if (gActiveExecutorSummary != nullptr)
			++gActiveExecutorSummary->textureRdramSampleCount;
		sampledColor = finalizeTextureSample(
			sampledColor,
			needsLUT,
			kTexelSourceRdramBit,
			kDebugTexSampleSourceRDRAM);
		return true;
	};

	if (forceRdramPrimary) {
		if (tryRdramSample())
			return sampledColor;
		if (tryTMEMSample())
			return sampledColor;
	}
	else {
		if (tryTMEMSample()) {
			DebugTextureSampleLogEntry tmemDetailSnapshot{};
			if (sampleDetail != nullptr)
				tmemDetailSnapshot = *sampleDetail;
			if (fallbackRdramIfTmemBlack && !pixelHasVisibleColor(sampledColor)) {
				const u32 tmemSampledColor = sampledColor;
				if (tryRdramSample()) {
					if (pixelHasVisibleColor(sampledColor))
						return sampledColor;
					sampledColor = tmemSampledColor;
					if (sampleDetail != nullptr)
						*sampleDetail = tmemDetailSnapshot;
				}
			}
			return sampledColor;
		}
		if (tryRdramSample())
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
	const u32 syntheticRawColor = rgba;
	recordTextureSampleMode(_work, lutModeEnabled, _sampleSlot);
	if (lutModeEnabled) {
		if (gActiveExecutorSummary != nullptr)
			++gActiveExecutorSummary->textureLUTSampleCount;
		rgba = applyTextureLUTModeColor(_work, seed, rgba, sampleDetail);
	}
	if (debugForceAllTexelAlphaOpaque())
		rgba = (rgba & 0xFFFFFF00U) | 0x000000FFU;
	rgba = applyTextureDetailModeColor(_work, seed, rgba);
	if (!debugTextureBucketMaskAllowsSample(_work, lutModeEnabled))
		rgba = 0x000000FFU;
	commitSampleDetail(
		kDebugTexSampleSourceSynthetic,
		syntheticRawColor,
		rgba,
		lutModeEnabled,
		tmemReject,
		rdramReject);
	return rgba;
}

inline u32 pseudoTexel(
	const rvk2::RenderWorkPacket & _work,
	u32 _x,
	u32 _y,
	u32 * _sourceBits = nullptr,
	u8 _sampleSlot = rvk2::kExecutorTextureSampleSlotTexel0)
{
	// In RDP copy mode the rectangle dsdx stream is specified at 4x horizontal scale.
	// Normalize to the per-pixel domain before deriving texel coordinates.
	s32 texDSDX = static_cast<s32>(_work.texDSDX);
	if (_work.phase == static_cast<u8>(rvk2::RenderPhase::kCopy)
		&& !debugDisableCopyModeDSDXDiv4())
		texDSDX /= 4;

	const s32 dx = static_cast<s32>(_x) - static_cast<s32>(_work.rectULX);
	const s32 dy = static_cast<s32>(_y) - static_cast<s32>(_work.rectULY);
	const s32 sRaw = _work.texRectFlip
		? static_cast<s32>(_work.texS) + ((dy * texDSDX) >> 5)
		: static_cast<s32>(_work.texS) + ((dx * texDSDX) >> 5);
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
	const bool copyPhase =
		_work.phase == static_cast<u8>(rvk2::RenderPhase::kCopy);
	const u8 filterMode = copyPhase ? 0U : decodeTextureFilterMode(_work);
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
	std::vector<u8> writeMask;
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
	std::vector<u8> resizedWriteMask(
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
			if (!_surface.writeMask.empty())
				resizedWriteMask[pixelIndex(targetWidth, x, y)] =
					_surface.writeMask[pixelIndex(_surface.width, x, y)];
		}
	}
	_surface.width = targetWidth;
	_surface.height = targetHeight;
	_surface.pixels.swap(resized);
	_surface.coverage.swap(resizedCoverage);
	_surface.hiddenCoverage.swap(resizedHiddenCoverage);
	_surface.writeMask.swap(resizedWriteMask);
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

void appendTrianglePacketLog(
	u64 _workOrdinal,
	const rvk2::RenderWorkPacket & _work,
	bool _boundsValid,
	const WriteBounds & _bounds,
	bool _boundsRejected,
	bool _degenerateRejected,
	u64 _sampleCandidates,
	u64 _writeCount,
	u64 _alphaRejectCount,
	u64 _coverageRejectCount,
	u64 _depthRejectCount,
	u64 _scissorFieldRejectRowCount,
	u64 _yRangeRejectRowCount,
	u64 _xEdgeRejectCount)
{
	const char * logPath = debugTrianglePacketLogPath();
	if (logPath == nullptr)
		return;

	static u32 emitted = 0U;
	const u32 limit = debugTrianglePacketLogLimit();
	if (limit != 0U && emitted >= limit)
		return;
	if (!debugOverwriteLogPassesFilters(_workOrdinal, _work.sourcePacketId))
		return;

	std::FILE * file = std::fopen(logPath, "ab");
	if (file == nullptr)
		return;

	std::fprintf(
		file,
		"frame=%llu\twork_ordinal=%llu\tsource_packet_id=%llu\top_kind=%u\top_name=triangle\tphase=%u\tcolor_image=0x%08X\tbounds_valid=%u\tbounds_x0=%u\tbounds_y0=%u\tbounds_x1=%u\tbounds_y1=%u\tbounds_reject=%u\tdegenerate_reject=%u\tsample_candidates=%llu\twrites=%llu\talpha_reject=%llu\tcoverage_reject=%llu\tdepth_reject=%llu\tscissor_field_reject_rows=%llu\ty_range_reject_rows=%llu\tx_edge_reject=%llu\tcombine_mux=0x%016llX\tother_modes=0x%016llX\tblend_params=0x%08X\ttile_format=%u\ttile_size=%u\ttile_line=%u\ttile_tmem=%u\ttexture_image_width=%u\ttexture_image_address=0x%08X\n",
		static_cast<unsigned long long>(gActiveExecutorFrameId),
		static_cast<unsigned long long>(_workOrdinal),
		static_cast<unsigned long long>(_work.sourcePacketId),
		static_cast<unsigned>(_work.opKind),
		static_cast<unsigned>(_work.phase),
		_work.colorImageAddress,
		_boundsValid ? 1U : 0U,
		_bounds.x0,
		_bounds.y0,
		_bounds.x1,
		_bounds.y1,
		_boundsRejected ? 1U : 0U,
		_degenerateRejected ? 1U : 0U,
		static_cast<unsigned long long>(_sampleCandidates),
		static_cast<unsigned long long>(_writeCount),
		static_cast<unsigned long long>(_alphaRejectCount),
		static_cast<unsigned long long>(_coverageRejectCount),
		static_cast<unsigned long long>(_depthRejectCount),
		static_cast<unsigned long long>(_scissorFieldRejectRowCount),
		static_cast<unsigned long long>(_yRangeRejectRowCount),
		static_cast<unsigned long long>(_xEdgeRejectCount),
		static_cast<unsigned long long>(_work.combineMux),
		static_cast<unsigned long long>(_work.otherModes),
		_work.blendParams,
		static_cast<unsigned>(_work.tileFormat),
		static_cast<unsigned>(_work.tileSize),
		static_cast<unsigned>(_work.tileLine),
		static_cast<unsigned>(_work.tileTmem),
		static_cast<unsigned>(_work.textureImageWidth),
		_work.textureImageAddress);
	std::fclose(file);
	++emitted;
}

inline bool passesScissorFieldFilter(const rvk2::RenderWorkPacket & _work, u32 _y)
{
	const bool interlacedFieldScissor = (_work.scissorMode & 0x2U) != 0U;
	if (!interlacedFieldScissor)
		return true;
	const u32 oddField = static_cast<u32>(_work.scissorMode & 0x1U);
	return (_y & 0x1U) == oddField;
}

inline s32 signExtend14(u16 _value)
{
	const u32 raw = static_cast<u32>(_value) & 0x3FFFU;
	return static_cast<s32>((raw ^ 0x2000U) - 0x2000U);
}

inline s64 evalTriangleEdgeXFixed16AtYSubpixel(
	s32 _xBase,
	s32 _dxdy,
	s32 _yBaseSubpixel,
	s32 _ySubpixel)
{
	return static_cast<s64>(_xBase)
		+ static_cast<s64>(_dxdy) * static_cast<s64>(_ySubpixel - _yBaseSubpixel);
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

inline s32 signExtend9(s32 _value)
{
	const s32 value = _value & 0x1FF;
	if ((value & 0x180) == 0x180)
		return value | ~0x1FF;
	return value;
}

inline u8 clampSpecial9(s32 _value)
{
	const u32 value9 = static_cast<u32>(_value) & 0x1FFU;
	if (value9 < 0x100U)
		return static_cast<u8>(value9);
	if (value9 < 0x180U)
		return 0xFFU;
	return 0x00U;
}

inline u8 evalCombinerEquation(s32 _a, s32 _b, s32 _c, s32 _d)
{
	const s32 value =
		(signExtend9(_a) - signExtend9(_b)) * signExtend9(_c)
		+ (signExtend9(_d) << 8)
		+ 0x80;
	return clampSpecial9(value >> 8);
}

inline u8 evalCombinerAlphaEquation(s32 _a, s32 _b, s32 _c, s32 _d)
{
	const s32 value =
		((signExtend9(_a) - signExtend9(_b)) * signExtend9(_c)
			+ (signExtend9(_d) << 8)
			+ 0x80)
		>> 8;
	return clampSpecial9(value);
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

inline const std::array<u8, 0x8000> & blenderDivideLUT()
{
	static const std::array<u8, 0x8000> table = []() {
		std::array<u8, 0x8000> lut{};
		for (u32 i = 0U; i < lut.size(); ++i) {
			u32 result = 0U;
			const u32 d = (i >> 11U) & 0xFU;
			const u32 n = i & 0x7FFU;
			const u32 invd = (~d) & 0xFU;
			u32 temp = invd + (n >> 8U) + 1U;
			u32 partial[9]{};
			partial[0] = temp & 0x7U;
			for (u32 k = 0U; k < 8U; ++k) {
				const u32 nbit = (n >> (7U - k)) & 0x1U;
				if ((result & (0x100U >> k)) != 0U)
					temp = invd + (partial[k] << 1U) + nbit + 1U;
				else
					temp = d + (partial[k] << 1U) + nbit;
				partial[k + 1U] = temp & 0x7U;
				if ((temp & 0x10U) != 0U)
					result |= 1U << (7U - k);
			}
			lut[i] = static_cast<u8>(result & 0xFFU);
		}
		return lut;
	}();
	return table;
}

inline u8 resolveBlenderDivide(u32 _blendSum, u32 _blended)
{
	const auto & table = blenderDivideLUT();
	const u32 index =
		((_blendSum & 0xFU) << 11U)
		| ((_blended >> 2U) & 0x7FFU);
	return table[index];
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
	bool _finalCycle = true,
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
	const bool useLegacyMemoryAlphaBlend = debugEnableLegacyMemoryAlphaBlend();
	const u8 memoryCoverageAlpha = [&]() -> u8 {
		const u8 coverage3 = static_cast<u8>(std::min<u32>(7U, static_cast<u32>(coverage.destination)));
		if (!debugUseWideMemoryAlphaModel())
			return memoryAlphaFromCoverage3(coverage3);
		const u32 coverage4 =
			std::min<u32>(
				15U,
				static_cast<u32>(coverage3)
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
	const bool blendEquationEnabled = debugDisableBlendEnGating()
		? blendEnabled
		: (_work.forceBlender || (aaEnable && !coverage.overflow));
	const bool colorOnCvgInhibitColorWrite =
		_work.colorOnCvg
		&& !coverage.overflow
		&& !debugDisableColorOnCvgInhibit();
	if (_summary != nullptr) {
		if (blendEnabled)
			++_summary->blenderEnabledOpCount;
		if (blendEnabled && !blendEquationEnabled)
			++_summary->blenderEquationBypassCount;
	}
	if (colorOnCvgInhibitColorWrite) {
		// color_on_cvg inhibit path writes blender M input (2B path) verbatim.
		out.r = mResolved.r;
		out.g = mResolved.g;
		out.b = mResolved.b;
	}
	else if (blendEquationEnabled) {
		const bool useDivide = debugForceBlenderDivide()
			|| (useLegacyMemoryAlphaBlend
				? (_finalCycle && !_work.forceBlender)
				: (!_work.forceBlender));
		if (_summary != nullptr) {
			if (useDivide)
				++_summary->blenderDivideOpCount;
			else
				++_summary->blenderNoDivideOpCount;
		}
		const auto blendChannelResolved = [&](u8 _p, u8 _m) -> u8 {
			if (useLegacyMemoryAlphaBlend) {
				u32 blend1a = static_cast<u32>(alphaA >> 3U);
				u32 blend2a = static_cast<u32>(alphaB >> 3U);
				if ((selectors.m2b & 0x3U) == 1U) {
					if (!_work.depthCompareEnable && !debugDisableLegacyMemoryAlphaShift()) {
						blend1a = (blend1a & 0x3CU);
						blend2a = (blend2a >> noZBlendMemoryAlphaShiftB(_work)) | 0x3U;
						if (_summary != nullptr)
							++_summary->blenderMemoryAlphaShiftApplyCount;
					}
					else {
						blend1a = (blend1a & 0x3CU);
						blend2a = (blend2a | 0x3U);
					}
				}
				const u32 mulb = blend2a + 1U;
				const u32 numer =
					static_cast<u32>(_p) * blend1a
					+ static_cast<u32>(_m) * mulb;
				if (useDivide) {
					const u32 blendSum =
						((blend1a >> 2U) & 0x7U)
						+ ((blend2a >> 2U) & 0x7U)
						+ 1U;
					return resolveBlenderDivide(blendSum, numer);
				}
				return static_cast<u8>((numer + 16U) >> 5U);
			}

			const u32 a5 = static_cast<u32>(alphaA >> 3U);
			const u32 b5 = static_cast<u32>(alphaB >> 3U);
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
		true,
		static_cast<u8>(_srcColor & 0xFFU),
		_summary,
		static_cast<u8>(_srcColor & 0xFFU));
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
	if (!_work.triangleShadeEnable)
		return 0xFFFFFFFFU;
	u32 shade = evaluateTriangleShadeColor(_work, _x, _y);
	// Temporary parity fallback: some paths carry shade alpha without RGB lanes.
	// Borrow primitive RGB to preserve color modulation while keeping shade alpha.
	if ((shade & 0xFFFFFF00U) == 0U && (shade & 0x000000FFU) != 0U) {
		shade = (_work.primColor & 0xFFFFFF00U) | (shade & 0x000000FFU);
	}
	return shade;
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
		if (_work.textured && debugCopyPhaseUseTexel1())
			finalColor = _texel1Color;
		else
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
					true,
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
	case DebugStageViewMode::kWriteMask:
		return 0xFFFFFFFFU;
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
	if (_work.opKind == static_cast<u8>(rvk2::RasterOpKind::kFillRect)
		&& debugDisableFillWrites()) {
		return;
	}
	if (_work.opKind == static_cast<u8>(rvk2::RasterOpKind::kTexRect)
		&& debugDisableTexRectWrites()) {
		return;
	}
	const DebugStageViewMode stageViewMode = debugStageViewMode();
	const bool imageReadEnabledWork = isImageReadEnabled(_work);
	const bool cycle2Work = _work.phase == static_cast<u8>(rvk2::RenderPhase::kCycle2);
	const bool decodeCycle2BlendSelectors = cycle2Work;
	const BlendMuxSelectors stageBlendSelectors = decodeBlendMuxSelectors(_work, decodeCycle2BlendSelectors);
	const bool focusFillCluster = isFocusFillStateCluster(_work);
	const bool focusTexRectCluster = isFocusTexRectStateCluster(_work);
	const u8 focusCluster = focusFillCluster
		? kOverwriteFocusClusterFill
		: (focusTexRectCluster ? kOverwriteFocusClusterTexRect : kOverwriteFocusClusterNone);
	bool hasPrevFocusTexelColor = false;
	u32 prevFocusTexelColor = 0U;
	bool hasPrevFocusTLUTLookup = false;
	u32 prevFocusTLUTLookup = 0U;
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
			resetDebugTextureSampleLogSlots();
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
			const bool captureTexelDetail = debugOverwriteLogIncludeTexelDetail();
			std::array<DebugTextureSampleLogEntry, rvk2::kExecutorTextureSampleSlotBuckets> preAlphaCompareTexelDetail{};
			if (captureTexelDetail)
				preAlphaCompareTexelDetail = gDebugTextureSampleLogSlots;
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
			if (captureTexelDetail)
				gDebugTextureSampleLogSlots = preAlphaCompareTexelDetail;
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
				u32 encodedWriteColor = encodeSurfaceColor(writeColor, _work.colorImageSize);
				const u32 previousEncodedColor = _surface.pixels[colorIdx];
				const bool previousWriteMaskSet =
					(!_surface.writeMask.empty() && _surface.writeMask[colorIdx] != 0U);
				const bool quantizedToBlack =
					(_work.colorImageSize == 2U)
					&& shouldQuantizeColorImage16Surface()
					&& pixelHasVisibleColor(writeColor)
					&& !pixelHasVisibleColor(encodedWriteColor);
			const bool overwriteToBlack =
				pixelHasVisibleColor(previousEncodedColor)
				&& !pixelHasVisibleColor(encodedWriteColor);
			const bool finalBlackWrite = !pixelHasVisibleColor(encodedWriteColor);
			const bool logBlackWrite =
				overwriteToBlack
				|| (finalBlackWrite && debugOverwriteLogIncludeBlackWrites());
			const bool logAnyWrite = debugOverwriteLogIncludeAllWrites();
			const bool logWrite = logBlackWrite || logAnyWrite;
			const bool preserveTexRectNonBlack =
				overwriteToBlack
				&& _work.opKind == static_cast<u8>(rvk2::RasterOpKind::kTexRect)
				&& debugPreserveTexRectNonBlackOverwrites();
			if (overwriteToBlack) {
				++_summary.writeOverwriteToBlackCount;
				if (_work.opKind == static_cast<u8>(rvk2::RasterOpKind::kTexRect))
					++_summary.writeTexRectOverwriteToBlackCount;
			}
				if (quantizedToBlack) {
					++_summary.writeQuantizedToBlackCount;
					if (_work.opKind == static_cast<u8>(rvk2::RasterOpKind::kTexRect))
						++_summary.writeTexRectQuantizedToBlackCount;
				}
				if (preserveTexRectNonBlack)
					encodedWriteColor = previousEncodedColor;
				const bool encodedWriteVisible = pixelHasVisibleColor(encodedWriteColor);
				const bool previousVisible = pixelHasVisibleColor(previousEncodedColor);
				const u64 previousLuma = static_cast<u64>(lumaFromRGBA(previousEncodedColor));
				const u64 encodedWriteLuma = static_cast<u64>(lumaFromRGBA(encodedWriteColor));
				bool focusTexelRepeat = false;
				bool focusTLUTRepeat = false;
				if (focusFillCluster) {
					++_summary.focusFillWriteCount;
					if (previousWriteMaskSet)
						++_summary.focusFillWriteMaskSetCount;
					else
						++_summary.focusFillWriteMaskUnsetCount;
					if (previousVisible)
						++_summary.focusFillPrevNonBlackCount;
					if (encodedWriteVisible)
						++_summary.focusFillNewNonBlackCount;
					if (overwriteToBlack)
						++_summary.focusFillOverwriteToBlackCount;
					_summary.focusFillPrevLumaSum += previousLuma;
					_summary.focusFillNewLumaSum += encodedWriteLuma;
					if (encodedWriteLuma > previousLuma)
						++_summary.focusFillLumaIncreaseCount;
					else if (encodedWriteLuma < previousLuma)
						++_summary.focusFillLumaDecreaseCount;
					else
						++_summary.focusFillLumaEqualCount;
				}
				else if (focusTexRectCluster) {
					++_summary.focusTexRectWriteCount;
					if (previousWriteMaskSet)
						++_summary.focusTexRectWriteMaskSetCount;
					else
						++_summary.focusTexRectWriteMaskUnsetCount;
					if (previousVisible)
						++_summary.focusTexRectPrevNonBlackCount;
					if (encodedWriteVisible)
						++_summary.focusTexRectNewNonBlackCount;
					if (overwriteToBlack)
						++_summary.focusTexRectOverwriteToBlackCount;
					_summary.focusTexRectPrevLumaSum += previousLuma;
					_summary.focusTexRectNewLumaSum += encodedWriteLuma;
					if (encodedWriteLuma > previousLuma)
						++_summary.focusTexRectLumaIncreaseCount;
					else if (encodedWriteLuma < previousLuma)
						++_summary.focusTexRectLumaDecreaseCount;
					else
						++_summary.focusTexRectLumaEqualCount;
					if (hasPrevFocusTexelColor) {
						focusTexelRepeat = (textureColor == prevFocusTexelColor);
						if (focusTexelRepeat)
							++_summary.focusTexRectTexelRepeatCount;
						else
							++_summary.focusTexRectTexelChangeCount;
					}
					prevFocusTexelColor = textureColor;
					hasPrevFocusTexelColor = true;
					if (captureTexelDetail) {
						const DebugTextureSampleLogEntry & tex0 =
							gDebugTextureSampleLogSlots[rvk2::kExecutorTextureSampleSlotTexel0];
						if (tex0.valid && tex0.tlutApplied != 0U) {
							++_summary.focusTexRectTLUTAppliedCount;
							const u32 currentTLUTLookup = static_cast<u32>(tex0.tlutLookupAddress);
							if (hasPrevFocusTLUTLookup) {
								focusTLUTRepeat = (currentTLUTLookup == prevFocusTLUTLookup);
								if (focusTLUTRepeat)
									++_summary.focusTexRectTLUTLookupRepeatCount;
								else
									++_summary.focusTexRectTLUTLookupChangeCount;
							}
							prevFocusTLUTLookup = currentTLUTLookup;
							hasPrevFocusTLUTLookup = true;
						}
						else {
							++_summary.focusTexRectTLUTLookupInvalidCount;
						}
					}
				}
				if (logWrite) {
					appendOverwriteLog(
						_summary.executedWorkCount,
						_work.sourcePacketId,
					_work.opKind,
					_work.colorImageAddress,
					x,
					y,
					previousEncodedColor,
					encodedWriteColor,
					textureColor,
					combinerColor,
					blenderColor,
					finalColor,
						textureSourceBits,
						overwriteToBlack,
						quantizedToBlack,
						preserveTexRectNonBlack,
						previousWriteMaskSet,
						focusCluster,
						focusTexelRepeat,
						focusTLUTRepeat,
						_work);
					}
				_surface.pixels[colorIdx] = encodedWriteColor;
				writeColorImagePixelToRdram(_work, x, y, encodedWriteColor);
				if (!_surface.writeMask.empty())
					_surface.writeMask[colorIdx] = 1U;
			if (!_surface.coverage.empty())
				_surface.coverage[colorIdx] = static_cast<u8>(resolvedCoverage & 0x7U);
			if (!_surface.hiddenCoverage.empty())
				_surface.hiddenCoverage[colorIdx] = resolvedHiddenCoverage ? 1U : 0U;
			const u32 effectiveWriteColor = preserveTexRectNonBlack ? previousEncodedColor : writeColor;
			const u64 effectiveWriteLuma = preserveTexRectNonBlack
				? static_cast<u64>(lumaFromRGBA(previousEncodedColor))
				: writeLuma;
			_summary.outputLumaSum += effectiveWriteLuma;
			if (_work.opKind == static_cast<u8>(rvk2::RasterOpKind::kTexRect)) {
				_summary.writeTexRectLumaSum += effectiveWriteLuma;
				if (pixelHasVisibleColor(effectiveWriteColor))
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
	u64 _workOrdinal,
	rvk2::ExecutorSummary & _summary)
{
	if (debugDisableTriangleWrites())
		return;
	u64 localSampleCandidateCount = 0ULL;
	u64 localWriteCount = 0ULL;
	u64 localAlphaRejectCount = 0ULL;
	u64 localCoverageRejectCount = 0ULL;
	u64 localDepthRejectCount = 0ULL;
	u64 localScissorFieldRejectRowCount = 0ULL;
	u64 localYRangeRejectRowCount = 0ULL;
	u64 localXEdgeRejectCount = 0ULL;
	bool boundsRejected = false;
	bool degenerateRejected = false;
	bool boundsValid = false;
	const DebugStageViewMode stageViewMode = debugStageViewMode();
	const bool imageReadEnabledWork = isImageReadEnabled(_work);
	const bool cycle2Work = _work.phase == static_cast<u8>(rvk2::RenderPhase::kCycle2);
	const bool decodeCycle2BlendSelectors = cycle2Work;
	const BlendMuxSelectors stageBlendSelectors = decodeBlendMuxSelectors(_work, decodeCycle2BlendSelectors);
	const u32 ySubpixelBias = debugTriangleSampleYSubpixelBias();
	const s64 xSubpixelBias = static_cast<s64>(debugTriangleSampleXSubpixelBias());
	WriteBounds bounds{};
	if (!computeWriteBounds(_work, _config.maxSurfaceWidth, _config.maxSurfaceHeight, bounds)) {
		++_summary.triangleBoundsRejectCount;
		boundsRejected = true;
		appendTrianglePacketLog(
			_workOrdinal,
			_work,
			false,
			bounds,
			boundsRejected,
			degenerateRejected,
			localSampleCandidateCount,
			localWriteCount,
			localAlphaRejectCount,
			localCoverageRejectCount,
			localDepthRejectCount,
			localScissorFieldRejectRowCount,
			localYRangeRejectRowCount,
			localXEdgeRejectCount);
		return;
	}
	boundsValid = true;
	const u16 requiredWidth = static_cast<u16>(std::min<u32>(bounds.x1 + 1U, _config.maxSurfaceWidth));
	const u16 requiredHeight = static_cast<u16>(std::min<u32>(bounds.y1 + 1U, _config.maxSurfaceHeight));
	ensureSurfaceSize(_surface, requiredWidth, requiredHeight, _config.maxSurfaceWidth, _config.maxSurfaceHeight);
	if (_depthSurface != nullptr)
		ensureDepthSurfaceSize(*_depthSurface, requiredWidth, requiredHeight, _config.maxSurfaceWidth, _config.maxSurfaceHeight);

	// Triangle Y edges are signed 14-bit s10.2 values.
	const s32 yhSigned = signExtend14(_work.triangleYH);
	const s32 ymSigned = signExtend14(_work.triangleYM);
	const s32 ylSigned = signExtend14(_work.triangleYL);
	if (ylSigned <= yhSigned) {
		++_summary.triangleDegenerateRejectCount;
		degenerateRejected = true;
		appendTrianglePacketLog(
			_workOrdinal,
			_work,
			boundsValid,
			bounds,
			boundsRejected,
			degenerateRejected,
			localSampleCandidateCount,
			localWriteCount,
			localAlphaRejectCount,
			localCoverageRejectCount,
			localDepthRejectCount,
			localScissorFieldRejectRowCount,
			localYRangeRejectRowCount,
			localXEdgeRejectCount);
		return;
	}

	for (u32 y = bounds.y0; y <= bounds.y1; ++y) {
		if (!passesScissorFieldFilter(_work, y)) {
			++_summary.triangleScissorFieldRejectCount;
			++localScissorFieldRejectRowCount;
			continue;
		}
		bool hasPrevPixelForCycle2 = false;
		u32 prevMemoryColorForCycle2 = 0U;
		u8 prevMemoryCoverageForCycle2 = 7U;
		bool prevMemoryHiddenCoverageForCycle2 = false;
		bool hasPrevCycle1CombinedColor = false;
		u32 prevCycle1CombinedColor = 0U;
		const s32 ySubpixelSample = static_cast<s32>((y << 2U) + ySubpixelBias);
		if (ySubpixelSample < yhSigned || ySubpixelSample >= ylSigned) {
			++localYRangeRejectRowCount;
			continue;
		}
		const bool upperShortEdge = ySubpixelSample < ymSigned;
		const s64 xLong = evalTriangleEdgeXFixed16AtYSubpixel(
			_work.triangleXH,
			_work.triangleDxHDY,
			yhSigned,
			ySubpixelSample);
		const s64 xShort = upperShortEdge
			? evalTriangleEdgeXFixed16AtYSubpixel(
				_work.triangleXM,
				_work.triangleDxMDY,
				yhSigned,
				ySubpixelSample)
			: evalTriangleEdgeXFixed16AtYSubpixel(
				_work.triangleXL,
				_work.triangleDxLDY,
				ymSigned,
				ySubpixelSample);
		const bool lMajor = debugInvertTriangleLMajor() ? !_work.triangleLMajor : _work.triangleLMajor;
		s64 xLeft = lMajor ? xLong : xShort;
		s64 xRight = lMajor ? xShort : xLong;
		if (xLeft > xRight)
			std::swap(xLeft, xRight);
		if (xLeft == xRight)
			continue;
		for (u32 x = bounds.x0; x <= bounds.x1; ++x) {
			const s64 xSubpixelSample = (static_cast<s64>(x) << 16U) + xSubpixelBias;
			if (xSubpixelSample < xLeft || xSubpixelSample > xRight) {
				++localXEdgeRejectCount;
				continue;
			}
			resetDebugTextureSampleLogSlots();
			++_summary.triangleSampleCandidateCount;
			++localSampleCandidateCount;

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
			const bool captureTexelDetail = debugOverwriteLogIncludeTexelDetail();
			std::array<DebugTextureSampleLogEntry, rvk2::kExecutorTextureSampleSlotBuckets> preAlphaCompareTexelDetail{};
			if (captureTexelDetail)
				preAlphaCompareTexelDetail = gDebugTextureSampleLogSlots;
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
			if (captureTexelDetail)
				gDebugTextureSampleLogSlots = preAlphaCompareTexelDetail;
			if (!passesSyntheticAlphaCompare(_work, alphaCompareColor, x, y, &_summary)) {
				++_summary.triangleAlphaRejectCount;
				++localAlphaRejectCount;
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
				++localCoverageRejectCount;
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
					++localDepthRejectCount;
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
			u32 encodedWriteColor = encodeSurfaceColor(writeColor, _work.colorImageSize);
			const u32 previousEncodedColor = _surface.pixels[colorIdx];
			const bool quantizedToBlack =
				(_work.colorImageSize == 2U)
				&& shouldQuantizeColorImage16Surface()
				&& pixelHasVisibleColor(writeColor)
				&& !pixelHasVisibleColor(encodedWriteColor);
			const bool overwriteToBlack =
				pixelHasVisibleColor(previousEncodedColor)
				&& !pixelHasVisibleColor(encodedWriteColor);
			const bool finalBlackWrite = !pixelHasVisibleColor(encodedWriteColor);
			const bool logBlackWrite =
				overwriteToBlack
				|| (finalBlackWrite && debugOverwriteLogIncludeBlackWrites());
			const bool logAnyWrite = debugOverwriteLogIncludeAllWrites();
			const bool logWrite = logBlackWrite || logAnyWrite;
			const bool preserveNonBlackOverwrite =
				overwriteToBlack && debugPreserveTriangleNonBlackOverwrites();
			if (overwriteToBlack) {
				++_summary.writeOverwriteToBlackCount;
				++_summary.writeTriangleOverwriteToBlackCount;
			}
			if (quantizedToBlack) {
				++_summary.writeQuantizedToBlackCount;
				++_summary.writeTriangleQuantizedToBlackCount;
			}
			if (preserveNonBlackOverwrite) {
				encodedWriteColor = previousEncodedColor;
				++_summary.writeTrianglePreserveNonBlackCount;
			}
			++localWriteCount;
			if (logWrite) {
				appendOverwriteLog(
					_workOrdinal,
					_work.sourcePacketId,
					_work.opKind,
					_work.colorImageAddress,
					x,
					y,
					previousEncodedColor,
					encodedWriteColor,
					textureColor,
					combinerColor,
					blenderColor,
					finalColor,
						textureSourceBits,
						overwriteToBlack,
						quantizedToBlack,
						preserveNonBlackOverwrite,
						false,
						kOverwriteFocusClusterNone,
						false,
						false,
						_work);
				}
				_surface.pixels[colorIdx] = encodedWriteColor;
				writeColorImagePixelToRdram(_work, x, y, encodedWriteColor);
				if (!_surface.writeMask.empty())
					_surface.writeMask[colorIdx] = 1U;
			if (!preserveNonBlackOverwrite && !_surface.coverage.empty())
					_surface.coverage[colorIdx] = static_cast<u8>(resolvedCoverage & 0x7U);
				if (!preserveNonBlackOverwrite && !_surface.hiddenCoverage.empty())
					_surface.hiddenCoverage[colorIdx] = resolvedHiddenCoverage ? 1U : 0U;
				const u32 effectiveWriteColor = preserveNonBlackOverwrite ? previousEncodedColor : writeColor;
				const u64 effectiveWriteLuma = preserveNonBlackOverwrite ? static_cast<u64>(lumaFromRGBA(previousEncodedColor)) : writeLuma;
				_summary.outputLumaSum += effectiveWriteLuma;
				_summary.writeTriangleLumaSum += effectiveWriteLuma;
				if (pixelHasVisibleColor(effectiveWriteColor))
					++_summary.writeTriangleNonBlackCount;
				++_summary.colorWriteCount;
		}
	}
	appendTrianglePacketLog(
		_workOrdinal,
		_work,
		boundsValid,
		bounds,
		boundsRejected,
		degenerateRejected,
		localSampleCandidateCount,
		localWriteCount,
		localAlphaRejectCount,
		localCoverageRejectCount,
		localDepthRejectCount,
		localScissorFieldRejectRowCount,
		localYRangeRejectRowCount,
		localXEdgeRejectCount);
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
	logActiveDebugTogglesOnce();
	const TextureReplacementStore * previousReplacementStore = gActiveTextureReplacementStore;
	gActiveTextureReplacementStore =
		(!m_textureReplacementStore.empty() && m_config.textureReplacementEnable)
		? &m_textureReplacementStore
		: nullptr;
	const u64 * previousTMEMWords = gActiveExecutorTMEMWords;
	const u64 previousFrameId = gActiveExecutorFrameId;
	gActiveExecutorFrameId = m_config.frameId;

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
			(stageViewMode == DebugStageViewMode::kVISource
				|| stageViewMode == DebugStageViewMode::kWriteMask)
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
	std::unordered_map<u32, u64> surfaceOverwriteBlackCounts;
	std::unordered_map<u32, u64> surfaceOverwriteBlackTexRectCounts;
	std::unordered_map<u32, u64> surfaceOverwriteBlackTriangleCounts;
	std::unordered_map<u32, u64> surfaceQuantizedBlackCounts;
	std::unordered_map<u32, u64> surfaceQuantizedBlackTexRectCounts;
	std::unordered_map<u32, u64> surfaceQuantizedBlackTriangleCounts;
	std::unordered_map<u32, u64> surfaceTrianglePreserveNonBlackCounts;
	std::unordered_map<u32, u64> surfaceTexRectNonBlackWriteCounts;
	std::unordered_map<u32, u64> surfaceTriangleNonBlackWriteCounts;
	struct SurfaceAddressRoleStats {
		u64 workCount = 0ULL;
		u64 depthAliasedWorkCount = 0ULL;
		u64 nonDepthAliasedWorkCount = 0ULL;
	};
	std::unordered_map<u32, SurfaceAddressRoleStats> surfaceAddressRoles;
	const bool allowSurfaceHistoryBootstrap = debugEnableSurfaceHistoryBootstrap();
	const bool allowCrossSurfaceBootstrap = debugEnableCrossSurfaceBootstrap();
	const bool allowDepthAliasedHistoryCarry = debugAllowDepthAliasHistoryCarry();
	for (const RenderWorkPacket & work : _workPackets) {
		if (work.opKind != static_cast<u8>(RasterOpKind::kFillRect)
			&& work.opKind != static_cast<u8>(RasterOpKind::kTexRect)
			&& work.opKind != static_cast<u8>(RasterOpKind::kTriangle)) {
			continue;
		}
		SurfaceAddressRoleStats & role = surfaceAddressRoles[work.colorImageAddress];
		++role.workCount;
		if (work.depthImageAddress != 0U
			&& work.depthImageAddress == work.colorImageAddress) {
			++role.depthAliasedWorkCount;
		}
		else {
			++role.nonDepthAliasedWorkCount;
		}
	}
	const auto isDepthOnlyColorAddress = [&](u32 _address) {
		const auto itRole = surfaceAddressRoles.find(_address);
		if (itRole == surfaceAddressRoles.end())
			return false;
		const SurfaceAddressRoleStats & role = itRole->second;
		return role.workCount > 0ULL
			&& role.depthAliasedWorkCount > 0ULL
			&& role.nonDepthAliasedWorkCount == 0ULL;
	};
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
				if (surface.width == 0U) {
					const u16 desiredWidth = std::max<u16>(
						1U,
						std::min<u16>(work.colorImageWidth, m_config.maxSurfaceWidth));
					const auto restoreSurfaceFromCache =
						[&](const ExecutorCachedSurface & _cached, u64 * _copiedNonBlackPixels = nullptr) -> bool {
						if (!_cached.valid
							|| _cached.format != work.colorImageFormat
							|| _cached.size != work.colorImageSize
							|| _cached.width != desiredWidth
							|| _cached.height == 0U) {
							return false;
						}
						const u16 restoredHeight = std::min<u16>(_cached.height, m_config.maxSurfaceHeight);
						const size_t restoredPixelCount =
							static_cast<size_t>(desiredWidth) * static_cast<size_t>(restoredHeight);
						if (_cached.pixels.size() < restoredPixelCount)
							return false;
						surface.width = desiredWidth;
						surface.height = restoredHeight;
						surface.pixels.assign(
							_cached.pixels.begin(),
							_cached.pixels.begin() + restoredPixelCount);
						if (_cached.coverage.size() >= restoredPixelCount) {
							surface.coverage.assign(
								_cached.coverage.begin(),
								_cached.coverage.begin() + restoredPixelCount);
						}
						else
							surface.coverage.assign(restoredPixelCount, 0U);
							if (_cached.hiddenCoverage.size() >= restoredPixelCount) {
								surface.hiddenCoverage.assign(
									_cached.hiddenCoverage.begin(),
									_cached.hiddenCoverage.begin() + restoredPixelCount);
							}
							else
								surface.hiddenCoverage.assign(restoredPixelCount, 0U);
							surface.writeMask.assign(restoredPixelCount, 0U);
							if (_copiedNonBlackPixels != nullptr) {
								u64 copied = 0ULL;
								for (size_t i = 0U; i < restoredPixelCount; ++i) {
								if (pixelHasVisibleColor(surface.pixels[i]))
									++copied;
							}
							*_copiedNonBlackPixels = copied;
						}
						return true;
					};

					bool restoredFromHistory = false;
					if (allowSurfaceHistoryBootstrap) {
						++summary.surfaceBootstrapSameAddressAttemptCount;
						const auto historyIt = m_surfaceHistory.find(work.colorImageAddress);
						if (historyIt != m_surfaceHistory.end()) {
							u64 copied = 0ULL;
							restoredFromHistory =
								restoreSurfaceFromCache(historyIt->second, &copied);
							if (restoredFromHistory) {
								++summary.surfaceBootstrapSameAddressSuccessCount;
								summary.surfaceBootstrapSameAddressCopiedPixels += copied;
							}
						}
					}
					if (!restoredFromHistory
						&& allowSurfaceHistoryBootstrap
						&& allowCrossSurfaceBootstrap) {
						u64 copied = 0ULL;
						restoredFromHistory =
							restoreSurfaceFromCache(m_lastSelectedSurface, &copied);
						if (restoredFromHistory) {
							++summary.surfaceBootstrapFallbackRestoreSuccessCount;
							summary.surfaceBootstrapFallbackRestoreCopiedPixels += copied;
						}
					}
					if (!restoredFromHistory)
						surface.width = desiredWidth;
				}
				if (surface.height == 0U)
					surface.height = 1U;
				if (surface.pixels.empty())
					surface.pixels.resize(static_cast<size_t>(surface.width) * static_cast<size_t>(surface.height), 0U);
				if (surface.coverage.empty())
					surface.coverage.resize(static_cast<size_t>(surface.width) * static_cast<size_t>(surface.height), 0U);
				if (surface.hiddenCoverage.empty())
					surface.hiddenCoverage.resize(static_cast<size_t>(surface.width) * static_cast<size_t>(surface.height), 0U);
				if (surface.writeMask.empty())
					surface.writeMask.resize(static_cast<size_t>(surface.width) * static_cast<size_t>(surface.height), 0U);
					if (allowSurfaceHistoryBootstrap && allowCrossSurfaceBootstrap) {
						const auto mergeSurfaceFromCache =
						[&](const ExecutorCachedSurface & _cached, bool _copyAll) {
							if (!_cached.valid
								|| _cached.address == work.colorImageAddress
								|| (isDepthOnlyColorAddress(_cached.address)
									&& !allowDepthAliasedHistoryCarry)
								|| _cached.format != work.colorImageFormat
								|| _cached.size != work.colorImageSize
								|| _cached.width != surface.width
								|| _cached.height < surface.height) {
								return;
							}
							++summary.surfaceBootstrapCrossMergeCandidateCount;
							const size_t pixelCount =
								static_cast<size_t>(surface.width) * static_cast<size_t>(surface.height);
							if (_cached.pixels.size() < pixelCount)
								return;
							const bool hasPrevCoverage = _cached.coverage.size() >= pixelCount;
							const bool hasPrevHidden = _cached.hiddenCoverage.size() >= pixelCount;
							u64 copiedPixels = 0ULL;
							for (size_t i = 0U; i < pixelCount; ++i) {
								const u32 previousPixel = _cached.pixels[i];
								if (!_copyAll) {
									if (!pixelHasVisibleColor(previousPixel))
										continue;
									const u32 destinationPixel = surface.pixels[i];
									const bool destinationVisible = pixelHasVisibleColor(destinationPixel);
									if (destinationVisible) {
										const u8 destinationLuma = lumaFromRGBA(destinationPixel);
										const u8 sourceLuma = lumaFromRGBA(previousPixel);
										const bool destinationVeryDark = destinationLuma <= 4U;
										const bool sourceMuchBrighter =
											sourceLuma >= 24U
											&& static_cast<u32>(sourceLuma)
												> (static_cast<u32>(destinationLuma) + 16U);
										if (!(destinationVeryDark && sourceMuchBrighter))
											continue;
									}
								}
								if (surface.pixels[i] != previousPixel) {
									surface.pixels[i] = previousPixel;
									++copiedPixels;
								}
								if (hasPrevCoverage) {
									if (_copyAll || surface.coverage[i] == 0U)
										surface.coverage[i] = static_cast<u8>(_cached.coverage[i] & 0x7U);
								}
								if (hasPrevHidden) {
									if (_copyAll || surface.hiddenCoverage[i] == 0U)
										surface.hiddenCoverage[i] = static_cast<u8>(_cached.hiddenCoverage[i] & 0x1U);
								}
							}
							summary.surfaceBootstrapCrossMergeCopiedPixels += copiedPixels;
							if (_copyAll)
								summary.surfaceBootstrapCrossMergeCopyAllPixels += copiedPixels;
						};

					mergeSurfaceFromCache(
						m_lastSelectedSurface,
						debugCrossSurfaceBootstrapCopyAllFromLastSurface());

					std::vector<const ExecutorCachedSurface *> historyCandidates;
					historyCandidates.reserve(m_surfaceHistory.size());
					for (const auto & historyEntry : m_surfaceHistory) {
						const ExecutorCachedSurface & cached = historyEntry.second;
						if (!cached.valid || cached.address == work.colorImageAddress)
							continue;
						if (isDepthOnlyColorAddress(cached.address)
							&& !allowDepthAliasedHistoryCarry) {
							continue;
						}
						if (m_lastSelectedSurface.valid
							&& cached.address == m_lastSelectedSurface.address) {
							continue;
						}
						historyCandidates.push_back(&cached);
					}
					std::sort(
						historyCandidates.begin(),
						historyCandidates.end(),
						[](const ExecutorCachedSurface * _a, const ExecutorCachedSurface * _b) {
							if (_a == nullptr || _b == nullptr)
								return _a != nullptr;
							if (_a->lastTouched != _b->lastTouched)
								return _a->lastTouched > _b->lastTouched;
							return _a->address < _b->address;
						});
					for (const ExecutorCachedSurface * cached : historyCandidates) {
						if (cached == nullptr)
							continue;
						mergeSurfaceFromCache(*cached, false);
					}
				}

				const u64 writesBefore = summary.colorWriteCount;
				const u64 overwriteBlackBefore = summary.writeOverwriteToBlackCount;
				const u64 overwriteBlackTexRectBefore = summary.writeTexRectOverwriteToBlackCount;
				const u64 overwriteBlackTriangleBefore = summary.writeTriangleOverwriteToBlackCount;
				const u64 quantizedBlackBefore = summary.writeQuantizedToBlackCount;
				const u64 quantizedBlackTexRectBefore = summary.writeTexRectQuantizedToBlackCount;
				const u64 quantizedBlackTriangleBefore = summary.writeTriangleQuantizedToBlackCount;
				const u64 trianglePreserveNonBlackBefore = summary.writeTrianglePreserveNonBlackCount;
				const u64 texRectNonBlackBefore = summary.writeTexRectNonBlackCount;
				const u64 triangleNonBlackBefore = summary.writeTriangleNonBlackCount;
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
				writeTriangle(surface, depthSurface, work, m_config, summary.executedWorkCount, summary);
			}
			else
				writeRect(surface, work, m_config, summary);
				const u64 writesAfter = summary.colorWriteCount;
				if (writesAfter > writesBefore)
					surfaceColorWrites[work.colorImageAddress] += (writesAfter - writesBefore);
				const u64 overwriteBlackDelta = summary.writeOverwriteToBlackCount - overwriteBlackBefore;
				if (overwriteBlackDelta > 0ULL)
					surfaceOverwriteBlackCounts[work.colorImageAddress] += overwriteBlackDelta;
				const u64 overwriteBlackTexRectDelta =
					summary.writeTexRectOverwriteToBlackCount - overwriteBlackTexRectBefore;
				if (overwriteBlackTexRectDelta > 0ULL)
					surfaceOverwriteBlackTexRectCounts[work.colorImageAddress] += overwriteBlackTexRectDelta;
				const u64 overwriteBlackTriangleDelta =
					summary.writeTriangleOverwriteToBlackCount - overwriteBlackTriangleBefore;
				if (overwriteBlackTriangleDelta > 0ULL)
					surfaceOverwriteBlackTriangleCounts[work.colorImageAddress] += overwriteBlackTriangleDelta;
				const u64 quantizedBlackDelta = summary.writeQuantizedToBlackCount - quantizedBlackBefore;
				if (quantizedBlackDelta > 0ULL)
					surfaceQuantizedBlackCounts[work.colorImageAddress] += quantizedBlackDelta;
				const u64 quantizedBlackTexRectDelta =
					summary.writeTexRectQuantizedToBlackCount - quantizedBlackTexRectBefore;
				if (quantizedBlackTexRectDelta > 0ULL)
					surfaceQuantizedBlackTexRectCounts[work.colorImageAddress] += quantizedBlackTexRectDelta;
				const u64 quantizedBlackTriangleDelta =
					summary.writeTriangleQuantizedToBlackCount - quantizedBlackTriangleBefore;
				if (quantizedBlackTriangleDelta > 0ULL)
					surfaceQuantizedBlackTriangleCounts[work.colorImageAddress] += quantizedBlackTriangleDelta;
				const u64 trianglePreserveNonBlackDelta =
					summary.writeTrianglePreserveNonBlackCount - trianglePreserveNonBlackBefore;
				if (trianglePreserveNonBlackDelta > 0ULL)
					surfaceTrianglePreserveNonBlackCounts[work.colorImageAddress] += trianglePreserveNonBlackDelta;
				const u64 texRectNonBlackDelta =
					summary.writeTexRectNonBlackCount - texRectNonBlackBefore;
				if (texRectNonBlackDelta > 0ULL)
					surfaceTexRectNonBlackWriteCounts[work.colorImageAddress] += texRectNonBlackDelta;
				const u64 triangleNonBlackDelta =
					summary.writeTriangleNonBlackCount - triangleNonBlackBefore;
				if (triangleNonBlackDelta > 0ULL)
					surfaceTriangleNonBlackWriteCounts[work.colorImageAddress] += triangleNonBlackDelta;
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
	const bool allowVIHistorySelection = !debugDisableVIHistoryPresentSelection();
	summary.historySurfaceCount = static_cast<u32>(m_surfaceHistory.size());
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
			if (!allowVIHistorySelection)
				return false;
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
			summary.historyVIOriginCandidateFound = 1U;
			summary.historyVIOriginCandidateAddress = historyMatchedAddress;
			summary.historyVIOriginCandidateExact = historyExact ? 1U : 0U;
			summary.historyVIOriginCandidateAge = historyAge;
			if (_requireRecentHistory && historyAge > kExecutorVIHistorySelectionMaxAge)
			{
				summary.historyVIOriginCandidateRejectedByAge = 1U;
				return false;
			}
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
	if (it == surfaces.end()
		&& (!allowVIHistorySelection || m_surfaceHistory.find(presentSurfaceAddress) == m_surfaceHistory.end())) {
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
	auto historyIt = allowVIHistorySelection
		? m_surfaceHistory.find(presentSurfaceAddress)
		: m_surfaceHistory.end();
	u32 forcedPresentSurfaceAddress = 0U;
	if (debugForcedPresentSurfaceAddress(forcedPresentSurfaceAddress)) {
		const auto forcedLiveIt = surfaces.find(forcedPresentSurfaceAddress);
		const auto forcedHistoryIt = allowVIHistorySelection
			? m_surfaceHistory.find(forcedPresentSurfaceAddress)
			: m_surfaceHistory.end();
		if (forcedLiveIt != surfaces.end() || forcedHistoryIt != m_surfaceHistory.end()) {
			presentSurfaceAddress = forcedPresentSurfaceAddress;
			it = forcedLiveIt;
			historyIt = forcedHistoryIt;
			viOriginMatchedSurface = false;
			summary.viOriginMatchedSurface = 0U;
			summary.presentSelectionReason =
				it != surfaces.end()
					? kExecutorPresentSelectionLastSurface
					: kExecutorPresentSelectionPreviousSurface;
		}
	}
	const bool viMatchedHistorySelection =
		viOriginMatchedSurface
		&& historyIt != m_surfaceHistory.end()
		&& it == surfaces.end();
	const auto chooseMostWrittenCompatibleLiveSurface =
		[&](const ExecutorCachedSurface & _historySurface, u32 & _outAddress) -> bool {
		u32 bestAddress = 0U;
		u64 bestWrites = 0ULL;
		u64 bestWorks = 0ULL;
		bool found = false;
		for (const auto & liveEntry : surfaces) {
			const u32 liveAddress = liveEntry.first;
			if (liveAddress == _historySurface.address)
				continue;
			if (isDepthOnlyColorAddress(liveAddress) && !allowDepthAliasedHistoryCarry)
				continue;
			const ColorSurface & liveSurface = liveEntry.second;
			if (liveSurface.format != _historySurface.format
				|| liveSurface.size != _historySurface.size
				|| liveSurface.width != _historySurface.width
				|| liveSurface.height != _historySurface.height) {
				continue;
			}
			const auto writeIt = surfaceColorWrites.find(liveAddress);
			const auto workIt = surfaceWorkCounts.find(liveAddress);
			const u64 writes = writeIt != surfaceColorWrites.end() ? writeIt->second : 0ULL;
			const u64 works = workIt != surfaceWorkCounts.end() ? workIt->second : 0ULL;
			if (writes == 0ULL)
				continue;
			if (!found
				|| writes > bestWrites
				|| (writes == bestWrites && works > bestWorks)
				|| (writes == bestWrites && works == bestWorks && liveAddress < bestAddress)) {
				found = true;
				bestAddress = liveAddress;
				bestWrites = writes;
				bestWorks = works;
			}
		}
		if (!found)
			return false;
		_outAddress = bestAddress;
		return true;
	};
	bool preserveVIHistorySelection = viMatchedHistorySelection;
	if (viMatchedHistorySelection
		&& frameHasLiveSurfaceWrites
		&& historyIt != m_surfaceHistory.end()
		&& debugPreferLiveSurfaceOverHistory()
		&& !debugKeepVIMatchedHistorySelection()) {
		u32 compatibleLiveAddress = 0U;
		if (chooseMostWrittenCompatibleLiveSurface(historyIt->second, compatibleLiveAddress)
			&& compatibleLiveAddress != 0U
			&& compatibleLiveAddress != presentSurfaceAddress) {
			presentSurfaceAddress = compatibleLiveAddress;
			it = surfaces.find(presentSurfaceAddress);
			historyIt = allowVIHistorySelection
				? m_surfaceHistory.find(presentSurfaceAddress)
				: m_surfaceHistory.end();
			viOriginMatchedSurface = false;
			summary.viOriginMatchedSurface = 0U;
			summary.selectedPresentSurfaceHistoryAge = 0ULL;
			summary.presentSelectionReason = kExecutorPresentSelectionMostWrittenFallback;
			preserveVIHistorySelection = false;
		}
	}
	if (historyIt != m_surfaceHistory.end()
		&& debugPreferLiveSurfaceOverHistory()
		&& !(preserveVIHistorySelection
			|| (viOriginMatchedSurface && debugKeepVIMatchedHistorySelection()))) {
		const auto selectedLiveWriteIt = surfaceColorWrites.find(presentSurfaceAddress);
		const bool selectedHasLiveWrites =
			selectedLiveWriteIt != surfaceColorWrites.end()
			&& selectedLiveWriteIt->second > 0ULL;
		if (it == surfaces.end() || !selectedHasLiveWrites) {
			u32 fallbackAddress = 0U;
			if (chooseMostWrittenSurfaceAddress(
					surfaces,
					surfaceColorWrites,
					surfaceWorkCounts,
					fallbackAddress)) {
				const auto fallbackWriteIt = surfaceColorWrites.find(fallbackAddress);
				const u64 fallbackWrites =
					fallbackWriteIt != surfaceColorWrites.end()
					? fallbackWriteIt->second
					: 0ULL;
				if (fallbackAddress != 0U
					&& fallbackAddress != presentSurfaceAddress
					&& fallbackWrites > 0ULL) {
					presentSurfaceAddress = fallbackAddress;
					it = surfaces.find(presentSurfaceAddress);
					historyIt = allowVIHistorySelection
						? m_surfaceHistory.find(presentSurfaceAddress)
						: m_surfaceHistory.end();
					viOriginMatchedSurface = false;
					summary.viOriginMatchedSurface = 0U;
					summary.presentSelectionReason = kExecutorPresentSelectionMostWrittenFallback;
				}
			}
		}
	}
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
	const auto selectedOverwriteBlackIt = surfaceOverwriteBlackCounts.find(presentSurfaceAddress);
	if (selectedOverwriteBlackIt != surfaceOverwriteBlackCounts.end())
		summary.selectedPresentSurfaceOverwriteBlackCount = selectedOverwriteBlackIt->second;
	const auto selectedOverwriteBlackTexRectIt = surfaceOverwriteBlackTexRectCounts.find(presentSurfaceAddress);
	if (selectedOverwriteBlackTexRectIt != surfaceOverwriteBlackTexRectCounts.end())
		summary.selectedPresentSurfaceOverwriteBlackTexRectCount = selectedOverwriteBlackTexRectIt->second;
	const auto selectedOverwriteBlackTriangleIt = surfaceOverwriteBlackTriangleCounts.find(presentSurfaceAddress);
	if (selectedOverwriteBlackTriangleIt != surfaceOverwriteBlackTriangleCounts.end())
		summary.selectedPresentSurfaceOverwriteBlackTriangleCount = selectedOverwriteBlackTriangleIt->second;
	const auto selectedQuantizedBlackIt = surfaceQuantizedBlackCounts.find(presentSurfaceAddress);
	if (selectedQuantizedBlackIt != surfaceQuantizedBlackCounts.end())
		summary.selectedPresentSurfaceQuantizedToBlackCount = selectedQuantizedBlackIt->second;
	const auto selectedQuantizedBlackTexRectIt = surfaceQuantizedBlackTexRectCounts.find(presentSurfaceAddress);
	if (selectedQuantizedBlackTexRectIt != surfaceQuantizedBlackTexRectCounts.end()) {
		summary.selectedPresentSurfaceQuantizedToBlackTexRectCount =
			selectedQuantizedBlackTexRectIt->second;
	}
	const auto selectedQuantizedBlackTriangleIt =
		surfaceQuantizedBlackTriangleCounts.find(presentSurfaceAddress);
	if (selectedQuantizedBlackTriangleIt != surfaceQuantizedBlackTriangleCounts.end()) {
		summary.selectedPresentSurfaceQuantizedToBlackTriangleCount =
			selectedQuantizedBlackTriangleIt->second;
	}
	const auto selectedTrianglePreserveNonBlackIt = surfaceTrianglePreserveNonBlackCounts.find(presentSurfaceAddress);
	if (selectedTrianglePreserveNonBlackIt != surfaceTrianglePreserveNonBlackCounts.end()) {
		summary.selectedPresentSurfaceTrianglePreserveNonBlackCount =
			selectedTrianglePreserveNonBlackIt->second;
	}
	const auto selectedTexRectNonBlackIt = surfaceTexRectNonBlackWriteCounts.find(presentSurfaceAddress);
	if (selectedTexRectNonBlackIt != surfaceTexRectNonBlackWriteCounts.end())
		summary.selectedPresentSurfaceTexRectNonBlackWriteCount = selectedTexRectNonBlackIt->second;
	const auto selectedTriangleNonBlackIt = surfaceTriangleNonBlackWriteCounts.find(presentSurfaceAddress);
	if (selectedTriangleNonBlackIt != surfaceTriangleNonBlackWriteCounts.end()) {
		summary.selectedPresentSurfaceTriangleNonBlackWriteCount =
			selectedTriangleNonBlackIt->second;
	}
	if (it != surfaces.end()
		&& debugEnableUntouchedPresentCarry()
		&& summary.selectedPresentSurfaceLiveWriteCount > 0ULL
		&& !m_surfaceHistory.empty()) {
		ColorSurface & selectedSurface = it->second;
		const size_t pixelCount =
			static_cast<size_t>(selectedSurface.width) * static_cast<size_t>(selectedSurface.height);
		if (selectedSurface.pixels.size() >= pixelCount && selectedSurface.writeMask.size() >= pixelCount) {
			const auto scoreCandidate = [&](const ExecutorCachedSurface & _candidate) -> u64 {
				if (!_candidate.valid
					|| _candidate.format != selectedSurface.format
					|| _candidate.size != selectedSurface.size
					|| _candidate.width != selectedSurface.width
					|| _candidate.height < selectedSurface.height
					|| _candidate.pixels.size() < pixelCount
					|| (isDepthOnlyColorAddress(_candidate.address)
						&& !allowDepthAliasedHistoryCarry)) {
					return 0ULL;
				}
				u64 score = 0ULL;
				for (size_t i = 0U; i < pixelCount; ++i) {
					if (selectedSurface.writeMask[i] != 0U)
						continue;
					if (pixelHasVisibleColor(_candidate.pixels[i]))
						++score;
				}
				return score;
			};

			std::vector<std::pair<const ExecutorCachedSurface *, u64>> candidateScores;
			candidateScores.reserve(m_surfaceHistory.size() + 1U);
			const auto addScoredCandidate = [&](const ExecutorCachedSurface & _candidate) {
				const u64 score = scoreCandidate(_candidate);
				if (score == 0ULL)
					return;
				for (const auto & existing : candidateScores) {
					if (existing.first != nullptr
						&& existing.first->address == _candidate.address) {
						return;
					}
				}
				candidateScores.emplace_back(&_candidate, score);
			};
			addScoredCandidate(m_lastSelectedSurface);
			for (const auto & historyEntry : m_surfaceHistory)
				addScoredCandidate(historyEntry.second);
			std::sort(
				candidateScores.begin(),
				candidateScores.end(),
				[](const std::pair<const ExecutorCachedSurface *, u64> & _a,
					const std::pair<const ExecutorCachedSurface *, u64> & _b) {
					if (_a.first == nullptr || _b.first == nullptr)
						return _a.first != nullptr;
					if (_a.second != _b.second)
						return _a.second > _b.second;
					if (_a.first->lastTouched != _b.first->lastTouched)
						return _a.first->lastTouched > _b.first->lastTouched;
					return _a.first->address < _b.first->address;
				});

			u64 copiedPixels = 0ULL;
			u32 copiedSourceAddress = 0U;
			bool copiedFromMultipleSources = false;
			for (const auto & scoredCandidate : candidateScores) {
				const ExecutorCachedSurface * source = scoredCandidate.first;
				if (source == nullptr)
					continue;
				const bool sourceHasCoverage = source->coverage.size() >= pixelCount;
				const bool sourceHasHiddenCoverage = source->hiddenCoverage.size() >= pixelCount;
				u64 copiedFromCandidate = 0ULL;
				for (size_t i = 0U; i < pixelCount; ++i) {
					if (selectedSurface.writeMask[i] != 0U)
						continue;
					const u32 sourcePixel = source->pixels[i];
					if (!pixelHasVisibleColor(sourcePixel))
						continue;
					if (selectedSurface.pixels[i] != sourcePixel) {
						selectedSurface.pixels[i] = sourcePixel;
						++copiedFromCandidate;
					}
					if (sourceHasCoverage && selectedSurface.coverage.size() >= pixelCount)
						selectedSurface.coverage[i] = static_cast<u8>(source->coverage[i] & 0x7U);
					if (sourceHasHiddenCoverage && selectedSurface.hiddenCoverage.size() >= pixelCount) {
						selectedSurface.hiddenCoverage[i] =
							static_cast<u8>(source->hiddenCoverage[i] & 0x1U);
					}
				}
				if (copiedFromCandidate == 0ULL)
					continue;
				copiedPixels += copiedFromCandidate;
				if (copiedSourceAddress == 0U)
					copiedSourceAddress = source->address;
				else if (copiedSourceAddress != source->address)
					copiedFromMultipleSources = true;
			}
			if (copiedPixels > 0ULL) {
				summary.selectedPresentSurfaceUntouchedCarryCount = copiedPixels;
				summary.selectedPresentSurfaceUntouchedCarrySourceAddress =
					copiedFromMultipleSources ? 0xFFFFFFFFU : copiedSourceAddress;
			}
		}
	}

	if (it != surfaces.end()) {
		summary.selectedPresentSurfaceFromHistory = 0U;
		summary.selectedPresentSurfaceHistoryAge = 0ULL;
		summary.selectedPresentSurfaceWidth = it->second.width;
		summary.selectedPresentSurfaceHeight = it->second.height;
		summary.selectedPresentSurfaceSize = it->second.size;
		ColorSurface & selectedSurface = it->second;
		const size_t selectedPixelCount =
			static_cast<size_t>(selectedSurface.width) * static_cast<size_t>(selectedSurface.height);
		const bool selectedSurfaceValid =
			selectedPixelCount > 0U
			&& selectedSurface.pixels.size() >= selectedPixelCount;
		const auto applyHistoryCarryFromCandidate =
			[&](
				const ExecutorCachedSurface & _candidate,
				bool _allowSameAddress,
				bool _requireRecentFrame,
				u64 & _potentialBlackFill,
				u64 & _potentialNonBlackDiff,
				u64 & _potentialUnwrittenDiff,
				u64 & _copiedPixels,
				u64 & _copiedUnwrittenPixels) -> bool {
			_potentialBlackFill = 0ULL;
			_potentialNonBlackDiff = 0ULL;
			_potentialUnwrittenDiff = 0ULL;
			_copiedPixels = 0ULL;
			_copiedUnwrittenPixels = 0ULL;
			if (!selectedSurfaceValid
				|| !_candidate.valid
				|| (!_allowSameAddress && _candidate.address == presentSurfaceAddress)
				|| (isDepthOnlyColorAddress(_candidate.address) && !allowDepthAliasedHistoryCarry)
				|| _candidate.format != selectedSurface.format
				|| _candidate.size != selectedSurface.size
				|| _candidate.width != selectedSurface.width
				|| _candidate.height < selectedSurface.height
				|| _candidate.pixels.size() < selectedPixelCount) {
				return false;
			}
			if (_requireRecentFrame) {
				const u64 candidateAge = frameStamp >= _candidate.lastTouched
					? (frameStamp - _candidate.lastTouched)
					: 0ULL;
				if (candidateAge == 0ULL || candidateAge > 8ULL)
					return false;
			}
			const bool destinationHasWriteMask =
				selectedSurface.writeMask.size() >= selectedPixelCount;
			const bool sourceHasCoverage = _candidate.coverage.size() >= selectedPixelCount;
			const bool sourceHasHiddenCoverage = _candidate.hiddenCoverage.size() >= selectedPixelCount;
			for (size_t i = 0U; i < selectedPixelCount; ++i) {
				const u32 sourcePixel = _candidate.pixels[i];
				if (!pixelHasVisibleColor(sourcePixel))
					continue;
				const u32 destinationPixel = selectedSurface.pixels[i];
				const bool destinationVisible = pixelHasVisibleColor(destinationPixel);
				const bool destinationUnwritten =
					destinationHasWriteMask && (selectedSurface.writeMask[i] == 0U);
				const u8 destinationLuma = lumaFromRGBA(destinationPixel);
				const u8 sourceLuma = lumaFromRGBA(sourcePixel);
				const bool destinationVeryDark = destinationLuma <= 4U;
				const bool sourceMuchBrighter =
					sourceLuma >= 24U
					&& static_cast<u32>(sourceLuma) > (static_cast<u32>(destinationLuma) + 16U);
				if (!destinationVisible)
					++_potentialBlackFill;
				else if (destinationPixel != sourcePixel)
					++_potentialNonBlackDiff;
				if (destinationUnwritten && destinationPixel != sourcePixel)
					++_potentialUnwrittenDiff;
				const bool allowCarryForPixel =
					!destinationVisible
					|| destinationUnwritten
					|| (destinationVeryDark && sourceMuchBrighter);
				if (!allowCarryForPixel || destinationPixel == sourcePixel)
					continue;
				selectedSurface.pixels[i] = sourcePixel;
				if (sourceHasCoverage && selectedSurface.coverage.size() >= selectedPixelCount) {
					selectedSurface.coverage[i] =
						static_cast<u8>(_candidate.coverage[i] & 0x7U);
				}
				if (sourceHasHiddenCoverage && selectedSurface.hiddenCoverage.size() >= selectedPixelCount) {
					selectedSurface.hiddenCoverage[i] =
						static_cast<u8>(_candidate.hiddenCoverage[i] & 0x1U);
				}
				++_copiedPixels;
				if (destinationUnwritten)
					++_copiedUnwrittenPixels;
			}
			return true;
		};
		const bool applySelfHistoryCarry =
			allowVIHistorySelection
			&& summary.presentSelectionReason == kExecutorPresentSelectionMostWrittenFallback
			&& summary.selectedPresentSurfaceLiveWriteCount > 0ULL
			&& historyIt != m_surfaceHistory.end()
			&& historyIt->second.address == presentSurfaceAddress;
		if (applySelfHistoryCarry) {
			u64 potentialBlackFill = 0ULL;
			u64 potentialNonBlackDiff = 0ULL;
			u64 potentialUnwrittenDiff = 0ULL;
			u64 copiedPixels = 0ULL;
			u64 copiedUnwrittenPixels = 0ULL;
			if (applyHistoryCarryFromCandidate(
					historyIt->second,
					true,
					true,
					potentialBlackFill,
					potentialNonBlackDiff,
					potentialUnwrittenDiff,
					copiedPixels,
					copiedUnwrittenPixels)) {
				summary.selectedPresentSurfaceSelfHistoryCarrySourceAddress =
					historyIt->second.address;
				summary.selectedPresentSurfaceSelfHistoryCarryPotentialBlackFillCount = potentialBlackFill;
				summary.selectedPresentSurfaceSelfHistoryCarryPotentialNonBlackDiffCount = potentialNonBlackDiff;
				summary.selectedPresentSurfaceSelfHistoryCarryPotentialUnwrittenDiffCount = potentialUnwrittenDiff;
				summary.selectedPresentSurfaceSelfHistoryCarryCopiedCount = copiedPixels;
				summary.selectedPresentSurfaceSelfHistoryCarryCopiedUnwrittenCount = copiedUnwrittenPixels;
				if (copiedPixels > 0ULL)
					summary.selectedPresentSurfaceSelfHistoryCarryApplied = 1U;
			}
		}
		const bool applyVIHistoryCandidateCarry =
			allowVIHistorySelection
			&& summary.presentSelectionReason == kExecutorPresentSelectionMostWrittenFallback
			&& summary.historyVIOriginCandidateFound != 0U
			&& summary.historyVIOriginCandidateRejectedByAge == 0U
			&& summary.historyVIOriginCandidateAddress != 0U
			&& summary.historyVIOriginCandidateAddress != presentSurfaceAddress
			&& summary.selectedPresentSurfaceLiveWriteCount > 0ULL;
		if (applyVIHistoryCandidateCarry) {
			const auto historyCandidateIt = m_surfaceHistory.find(summary.historyVIOriginCandidateAddress);
			if (historyCandidateIt != m_surfaceHistory.end()) {
				u64 potentialBlackFill = 0ULL;
				u64 potentialNonBlackDiff = 0ULL;
				u64 potentialUnwrittenDiff = 0ULL;
				u64 copiedPixels = 0ULL;
				u64 copiedUnwrittenPixels = 0ULL;
				if (applyHistoryCarryFromCandidate(
						historyCandidateIt->second,
						false,
						false,
						potentialBlackFill,
						potentialNonBlackDiff,
						potentialUnwrittenDiff,
						copiedPixels,
						copiedUnwrittenPixels)) {
					summary.selectedPresentSurfaceVIHistoryCarrySourceAddress =
						historyCandidateIt->second.address;
					summary.selectedPresentSurfaceVIHistoryCarryPotentialBlackFillCount = potentialBlackFill;
					summary.selectedPresentSurfaceVIHistoryCarryPotentialNonBlackDiffCount = potentialNonBlackDiff;
					summary.selectedPresentSurfaceVIHistoryCarryPotentialUnwrittenDiffCount = potentialUnwrittenDiff;
					summary.selectedPresentSurfaceVIHistoryCarryCopiedCount = copiedPixels;
					summary.selectedPresentSurfaceVIHistoryCarryCopiedUnwrittenCount = copiedUnwrittenPixels;
					if (copiedPixels > 0ULL)
						summary.selectedPresentSurfaceVIHistoryCarryApplied = 1U;
				}
			}
		}
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
		m_lastSelectedSurface.coverage = it->second.coverage;
		m_lastSelectedSurface.hiddenCoverage = it->second.hiddenCoverage;
	}
	else if (historyIt != m_surfaceHistory.end()) {
		summary.selectedPresentSurfaceFromHistory = 1U;
		const ExecutorCachedSurface & cached = historyIt->second;
		ExecutorCachedSurface presentCached = cached;
		summary.selectedPresentSurfaceHistoryAge = frameStamp >= cached.lastTouched
			? (frameStamp - cached.lastTouched)
			: 0ULL;
		summary.selectedPresentSurfaceWidth = cached.width;
		summary.selectedPresentSurfaceHeight = cached.height;
		summary.selectedPresentSurfaceSize = cached.size;
		const bool historyPresentMergeEnabled =
			debugEnableUntouchedPresentCarry()
			|| debugHistoryMergeCopyNonBlack()
			|| debugHistoryMergeLogPath() != nullptr;
		const size_t presentPixelCount =
			static_cast<size_t>(cached.width) * static_cast<size_t>(cached.height);
		if (historyPresentMergeEnabled
			&& presentPixelCount > 0U
			&& presentCached.pixels.size() >= presentPixelCount
			&& summary.selectedPresentSurfaceHistoryAge <= 2ULL
			&& m_surfaceHistory.size() > 1U) {
			const auto canMergeHistoryCandidate = [&](const ExecutorCachedSurface & _candidate) {
				if (!_candidate.valid
					|| _candidate.address == cached.address
					|| (isDepthOnlyColorAddress(_candidate.address)
						&& !allowDepthAliasedHistoryCarry)
					|| _candidate.format != cached.format
					|| _candidate.size != cached.size
					|| _candidate.width != cached.width
					|| _candidate.height < cached.height
					|| _candidate.pixels.size() < presentPixelCount) {
					return false;
				}
				const u64 candidateAge = frameStamp >= _candidate.lastTouched
					? (frameStamp - _candidate.lastTouched)
					: 0ULL;
				return candidateAge <= 8ULL;
			};

			std::vector<const ExecutorCachedSurface *> historyMergeCandidates;
			historyMergeCandidates.reserve(m_surfaceHistory.size());
			if (canMergeHistoryCandidate(m_lastSelectedSurface))
				historyMergeCandidates.push_back(&m_lastSelectedSurface);
			for (const auto & historyEntry : m_surfaceHistory) {
				const ExecutorCachedSurface & candidate = historyEntry.second;
				if (!canMergeHistoryCandidate(candidate))
					continue;
				if (!historyMergeCandidates.empty()
					&& candidate.address == historyMergeCandidates.front()->address) {
					continue;
				}
				historyMergeCandidates.push_back(&candidate);
			}
			std::sort(
				historyMergeCandidates.begin(),
				historyMergeCandidates.end(),
				[](const ExecutorCachedSurface * _a, const ExecutorCachedSurface * _b) {
					if (_a == nullptr || _b == nullptr)
						return _a != nullptr;
					if (_a->lastTouched != _b->lastTouched)
						return _a->lastTouched > _b->lastTouched;
					return _a->address < _b->address;
				});
			summary.selectedPresentSurfaceHistoryMergeCandidateCount =
				static_cast<u64>(historyMergeCandidates.size());

			u64 mergedHistoryPixels = 0ULL;
			u32 mergedHistorySource = 0U;
			bool mergedFromMultipleSources = false;
			const bool copyNonBlackHistoryMerge = debugHistoryMergeCopyNonBlack();
			const auto mergeHistoryCandidatePixels =
				[&](
					u32 _candidateAddress,
					const std::vector<u32> & _candidatePixels,
					const std::vector<u8> * _candidateCoverage,
					const std::vector<u8> * _candidateHiddenCoverage) {
				if (_candidatePixels.size() < presentPixelCount)
					return;
				const bool sourceHasCoverage =
					_candidateCoverage != nullptr
					&& _candidateCoverage->size() >= presentPixelCount;
				const bool sourceHasHiddenCoverage =
					_candidateHiddenCoverage != nullptr
					&& _candidateHiddenCoverage->size() >= presentPixelCount;
				u64 mergedFromCandidate = 0ULL;
				u64 potentialBlackFill = 0ULL;
				u64 potentialNonBlackDiff = 0ULL;
					for (size_t i = 0U; i < presentPixelCount; ++i) {
						const u32 sourcePixel = _candidatePixels[i];
						if (!pixelHasVisibleColor(sourcePixel))
							continue;
						const u32 destinationPixel = presentCached.pixels[i];
						const bool destinationVisible = pixelHasVisibleColor(destinationPixel);
						if (!destinationVisible)
							++potentialBlackFill;
						else if (destinationPixel != sourcePixel)
							++potentialNonBlackDiff;
						if (destinationVisible && !copyNonBlackHistoryMerge)
							continue;
						if (destinationPixel == sourcePixel)
							continue;
					presentCached.pixels[i] = sourcePixel;
					if (sourceHasCoverage && presentCached.coverage.size() >= presentPixelCount)
						presentCached.coverage[i] = static_cast<u8>((*_candidateCoverage)[i] & 0x7U);
					if (sourceHasHiddenCoverage && presentCached.hiddenCoverage.size() >= presentPixelCount) {
						presentCached.hiddenCoverage[i] =
							static_cast<u8>((*_candidateHiddenCoverage)[i] & 0x1U);
					}
					++mergedFromCandidate;
				}
				summary.selectedPresentSurfaceHistoryMergePotentialBlackFillCount += potentialBlackFill;
				summary.selectedPresentSurfaceHistoryMergePotentialNonBlackDiffCount += potentialNonBlackDiff;
				summary.selectedPresentSurfaceHistoryMergeCopiedCount += mergedFromCandidate;
				if (mergedFromCandidate > 0ULL) {
					mergedHistoryPixels += mergedFromCandidate;
					if (mergedHistorySource == 0U)
						mergedHistorySource = _candidateAddress;
					else if (mergedHistorySource != _candidateAddress)
						mergedFromMultipleSources = true;
				}
				appendHistoryMergeLog(
					frameStamp,
					cached.address,
					_candidateAddress,
					potentialBlackFill,
					potentialNonBlackDiff,
					mergedFromCandidate);
			};
			for (const ExecutorCachedSurface * candidate : historyMergeCandidates) {
				if (candidate == nullptr)
					continue;
				mergeHistoryCandidatePixels(
					candidate->address,
					candidate->pixels,
					&candidate->coverage,
					&candidate->hiddenCoverage);
			}
			if (mergedHistoryPixels > 0ULL) {
				summary.selectedPresentSurfaceUntouchedCarryCount = mergedHistoryPixels;
				summary.selectedPresentSurfaceUntouchedCarrySourceAddress =
					mergedFromMultipleSources ? 0xFFFFFFFFU : mergedHistorySource;
			}
		}
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
		presentInput.sourceAddress = presentCached.address;
		presentInput.sourceWidth = presentCached.width;
		presentInput.sourceHeight = presentCached.height;
		presentInput.sourceSize = presentCached.size;
		presentInput.sourcePixels = &presentCached.pixels;
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
		m_lastSelectedSurface = presentCached;
		m_lastSelectedSurface.lastTouched = frameStamp;
	}
	else if (allowVIHistorySelection && m_lastSelectedSurface.valid) {
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
		if (isDepthOnlyColorAddress(address) && !allowDepthAliasedHistoryCarry) {
			m_surfaceHistory.erase(address);
			if (m_lastSelectedSurface.valid && m_lastSelectedSurface.address == address)
				m_lastSelectedSurface.valid = false;
			continue;
		}
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
		cached.coverage = surface.coverage;
		cached.hiddenCoverage = surface.hiddenCoverage;
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
	gActiveExecutorFrameId = previousFrameId;
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
