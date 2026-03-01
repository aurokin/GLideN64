#include "vulkan_BlendMux.h"

#include <cstring>
#include <cstdlib>
#include <vector>

#include <GBI.h>
#include <Log.h>
#include <gDP.h>

namespace vulkan {
namespace blendmux {

namespace {

u32 packBlendMux(u32 _m1a, u32 _m1b, u32 _m2a, u32 _m2b)
{
	return (_m1a & 0x3U)
		| ((_m1b & 0x3U) << 2)
		| ((_m2a & 0x3U) << 4)
		| ((_m2b & 0x3U) << 6);
}

u32 packBlendParams(bool _texrect)
{
	const u32 forceBlendCycle1 = (gDP.otherMode.cycleType == G_CYC_2CYCLE)
		? 1U
		: static_cast<u32>(gDP.otherMode.forceBlender);
	const u32 forceBlendCycle2 = (gDP.otherMode.cycleType == G_CYC_2CYCLE)
		? static_cast<u32>(gDP.otherMode.forceBlender)
		: 0U;
	const u32 isTwoCycle = (gDP.otherMode.cycleType == G_CYC_2CYCLE) ? 1U : 0U;
	const u32 blendAlphaMode = _texrect
		? 2U
		: static_cast<u32>(gDP.otherMode.forceBlender & 0x3U);
	const u32 cvgDest = static_cast<u32>(gDP.otherMode.cvgDest & 0x3U);
	return (forceBlendCycle1 & 0x1U)
		| ((forceBlendCycle2 & 0x1U) << 1)
		| ((isTwoCycle & 0x1U) << 2)
		| ((blendAlphaMode & 0x3U) << 4)
		| ((cvgDest & 0x3U) << 6);
}

bool shouldEnableStrictBlendMux(bool _texrect)
{
	static const bool strictDualSource = std::getenv("REALITYVK_VK_STRICT_DUAL_SOURCE_BLEND") != nullptr;
	static const bool strictFramebufferFetchColor = std::getenv("REALITYVK_VK_STRICT_FB_FETCH_COLOR") != nullptr;
	if (_texrect)
		return false;
	if (gDP.otherMode.cycleType >= G_CYC_COPY)
		return false;
	return strictDualSource || strictFramebufferFetchColor;
}

bool isBlendMuxTraceEnabled()
{
	static const bool enabled = std::getenv("REALITYVK_VK_TRACE_BLEND_MUX") != nullptr;
	return enabled;
}

u32 blendMuxTraceLimit()
{
	static const u32 limit = []() -> u32 {
		const char * env = std::getenv("REALITYVK_VK_TRACE_BLEND_MUX_LIMIT");
		if (env == nullptr || env[0] == '\0')
			return 256U;
		const u32 parsed = static_cast<u32>(std::strtoul(env, nullptr, 10));
		return parsed == 0U ? 256U : parsed;
	}();
	return limit;
}

u32 blendMuxDecisionId(const char * _decision)
{
	if (_decision == nullptr)
		return 0U;
	if (std::strcmp(_decision, "disabled") == 0) return 1U;
	if (std::strcmp(_decision, "filtered_mux2") == 0) return 2U;
	if (std::strcmp(_decision, "filtered_params") == 0) return 3U;
	if (std::strcmp(_decision, "applied") == 0) return 4U;
	return 31U;
}

struct BlendMuxTraceCounter {
	u32 mux1 = 0U;
	u32 mux2 = 0U;
	u32 params = 0U;
	u32 cycleType = 0U;
	u32 texrect = 0U;
	u32 decision = 0U;
	u32 count = 0U;
};

void traceBlendMuxIntent(
	const vulkan::DrawPacket & _packet,
	bool _texrect,
	u32 _mux1,
	u32 _mux2,
	u32 _params,
	const char * _decision)
{
	if (!isBlendMuxTraceEnabled())
		return;

	static std::vector<BlendMuxTraceCounter> counters;
	static const u32 kMaxCounters = 128U;
	static u32 emitted = 0U;
	const u32 limit = blendMuxTraceLimit();
	const u32 decisionValue = blendMuxDecisionId(_decision);

	BlendMuxTraceCounter * matched = nullptr;
	for (BlendMuxTraceCounter & entry : counters) {
		if (entry.mux1 == _mux1
			&& entry.mux2 == _mux2
			&& entry.params == _params
			&& entry.cycleType == static_cast<u32>(gDP.otherMode.cycleType)
			&& entry.texrect == (_texrect ? 1U : 0U)
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
		BlendMuxTraceCounter entry{};
		entry.mux1 = _mux1;
		entry.mux2 = _mux2;
		entry.params = _params;
		entry.cycleType = static_cast<u32>(gDP.otherMode.cycleType);
		entry.texrect = _texrect ? 1U : 0U;
		entry.decision = decisionValue;
		entry.count = 1U;
		counters.push_back(entry);
		matched = &counters.back();
		shouldLog = true;
	}

	if (!shouldLog || emitted >= limit)
		return;

	const u32 count = matched != nullptr ? matched->count : 1U;
	LOG(
		LOG_WARNING,
		"VK blend mux intent: id=%llu mux1=0x%02x mux2=0x%02x params=0x%02x cycle=%u texrect=%u decision=%s count=%u",
		static_cast<unsigned long long>(_packet.debugPacketId),
		_mux1,
		_mux2,
		_params,
		static_cast<u32>(gDP.otherMode.cycleType),
		_texrect ? 1U : 0U,
		_decision != nullptr ? _decision : "unknown",
		count);
	++emitted;
	if (emitted == limit) {
		LOG(LOG_WARNING, "VK blend mux intent: limit reached (%u); suppressing further blend mux intent logs.", limit);
	}
}

} // namespace

void applyStrictBlendMuxPacketState(bool _texrect, vulkan::DrawPacket & _packet)
{
	if (!shouldEnableStrictBlendMux(_texrect)) {
		traceBlendMuxIntent(_packet, _texrect, 0U, 0U, 0U, "disabled");
		return;
	}

	_packet.shaderFlags |= vulkan::draw_shader_flags::kSpecialStrictBlendMux;
	_packet.blendMux1Packed = packBlendMux(
		static_cast<u32>(gDP.otherMode.c1_m1a),
		static_cast<u32>(gDP.otherMode.c1_m1b),
		static_cast<u32>(gDP.otherMode.c1_m2a),
		static_cast<u32>(gDP.otherMode.c1_m2b));
	_packet.blendMux2Packed = packBlendMux(
		static_cast<u32>(gDP.otherMode.c2_m1a),
		static_cast<u32>(gDP.otherMode.c2_m1b),
		static_cast<u32>(gDP.otherMode.c2_m2a),
		static_cast<u32>(gDP.otherMode.c2_m2b));
	_packet.blendParamsPacked = packBlendParams(_texrect);

	static const int strictOnlyMux2 = []() -> int {
		const char * env = std::getenv("REALITYVK_VK_STRICT_ONLY_MUX2");
		if (env == nullptr || env[0] == '\0')
			return -1;
		return static_cast<int>(std::strtol(env, nullptr, 0));
	}();
	static const int strictOnlyParams = []() -> int {
		const char * env = std::getenv("REALITYVK_VK_STRICT_ONLY_PARAMS");
		if (env == nullptr || env[0] == '\0')
			return -1;
		return static_cast<int>(std::strtol(env, nullptr, 0));
	}();
	if (strictOnlyMux2 >= 0 && static_cast<int>(_packet.blendMux2Packed) != strictOnlyMux2) {
		traceBlendMuxIntent(_packet, _texrect, _packet.blendMux1Packed, _packet.blendMux2Packed, _packet.blendParamsPacked, "filtered_mux2");
		_packet.shaderFlags &= ~vulkan::draw_shader_flags::kSpecialStrictBlendMux;
		_packet.blendMux1Packed = 0U;
		_packet.blendMux2Packed = 0U;
		_packet.blendParamsPacked = 0U;
		return;
	}
	if (strictOnlyParams >= 0 && static_cast<int>(_packet.blendParamsPacked) != strictOnlyParams) {
		traceBlendMuxIntent(_packet, _texrect, _packet.blendMux1Packed, _packet.blendMux2Packed, _packet.blendParamsPacked, "filtered_params");
		_packet.shaderFlags &= ~vulkan::draw_shader_flags::kSpecialStrictBlendMux;
		_packet.blendMux1Packed = 0U;
		_packet.blendMux2Packed = 0U;
		_packet.blendParamsPacked = 0U;
		return;
	}

	traceBlendMuxIntent(_packet, _texrect, _packet.blendMux1Packed, _packet.blendMux2Packed, _packet.blendParamsPacked, "applied");

	static const bool debugStrictBlendMux = std::getenv("REALITYVK_VK_DEBUG_STRICT_BLEND_MUX") != nullptr;
	if (debugStrictBlendMux) {
		struct StrictBlendMuxCounter {
			u32 mux1 = 0U;
			u32 mux2 = 0U;
			u32 params = 0U;
			u32 cycleType = 0U;
			u32 count = 0U;
		};
		static std::vector<StrictBlendMuxCounter> s_counters;
		static const u32 kMaxTracked = 64U;
		bool found = false;
		for (StrictBlendMuxCounter & entry : s_counters) {
			if (entry.mux1 == _packet.blendMux1Packed
				&& entry.mux2 == _packet.blendMux2Packed
				&& entry.params == _packet.blendParamsPacked
				&& entry.cycleType == gDP.otherMode.cycleType) {
				entry.count += 1U;
				if ((entry.count & (entry.count - 1U)) == 0U) {
					LOG(
						LOG_WARNING,
						"VK strict blend mux: mux1=0x%02x mux2=0x%02x params=0x%02x cycle=%u count=%u",
						entry.mux1,
						entry.mux2,
						entry.params,
						entry.cycleType,
						entry.count);
				}
				found = true;
				break;
			}
		}
		if (!found && s_counters.size() < kMaxTracked) {
			StrictBlendMuxCounter entry{};
			entry.mux1 = _packet.blendMux1Packed;
			entry.mux2 = _packet.blendMux2Packed;
			entry.params = _packet.blendParamsPacked;
			entry.cycleType = gDP.otherMode.cycleType;
			entry.count = 1U;
			s_counters.push_back(entry);
			LOG(
				LOG_WARNING,
				"VK strict blend mux: new mux1=0x%02x mux2=0x%02x params=0x%02x cycle=%u tracked=%u",
				entry.mux1,
				entry.mux2,
				entry.params,
				entry.cycleType,
				static_cast<u32>(s_counters.size()));
		}
	}
}

} // namespace blendmux
} // namespace vulkan
