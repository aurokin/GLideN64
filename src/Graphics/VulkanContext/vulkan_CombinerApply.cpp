#include "vulkan_CombinerApply.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <cstdlib>
#include <vector>

#include <Combiner.h>
#include <Config.h>
#include <Log.h>
#include <gDP.h>

#include "vulkan_CombinerClassify.h"
#include "vulkan_Env.h"

namespace vulkan {
namespace combiner {

namespace {

bool isCombinerIntentTraceEnabled()
{
	static const bool enabled = vulkan::env::flagEnabled("REALITYVK_VK_TRACE_COMBINER_INTENT", false);
	return enabled;
}

u32 combinerIntentTraceLimit()
{
	static const u32 limit = []() -> u32 {
		const char * env = std::getenv("REALITYVK_VK_TRACE_COMBINER_INTENT_LIMIT");
		if (env == nullptr || env[0] == '\0')
			return 256U;
		const u32 parsed = static_cast<u32>(std::strtoul(env, nullptr, 10));
		return parsed == 0U ? 256U : parsed;
	}();
	return limit;
}

const char * combinerInputName(u32 _input)
{
	if (_input == G_GCI_COMBINED) return "COMBINED";
	if (_input == G_GCI_TEXEL0) return "TEXEL0";
	if (_input == G_GCI_TEXEL1) return "TEXEL1";
	if (_input == G_GCI_PRIMITIVE) return "PRIMITIVE";
	if (_input == G_GCI_SHADE) return "SHADE";
	if (_input == G_GCI_ENVIRONMENT) return "ENVIRONMENT";
	if (_input == G_GCI_ONE) return "ONE";
	if (_input == G_GCI_NOISE) return "NOISE";
	if (_input == G_GCI_CENTER) return "CENTER";
	if (_input == G_GCI_K4) return "K4";
	if (_input == G_GCI_SCALE) return "SCALE";
	if (_input == G_GCI_COMBINED_ALPHA) return "COMBINED_ALPHA";
	if (_input == G_GCI_TEXEL0_ALPHA) return "TEXEL0_ALPHA";
	if (_input == G_GCI_TEXEL1_ALPHA) return "TEXEL1_ALPHA";
	if (_input == G_GCI_PRIMITIVE_ALPHA) return "PRIMITIVE_ALPHA";
	if (_input == G_GCI_SHADE_ALPHA) return "SHADE_ALPHA";
	if (_input == G_GCI_ENV_ALPHA) return "ENV_ALPHA";
	if (_input == G_GCI_LOD_FRACTION) return "LOD_FRACTION";
	if (_input == G_GCI_PRIM_LOD_FRAC) return "PRIM_LOD_FRAC";
	if (_input == G_GCI_K5) return "K5";
	return "ZERO";
}

u32 combinerIntentDecisionId(const char * _decision)
{
	if (_decision == nullptr)
		return 0U;
	if (std::strcmp(_decision, "debug_skip_drop") == 0) return 1U;
	if (std::strcmp(_decision, "debug_force_shade") == 0) return 2U;
	if (std::strcmp(_decision, "debug_force_shade_rgb") == 0) return 3U;
	if (std::strcmp(_decision, "no_override") == 0) return 4U;
	if (std::strcmp(_decision, "direct_shade") == 0) return 5U;
	if (std::strcmp(_decision, "direct_texel0") == 0) return 6U;
	if (std::strcmp(_decision, "direct_texel1") == 0) return 7U;
	if (std::strcmp(_decision, "alpha_scale") == 0) return 8U;
	if (std::strcmp(_decision, "two_cycle_shade") == 0) return 9U;
	if (std::strcmp(_decision, "two_cycle_texel0") == 0) return 10U;
	if (std::strcmp(_decision, "add_constant") == 0) return 11U;
	if (std::strcmp(_decision, "modulate_shade") == 0) return 12U;
	if (std::strcmp(_decision, "modulate_texel0") == 0) return 13U;
	if (std::strcmp(_decision, "modulate_texel1") == 0) return 14U;
	if (std::strcmp(_decision, "direct_constant") == 0) return 15U;
	if (std::strcmp(_decision, "modulate_constant") == 0) return 16U;
	return 31U;
}

struct CombinerIntentTraceCounter {
	u64 mux = 0ULL;
	u32 cycleType = 0U;
	u32 mode = 0U;
	u32 modulate = 0U;
	u32 add = 0U;
	u32 alphaMode = 0U;
	u32 alphaScale = 0U;
	u32 twoCycleSource = 0U;
	u32 twoCycleBase = 0U;
	u32 decision = 0U;
	u32 count = 0U;
};

void traceCombinerIntentDecision(
	const CombinerKey & _key,
	const vulkan::DrawPacket & _packet,
	const CombinerActiveCycleTerms * _decodedTerms,
	CombinerDirectOutputMode _mode,
	CombinerConstantModulateMode _modulateMode,
	CombinerConstantAddMode _addMode,
	CombinerDirectAlphaMode _alphaMode,
	bool _hasAlphaScaleInfo,
	const CombinerTexturedAlphaScaleInfo & _alphaScaleInfo,
	bool _hasTwoCyclePostModulateInfo,
	const CombinerTwoCyclePostModulateInfo & _twoCyclePostModulateInfo,
	const char * _decision)
{
	if (!isCombinerIntentTraceEnabled())
		return;
	if (_key == CombinerKey::getEmpty())
		return;

	static std::vector<CombinerIntentTraceCounter> counters;
	static const u32 kMaxCounters = 256U;
	static u32 emitted = 0U;
	const u32 logLimit = combinerIntentTraceLimit();

	const u32 alphaScaleValue = _hasAlphaScaleInfo ? static_cast<u32>(_alphaScaleInfo.alphaScaleMode) + 1U : 0U;
	const u32 twoCycleSourceValue = _hasTwoCyclePostModulateInfo ? static_cast<u32>(_twoCyclePostModulateInfo.source) + 1U : 0U;
	const u32 twoCycleBaseValue = _hasTwoCyclePostModulateInfo ? static_cast<u32>(_twoCyclePostModulateInfo.base) + 1U : 0U;
	const u32 decisionValue = combinerIntentDecisionId(_decision);

	CombinerIntentTraceCounter * matched = nullptr;
	for (CombinerIntentTraceCounter & entry : counters) {
		if (entry.mux == _key.getMux()
			&& entry.cycleType == _key.getCycleType()
			&& entry.mode == static_cast<u32>(_mode)
			&& entry.modulate == static_cast<u32>(_modulateMode)
			&& entry.add == static_cast<u32>(_addMode)
			&& entry.alphaMode == static_cast<u32>(_alphaMode)
			&& entry.alphaScale == alphaScaleValue
			&& entry.twoCycleSource == twoCycleSourceValue
			&& entry.twoCycleBase == twoCycleBaseValue
			&& entry.decision == decisionValue) {
			matched = &entry;
			break;
		}
	}

	bool shouldLog = false;
	if (matched != nullptr) {
		matched->count += 1U;
		shouldLog = (matched->count & (matched->count - 1U)) == 0U;
	} else if (counters.size() < kMaxCounters) {
		CombinerIntentTraceCounter entry{};
		entry.mux = _key.getMux();
		entry.cycleType = _key.getCycleType();
		entry.mode = static_cast<u32>(_mode);
		entry.modulate = static_cast<u32>(_modulateMode);
		entry.add = static_cast<u32>(_addMode);
		entry.alphaMode = static_cast<u32>(_alphaMode);
		entry.alphaScale = alphaScaleValue;
		entry.twoCycleSource = twoCycleSourceValue;
		entry.twoCycleBase = twoCycleBaseValue;
		entry.decision = decisionValue;
		entry.count = 1U;
		counters.push_back(entry);
		matched = &counters.back();
		shouldLog = true;
	}

	if (!shouldLog || emitted >= logLimit)
		return;

	const char * ca = _decodedTerms != nullptr ? combinerInputName(_decodedTerms->colorA) : "n/a";
	const char * cb = _decodedTerms != nullptr ? combinerInputName(_decodedTerms->colorB) : "n/a";
	const char * cm = _decodedTerms != nullptr ? combinerInputName(_decodedTerms->colorM) : "n/a";
	const char * cd = _decodedTerms != nullptr ? combinerInputName(_decodedTerms->colorD) : "n/a";
	const char * aa = _decodedTerms != nullptr ? combinerInputName(_decodedTerms->alphaA) : "n/a";
	const char * ab = _decodedTerms != nullptr ? combinerInputName(_decodedTerms->alphaB) : "n/a";
	const char * am = _decodedTerms != nullptr ? combinerInputName(_decodedTerms->alphaM) : "n/a";
	const char * ad = _decodedTerms != nullptr ? combinerInputName(_decodedTerms->alphaD) : "n/a";
	const u32 count = matched != nullptr ? matched->count : 1U;

	LOG(
		LOG_WARNING,
		"VK combiner intent: id=%llu mux=0x%016" PRIx64 " cycle=%u decode=[c:%s,%s,%s,%s a:%s,%s,%s,%s] classify=[direct=%u mod=%u add=%u alpha=%u alphaScale=%u twoCycleSrc=%u twoCycleBase=%u] apply=%s flags=0x%08x count=%u",
		static_cast<unsigned long long>(_packet.debugPacketId),
		static_cast<u64>(_key.getMux()),
		_key.getCycleType(),
		ca, cb, cm, cd,
		aa, ab, am, ad,
		static_cast<u32>(_mode),
		static_cast<u32>(_modulateMode),
		static_cast<u32>(_addMode),
		static_cast<u32>(_alphaMode),
		alphaScaleValue,
		twoCycleSourceValue,
		twoCycleBaseValue,
		_decision != nullptr ? _decision : "unknown",
		_packet.shaderFlags,
		count);
	++emitted;
	if (emitted == logLimit) {
		LOG(LOG_WARNING, "VK combiner intent: limit reached (%u); suppressing further combiner intent logs.", logLimit);
	}
}

} // namespace

void applyDirectAlphaOverrideForTextured(CombinerDirectAlphaMode _alphaMode, bool _primaryIsTexel0, vulkan::DrawPacket & _packet)
{
	auto applyShadeAlphaScale = [&](f32 _alphaScale, bool _keepVertexAlpha) {
		_packet.shaderFlags |= vulkan::draw_shader_flags::kShade;
		for (vulkan::DrawVertex & vertex : _packet.vertices) {
			vertex.r = 1.0f;
			vertex.g = 1.0f;
			vertex.b = 1.0f;
			if (_keepVertexAlpha) {
				vertex.a = std::max(0.0f, std::min(1.0f, vertex.a));
			} else {
				vertex.a = std::max(0.0f, std::min(1.0f, _alphaScale));
			}
		}
	};

	switch (_alphaMode) {
	case CombinerDirectAlphaMode::kTexel0:
		if (_primaryIsTexel0)
			return;
		break;
	case CombinerDirectAlphaMode::kTexel1:
		if (!_primaryIsTexel0)
			return;
		break;
	case CombinerDirectAlphaMode::kShade:
		applyShadeAlphaScale(1.0f, true);
		return;
	case CombinerDirectAlphaMode::kPrimitive:
		applyShadeAlphaScale(gDP.primColor.a, false);
		return;
	case CombinerDirectAlphaMode::kEnvironment:
		applyShadeAlphaScale(gDP.envColor.a, false);
		return;
	case CombinerDirectAlphaMode::kZero:
		applyShadeAlphaScale(0.0f, false);
		return;
	case CombinerDirectAlphaMode::kOne:
		applyShadeAlphaScale(1.0f, false);
		return;
	case CombinerDirectAlphaMode::kNone:
	default:
		return;
	}
}

void applyCombinerSolidColorOverride(const graphics::CombinerProgram * _combiner, vulkan::DrawPacket & _packet)
{
	static const bool disableCombinerSolidOverride = vulkan::env::flagEnabled("REALITYVK_VK_DISABLE_COMBINER_SOLID_OVERRIDE", false);
	if (disableCombinerSolidOverride)
		return;
	static const bool disableCombinerDirectOverride = vulkan::env::flagEnabled("REALITYVK_VK_DISABLE_COMBINER_DIRECT_OVERRIDE", false);
	static const bool disableCombinerModulateOverride = vulkan::env::flagEnabled("REALITYVK_VK_DISABLE_COMBINER_MODULATE_OVERRIDE", false);
	static const bool disableCombinerAddOverride = vulkan::env::flagEnabled("REALITYVK_VK_DISABLE_COMBINER_ADD_OVERRIDE", false);
	static const bool disableCombinerAlphaScaleOverride = vulkan::env::flagEnabled("REALITYVK_VK_DISABLE_COMBINER_ALPHA_SCALE_OVERRIDE", false);
	static const bool disableCombinerTwoCyclePostModulateOverride = vulkan::env::flagEnabled("REALITYVK_VK_DISABLE_COMBINER_TWO_CYCLE_POST_MODULATE_OVERRIDE", false);

	if (_combiner == nullptr || _packet.vertices.empty())
		return;
	const CombinerKey & key = _combiner->getKey();
	CombinerDirectOutputMode mode = CombinerDirectOutputMode::kNone;
	CombinerConstantModulateMode modulateMode = CombinerConstantModulateMode::kNone;
	CombinerConstantAddMode addMode = CombinerConstantAddMode::kNone;
	CombinerDirectAlphaMode alphaMode = CombinerDirectAlphaMode::kNone;
	CombinerTexturedAlphaScaleInfo alphaScaleInfo{};
	CombinerTwoCyclePostModulateInfo twoCyclePostModulateInfo{};
	bool hasAlphaScaleInfo = false;
	bool hasTwoCyclePostModulateInfo = false;
	CombinerActiveCycleTerms decodedTerms{};
	const bool hasDecodedTerms = decodeCombinerActiveCycleTerms(key, decodedTerms);
	auto traceIntentDecision = [&](const char * _decision) {
		traceCombinerIntentDecision(
			key,
			_packet,
			hasDecodedTerms ? &decodedTerms : nullptr,
			mode,
			modulateMode,
			addMode,
			alphaMode,
			hasAlphaScaleInfo,
			alphaScaleInfo,
			hasTwoCyclePostModulateInfo,
			twoCyclePostModulateInfo,
			_decision);
	};

	auto parseDebugMuxEnv = [](const char * _name) -> u64 {
		const char * env = std::getenv(_name);
		if (env == nullptr || env[0] == '\0')
			return 0ULL;
		return static_cast<u64>(std::strtoull(env, nullptr, 0));
	};
	static const u64 forceShadeMux = parseDebugMuxEnv("REALITYVK_VK_DEBUG_FORCE_SHADE_MUX");
	static const u64 forceShadeMux2 = parseDebugMuxEnv("REALITYVK_VK_DEBUG_FORCE_SHADE_MUX2");
	static const u64 forceShadeRgbMux = parseDebugMuxEnv("REALITYVK_VK_DEBUG_FORCE_SHADE_RGB_MUX");
	static const u64 forceShadeRgbMux2 = parseDebugMuxEnv("REALITYVK_VK_DEBUG_FORCE_SHADE_RGB_MUX2");
	static const u64 debugSkipMux = parseDebugMuxEnv("REALITYVK_VK_DEBUG_SKIP_MUX");
	static const u64 debugSkipMux2 = parseDebugMuxEnv("REALITYVK_VK_DEBUG_SKIP_MUX2");
	static const bool debugSkipMuxAlphaZeroOnly = vulkan::env::flagEnabled("REALITYVK_VK_DEBUG_SKIP_MUX_ALPHA_ZERO_ONLY", false);
	static const bool debugSkipMuxAlphaNonZeroOnly = vulkan::env::flagEnabled("REALITYVK_VK_DEBUG_SKIP_MUX_ALPHA_NONZERO_ONLY", false);
	if (!(key == CombinerKey::getEmpty())) {
		const u64 mux = key.getMux();
		if ((debugSkipMux != 0ULL && mux == debugSkipMux)
			|| (debugSkipMux2 != 0ULL && mux == debugSkipMux2)) {
			f32 maxAlpha = 0.0f;
			for (const vulkan::DrawVertex & vertex : _packet.vertices)
				maxAlpha = std::max(maxAlpha, vertex.a);
			if (debugSkipMuxAlphaZeroOnly && maxAlpha > 0.001f) {
				// Skip request targets alpha==0 packets only.
			} else if (debugSkipMuxAlphaNonZeroOnly && maxAlpha <= 0.001f) {
				// Skip request targets alpha>0 packets only.
			} else {
				_packet.vertices.clear();
				traceIntentDecision("debug_skip_drop");
				return;
			}
		}
		if ((forceShadeMux != 0ULL && mux == forceShadeMux)
			|| (forceShadeMux2 != 0ULL && mux == forceShadeMux2)) {
			_packet.shaderFlags |= vulkan::draw_shader_flags::kShade;
			_packet.shaderFlags &= ~vulkan::draw_shader_flags::kShadeRGBOnly;
			traceIntentDecision("debug_force_shade");
			return;
		}
		if ((forceShadeRgbMux != 0ULL && mux == forceShadeRgbMux)
			|| (forceShadeRgbMux2 != 0ULL && mux == forceShadeRgbMux2)) {
			_packet.shaderFlags |= vulkan::draw_shader_flags::kShade;
			_packet.shaderFlags |= vulkan::draw_shader_flags::kShadeRGBOnly;
			traceIntentDecision("debug_force_shade_rgb");
			return;
		}
	}
	_packet.shaderFlags &= ~vulkan::draw_shader_flags::kShadeRGBOnly;

	mode = detectCombinerDirectOutputMode(key);
	modulateMode = detectCombinerConstantModulateMode(key);
	addMode = detectCombinerConstantAddMode(key);
	alphaMode = detectCombinerDirectAlphaMode(key);
	alphaScaleInfo = detectCombinerTexturedAlphaScaleInfo(key);
	twoCyclePostModulateInfo = detectCombinerTwoCyclePostModulateInfo(key);
	if (disableCombinerDirectOverride)
		mode = CombinerDirectOutputMode::kNone;
	// Modulate override currently improves the Paper Mario intro gate signal on
	// the refreshed stock-reference path; keep it on by default with opt-out.
	if (disableCombinerModulateOverride)
		modulateMode = CombinerConstantModulateMode::kNone;
	if (disableCombinerAddOverride)
		addMode = CombinerConstantAddMode::kNone;
	hasAlphaScaleInfo = alphaScaleInfo.valid && !disableCombinerAlphaScaleOverride;
	hasTwoCyclePostModulateInfo = twoCyclePostModulateInfo.valid() && !disableCombinerTwoCyclePostModulateOverride;
	static const bool debugCombinerOverrides = vulkan::env::flagEnabled("REALITYVK_VK_DEBUG_COMBINER_OVERRIDES", false);
	if (debugCombinerOverrides && !(key == CombinerKey::getEmpty())) {
		struct CombinerOverrideCounter {
			u64 mux = 0ULL;
			u32 cycleType = 0U;
			u32 mode = 0U;
			u32 modulateMode = 0U;
			u32 addMode = 0U;
			u32 alphaScale = 0U;
			u32 twoCyclePost = 0U;
			u32 count = 0U;
		};
		static std::vector<CombinerOverrideCounter> counters;
		static const u32 kMaxCounters = 256U;
		const u64 mux = key.getMux();
		const u32 cycleType = key.getCycleType();
		const u32 modeValue = static_cast<u32>(mode);
		const u32 modulateValue = static_cast<u32>(modulateMode);
		const u32 addValue = static_cast<u32>(addMode);
		const u32 alphaScaleValue = hasAlphaScaleInfo ? static_cast<u32>(alphaScaleInfo.alphaScaleMode) + 1U : 0U;
		const u32 twoCyclePostValue = hasTwoCyclePostModulateInfo ? static_cast<u32>(twoCyclePostModulateInfo.base) + 1U : 0U;
		bool found = false;
		for (CombinerOverrideCounter & entry : counters) {
			if (entry.mux == mux
				&& entry.cycleType == cycleType
				&& entry.mode == modeValue
				&& entry.modulateMode == modulateValue
				&& entry.addMode == addValue
				&& entry.alphaScale == alphaScaleValue
				&& entry.twoCyclePost == twoCyclePostValue) {
				entry.count += 1U;
				if ((entry.count & (entry.count - 1U)) == 0U) {
					LOG(
						LOG_WARNING,
						"VK combiner override: mux=0x%016" PRIx64 " cycle=%u mode=%u mod=%u add=%u alphaScale=%u twoCyclePost=%u count=%u",
						static_cast<u64>(entry.mux),
						entry.cycleType,
						entry.mode,
						entry.modulateMode,
						entry.addMode,
						entry.alphaScale,
						entry.twoCyclePost,
						entry.count);
				}
				found = true;
				break;
			}
		}
		if (!found && counters.size() < kMaxCounters) {
			CombinerOverrideCounter entry{};
			entry.mux = mux;
			entry.cycleType = cycleType;
			entry.mode = modeValue;
			entry.modulateMode = modulateValue;
			entry.addMode = addValue;
			entry.alphaScale = alphaScaleValue;
			entry.twoCyclePost = twoCyclePostValue;
			entry.count = 1U;
			counters.push_back(entry);
			LOG(
				LOG_WARNING,
				"VK combiner override: new mux=0x%016" PRIx64 " cycle=%u mode=%u mod=%u add=%u alphaScale=%u twoCyclePost=%u tracked=%u",
				static_cast<u64>(entry.mux),
				entry.cycleType,
				entry.mode,
				entry.modulateMode,
				entry.addMode,
				entry.alphaScale,
				entry.twoCyclePost,
				static_cast<u32>(counters.size()));
		}
	}
	if (mode == CombinerDirectOutputMode::kNone
		&& modulateMode == CombinerConstantModulateMode::kNone
		&& addMode == CombinerConstantAddMode::kNone
		&& !hasAlphaScaleInfo
		&& !hasTwoCyclePostModulateInfo) {
		static const bool debugCombinerCoverage = vulkan::env::flagEnabled("REALITYVK_VK_DEBUG_COMBINER_COVERAGE", false);
		if (debugCombinerCoverage) {
			struct UnhandledCombinerKeyCounter {
				u64 mux = 0ULL;
				u32 cycleType = 0U;
				u32 count = 0U;
			};
			static std::vector<UnhandledCombinerKeyCounter> s_unhandledKeys;
			static const u32 kMaxTrackedKeys = 128U;
			if (!(key == CombinerKey::getEmpty())
				&& key.getCycleType() != G_CYC_COPY
				&& key.getCycleType() != G_CYC_FILL) {
				bool found = false;
				for (UnhandledCombinerKeyCounter & entry : s_unhandledKeys) {
					if (entry.mux == key.getMux() && entry.cycleType == key.getCycleType()) {
						entry.count += 1U;
						if ((entry.count & (entry.count - 1U)) == 0U) {
							LOG(
								LOG_WARNING,
								"VK combiner coverage: unresolved mux=0x%016" PRIx64 " cycle=%u count=%u",
								static_cast<u64>(entry.mux),
								entry.cycleType,
								entry.count);
						}
						found = true;
						break;
					}
				}
				if (!found && s_unhandledKeys.size() < kMaxTrackedKeys) {
					UnhandledCombinerKeyCounter entry{};
					entry.mux = key.getMux();
					entry.cycleType = key.getCycleType();
					entry.count = 1U;
					s_unhandledKeys.push_back(entry);
					LOG(
						LOG_WARNING,
						"VK combiner coverage: new unresolved mux=0x%016" PRIx64 " cycle=%u tracked=%u",
						static_cast<u64>(entry.mux),
						entry.cycleType,
						static_cast<u32>(s_unhandledKeys.size()));
				}
			}
		}
		traceIntentDecision("no_override");
		return;
	}

	if (mode == CombinerDirectOutputMode::kShade && modulateMode == CombinerConstantModulateMode::kNone) {
		_packet.shaderFlags &= ~(vulkan::draw_shader_flags::kTexture0 | vulkan::draw_shader_flags::kTexture1);
		_packet.shaderFlags |= vulkan::draw_shader_flags::kShade;
		traceIntentDecision("direct_shade");
		return;
	}

	if (mode == CombinerDirectOutputMode::kTexel0 && modulateMode == CombinerConstantModulateMode::kNone) {
		_packet.shaderFlags &= ~(vulkan::draw_shader_flags::kTexture1
			| vulkan::draw_shader_flags::kShade
			| vulkan::draw_shader_flags::kShadeRGBOnly);
		_packet.shaderFlags |= vulkan::draw_shader_flags::kTexture0;
		applyDirectAlphaOverrideForTextured(alphaMode, true, _packet);
		traceIntentDecision("direct_texel0");
		return;
	}

	if (mode == CombinerDirectOutputMode::kTexel1 && modulateMode == CombinerConstantModulateMode::kNone) {
		_packet.shaderFlags &= ~(vulkan::draw_shader_flags::kTexture0
			| vulkan::draw_shader_flags::kShade
			| vulkan::draw_shader_flags::kShadeRGBOnly);
		_packet.shaderFlags |= vulkan::draw_shader_flags::kTexture1;
		applyDirectAlphaOverrideForTextured(alphaMode, false, _packet);
		traceIntentDecision("direct_texel1");
		return;
	}

	if (hasAlphaScaleInfo && modulateMode == CombinerConstantModulateMode::kNone) {
		_packet.shaderFlags &= ~(vulkan::draw_shader_flags::kTexture0
			| vulkan::draw_shader_flags::kTexture1
			| vulkan::draw_shader_flags::kShadeRGBOnly);
		if (alphaScaleInfo.primaryIsTexel0) {
			_packet.shaderFlags |= vulkan::draw_shader_flags::kTexture0;
		} else {
			_packet.shaderFlags |= vulkan::draw_shader_flags::kTexture1;
		}
		applyDirectAlphaOverrideForTextured(alphaScaleInfo.alphaScaleMode, alphaScaleInfo.primaryIsTexel0, _packet);
		traceIntentDecision("alpha_scale");
		return;
	}

	auto clamp01 = [](f32 _v) -> f32 {
		return std::max(0.0f, std::min(1.0f, _v));
	};

	if (hasTwoCyclePostModulateInfo) {
		f32 scaleR = 1.0f;
		f32 scaleG = 1.0f;
		f32 scaleB = 1.0f;
		if (twoCyclePostModulateInfo.source == CombinerPostModulateConstantSource::kPrimitive) {
			scaleR = gDP.primColor.r;
			scaleG = gDP.primColor.g;
			scaleB = gDP.primColor.b;
		} else {
			scaleR = gDP.envColor.r;
			scaleG = gDP.envColor.g;
			scaleB = gDP.envColor.b;
		}

		if (twoCyclePostModulateInfo.base == CombinerTwoCyclePostModulateBase::kShadeAlphaShade) {
			_packet.shaderFlags &= ~(vulkan::draw_shader_flags::kTexture0
				| vulkan::draw_shader_flags::kTexture1
				| vulkan::draw_shader_flags::kShadeRGBOnly);
			_packet.shaderFlags |= vulkan::draw_shader_flags::kShade;
			for (vulkan::DrawVertex & vertex : _packet.vertices) {
				vertex.r = clamp01(vertex.r * scaleR);
				vertex.g = clamp01(vertex.g * scaleG);
				vertex.b = clamp01(vertex.b * scaleB);
			}
			traceIntentDecision("two_cycle_shade");
			return;
		}

		if (twoCyclePostModulateInfo.base == CombinerTwoCyclePostModulateBase::kTexel0ShadeAlphaTexel0Shade
			|| twoCyclePostModulateInfo.base == CombinerTwoCyclePostModulateBase::kTexel0ShadeAlphaTexel0) {
			_packet.shaderFlags &= ~vulkan::draw_shader_flags::kTexture1;
			_packet.shaderFlags |= vulkan::draw_shader_flags::kTexture0 | vulkan::draw_shader_flags::kShade;
			if (twoCyclePostModulateInfo.base == CombinerTwoCyclePostModulateBase::kTexel0ShadeAlphaTexel0) {
				_packet.shaderFlags |= vulkan::draw_shader_flags::kShadeRGBOnly;
			} else {
				_packet.shaderFlags &= ~vulkan::draw_shader_flags::kShadeRGBOnly;
			}
			for (vulkan::DrawVertex & vertex : _packet.vertices) {
				vertex.r = clamp01(vertex.r * scaleR);
				vertex.g = clamp01(vertex.g * scaleG);
				vertex.b = clamp01(vertex.b * scaleB);
				if (twoCyclePostModulateInfo.base == CombinerTwoCyclePostModulateBase::kTexel0ShadeAlphaTexel0)
					vertex.a = 1.0f;
			}
			traceIntentDecision("two_cycle_texel0");
			return;
		}
	}

	if (addMode != CombinerConstantAddMode::kNone
		&& (_packet.shaderFlags & vulkan::draw_shader_flags::kSpecialStrictBlendMux) == 0U) {
		f32 addR = 0.0f;
		f32 addG = 0.0f;
		f32 addB = 0.0f;
		f32 addA = 0.0f;
		if (addMode == CombinerConstantAddMode::kPrimitiveTexel0
			|| addMode == CombinerConstantAddMode::kPrimitiveTexel1
			|| addMode == CombinerConstantAddMode::kPrimitiveShade) {
			addR = gDP.primColor.r;
			addG = gDP.primColor.g;
			addB = gDP.primColor.b;
			addA = gDP.primColor.a;
		} else {
			addR = gDP.envColor.r;
			addG = gDP.envColor.g;
			addB = gDP.envColor.b;
			addA = gDP.envColor.a;
		}

		_packet.shaderFlags &= ~(vulkan::draw_shader_flags::kTexture0
			| vulkan::draw_shader_flags::kTexture1
			| vulkan::draw_shader_flags::kShade);
		if (addMode == CombinerConstantAddMode::kPrimitiveTexel0
			|| addMode == CombinerConstantAddMode::kEnvironmentTexel0) {
			_packet.shaderFlags |= vulkan::draw_shader_flags::kTexture0;
		} else if (addMode == CombinerConstantAddMode::kPrimitiveTexel1
			|| addMode == CombinerConstantAddMode::kEnvironmentTexel1) {
			_packet.shaderFlags |= vulkan::draw_shader_flags::kTexture1;
		} else {
			_packet.shaderFlags |= vulkan::draw_shader_flags::kShade;
		}
		_packet.shaderFlags |= vulkan::draw_shader_flags::kCombinerAddConstant;
		_packet.fogColorR = addR;
		_packet.fogColorG = addG;
		_packet.fogColorB = addB;
		_packet.fogColorA = addA;
		traceIntentDecision("add_constant");
		return;
	}

	f32 r = 1.0f;
	f32 g = 1.0f;
	f32 b = 1.0f;
	f32 a = 1.0f;
	if (modulateMode == CombinerConstantModulateMode::kNone) {
		if (mode == CombinerDirectOutputMode::kZero) {
			r = 0.0f;
			g = 0.0f;
			b = 0.0f;
			a = 0.0f;
		} else if (mode == CombinerDirectOutputMode::kOne) {
			r = 1.0f;
			g = 1.0f;
			b = 1.0f;
			a = 1.0f;
		} else if (mode == CombinerDirectOutputMode::kPrimitive) {
			r = gDP.primColor.r;
			g = gDP.primColor.g;
			b = gDP.primColor.b;
			a = gDP.primColor.a;
		} else if (mode == CombinerDirectOutputMode::kEnvironment) {
			r = gDP.envColor.r;
			g = gDP.envColor.g;
			b = gDP.envColor.b;
			a = gDP.envColor.a;
		} else {
			traceIntentDecision("no_override");
			return;
		}
		_packet.shaderFlags &= ~(vulkan::draw_shader_flags::kTexture0 | vulkan::draw_shader_flags::kTexture1);
		_packet.shaderFlags |= vulkan::draw_shader_flags::kShade;
	} else {
		if (modulateMode == CombinerConstantModulateMode::kPrimitiveTexel0
			|| modulateMode == CombinerConstantModulateMode::kPrimitiveTexel1) {
			r = gDP.primColor.r;
			g = gDP.primColor.g;
			b = gDP.primColor.b;
			a = gDP.primColor.a;
		} else {
			r = gDP.envColor.r;
			g = gDP.envColor.g;
			b = gDP.envColor.b;
			a = gDP.envColor.a;
		}
		static const bool debugModulateState = vulkan::env::flagEnabled("REALITYVK_VK_DEBUG_MODULATE_STATE", false);
		if (debugModulateState && !(key == CombinerKey::getEmpty())) {
			static u32 debugModulateLogCount = 0U;
			static const u32 debugModulateLogLimit = []() -> u32 {
				const char * env = std::getenv("REALITYVK_VK_DEBUG_MODULATE_STATE_LIMIT");
				if (env == nullptr || env[0] == '\0')
					return 96U;
				return static_cast<u32>(std::strtoul(env, nullptr, 10));
			}();
			if (debugModulateLogCount < debugModulateLogLimit) {
				const vulkan::DrawVertex & firstVertex = _packet.vertices.front();
				LOG(
					LOG_WARNING,
					"VK combiner modulate state: id=%llu mux=0x%016" PRIx64 " mode=%u prim=[%.3f,%.3f,%.3f,%.3f] env=[%.3f,%.3f,%.3f,%.3f] firstVertex=[%.3f,%.3f,%.3f,%.3f]",
					static_cast<unsigned long long>(_packet.debugPacketId),
					static_cast<u64>(key.getMux()),
					static_cast<u32>(modulateMode),
					gDP.primColor.r, gDP.primColor.g, gDP.primColor.b, gDP.primColor.a,
					gDP.envColor.r, gDP.envColor.g, gDP.envColor.b, gDP.envColor.a,
					firstVertex.r, firstVertex.g, firstVertex.b, firstVertex.a);
				++debugModulateLogCount;
			}
		}

		if (modulateMode == CombinerConstantModulateMode::kPrimitiveShade
			|| modulateMode == CombinerConstantModulateMode::kEnvironmentShade) {
			_packet.shaderFlags &= ~(vulkan::draw_shader_flags::kTexture0 | vulkan::draw_shader_flags::kTexture1);
			_packet.shaderFlags |= vulkan::draw_shader_flags::kShade;
			for (vulkan::DrawVertex & vertex : _packet.vertices) {
				vertex.r = clamp01(vertex.r * r);
				vertex.g = clamp01(vertex.g * g);
				vertex.b = clamp01(vertex.b * b);
				vertex.a = clamp01(vertex.a * a);
			}
			traceIntentDecision("modulate_shade");
			return;
		}

		if (modulateMode == CombinerConstantModulateMode::kPrimitiveTexel0
			|| modulateMode == CombinerConstantModulateMode::kEnvironmentTexel0) {
			_packet.shaderFlags &= ~vulkan::draw_shader_flags::kTexture1;
			_packet.shaderFlags |= vulkan::draw_shader_flags::kTexture0 | vulkan::draw_shader_flags::kShade;
			applyDirectAlphaOverrideForTextured(alphaMode, true, _packet);
		} else {
			_packet.shaderFlags &= ~vulkan::draw_shader_flags::kTexture0;
			_packet.shaderFlags |= vulkan::draw_shader_flags::kTexture1 | vulkan::draw_shader_flags::kShade;
			applyDirectAlphaOverrideForTextured(alphaMode, false, _packet);
		}
	}

	for (vulkan::DrawVertex & vertex : _packet.vertices) {
		vertex.r = r;
		vertex.g = g;
		vertex.b = b;
		vertex.a = a;
	}
	traceIntentDecision(modulateMode == CombinerConstantModulateMode::kNone ? "direct_constant" : "modulate_constant");
}

} // namespace combiner
} // namespace vulkan
