#include "vulkan_CombinerHeuristics.h"

#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <vector>

#include <Combiner.h>
#include <Config.h>
#include <GBI.h>
#include <Log.h>
#include <gDP.h>

#include "vulkan_CombinerDecode.h"
#include "vulkan_DrawRecorder.h"
#include "vulkan_Env.h"

namespace vulkan {
namespace combiner {

// Heuristic RDP combiner mapping based on the documented N64 combiner equation:
// (A - B) * C + D across one- and two-cycle modes.
class VulkanInferredCombinerProgram final : public graphics::CombinerProgram
{
public:
	explicit VulkanInferredCombinerProgram(const CombinerKey & _key)
		: m_key(_key)
	{
		_initUsageFromKey();
	}

	void activate() override {}

	void update(bool _force) override
	{
		(void)_force;
	}

	const CombinerKey & getKey() const override { return m_key; }
	bool usesTexture() const override { return m_usesTexture; }
	bool usesTile(u32 _t) const override
	{
		if (_t == 0U)
			return m_usesTile0;
		if (_t == 1U)
			return m_usesTile1;
		return false;
	}
	bool usesShade() const override { return m_usesShade; }
	bool usesLOD() const override { return m_usesLOD; }
	bool usesHwLighting() const override { return m_usesHwLighting; }
	bool getBinaryForm(std::vector<char> & _buffer) override
	{
		_buffer.clear();
		return false;
	}

private:
	void _markExpandedInput(u32 _input)
	{
		if (_input == G_GCI_TEXEL0 || _input == G_GCI_TEXEL0_ALPHA)
			m_usesTile0 = true;
		if (_input == G_GCI_TEXEL1 || _input == G_GCI_TEXEL1_ALPHA)
			m_usesTile1 = true;
		if (_input == G_GCI_SHADE || _input == G_GCI_SHADE_ALPHA)
			m_usesShade = true;
		if (_input == G_GCI_LOD_FRACTION)
			m_usesLOD = true;
	}

	void _initUsageFromKey()
	{
		gDPCombine decoded{};
		decoded.mux = m_key.getMux();
		_markExpandedInput(expandCombinerColorAForSolidCheck(decoded.saRGB0));
		_markExpandedInput(expandCombinerColorBForSolidCheck(decoded.sbRGB0));
		_markExpandedInput(expandCombinerColorMForSolidCheck(decoded.mRGB0));
		_markExpandedInput(expandCombinerColorDForSolidCheck(decoded.aRGB0));
		_markExpandedInput(expandCombinerAlphaAForSolidCheck(decoded.saA0));
		_markExpandedInput(expandCombinerAlphaBForSolidCheck(decoded.sbA0));
		_markExpandedInput(expandCombinerAlphaMForSolidCheck(decoded.mA0));
		_markExpandedInput(expandCombinerAlphaDForSolidCheck(decoded.aA0));
		if (m_key.getCycleType() == G_CYC_2CYCLE) {
			_markExpandedInput(expandCombinerColorAForSolidCheck(decoded.saRGB1));
			_markExpandedInput(expandCombinerColorBForSolidCheck(decoded.sbRGB1));
			_markExpandedInput(expandCombinerColorMForSolidCheck(decoded.mRGB1));
			_markExpandedInput(expandCombinerColorDForSolidCheck(decoded.aRGB1));
			_markExpandedInput(expandCombinerAlphaAForSolidCheck(decoded.saA1));
			_markExpandedInput(expandCombinerAlphaBForSolidCheck(decoded.sbA1));
			_markExpandedInput(expandCombinerAlphaMForSolidCheck(decoded.mA1));
			_markExpandedInput(expandCombinerAlphaDForSolidCheck(decoded.aA1));
		}
		m_usesTexture = m_usesTile0 || m_usesTile1;
		m_usesHwLighting = m_key.isHWLSupported() && m_usesShade;
	}

	CombinerKey m_key;
	bool m_usesTexture = false;
	bool m_usesTile0 = false;
	bool m_usesTile1 = false;
	bool m_usesShade = false;
	bool m_usesLOD = false;
	bool m_usesHwLighting = false;
};

graphics::CombinerProgram * createInferredCombinerProgram(const CombinerKey & _key)
{
	return new VulkanInferredCombinerProgram(_key);
}

u32 resolveShaderFlags(const graphics::CombinerProgram * _combiner, bool _defaultShade, bool _defaultTexture0)
{
	static const bool forceShade = vulkan::env::flagEnabled("REALITYVK_VK_DEBUG_FORCE_SHADE", false);
	static const bool debugCombiners = vulkan::env::flagEnabled("REALITYVK_VK_DEBUG_COMBINERS", false);
	u32 flags = _defaultShade ? vulkan::draw_shader_flags::kShade : 0U;
	if (_defaultTexture0)
		flags |= vulkan::draw_shader_flags::kTexture0;
	if (_combiner == nullptr)
		return flags;

	// First combiner semantic slice: honor canonical FILL/COPY cycle intent
	// from combiner keys before generic usage inference.
	const CombinerKey & key = _combiner->getKey();
	const bool hasCombinerKey = !(key == CombinerKey::getEmpty());
	if (hasCombinerKey) {
		if (key.getCycleType() == G_CYC_FILL) {
			flags &= ~(vulkan::draw_shader_flags::kTexture0 | vulkan::draw_shader_flags::kTexture1);
			flags |= vulkan::draw_shader_flags::kShade;
			return flags;
		}
		if (key.getCycleType() == G_CYC_COPY) {
			flags &= ~(vulkan::draw_shader_flags::kTexture1 | vulkan::draw_shader_flags::kShade);
			flags |= vulkan::draw_shader_flags::kTexture0;
			return flags;
		}
	}

	if (_combiner->usesShade())
		flags |= vulkan::draw_shader_flags::kShade;
	else
		flags &= ~vulkan::draw_shader_flags::kShade;

	flags &= ~(vulkan::draw_shader_flags::kTexture0 | vulkan::draw_shader_flags::kTexture1);
	if (!_combiner->usesTexture())
		return flags;

	bool mappedTile = false;
	if (_combiner->usesTile(0)) {
		flags |= vulkan::draw_shader_flags::kTexture0;
		mappedTile = true;
	}
	if (_combiner->usesTile(1)) {
		flags |= vulkan::draw_shader_flags::kTexture1;
		mappedTile = true;
	}
	if (!mappedTile)
		flags |= vulkan::draw_shader_flags::kTexture0;
	if (forceShade)
		flags |= vulkan::draw_shader_flags::kShade;
	if (debugCombiners) {
		static const u32 logLimit = []() -> u32 {
			const char * env = std::getenv("REALITYVK_VK_DEBUG_COMBINERS_LIMIT");
			if (env == nullptr || env[0] == '\0')
				return 256U;
			return static_cast<u32>(std::strtoul(env, nullptr, 10));
		}();
		static u32 logCount = 0U;
		if (logCount < logLimit) {
			const CombinerKey & key = _combiner->getKey();
			LOG(
				LOG_WARNING,
				"VK combiner debug: mux=0x%016llx cycleType=%u usesTexture=%u tile0=%u tile1=%u usesShade=%u flags=0x%08x",
				static_cast<unsigned long long>(key.getMux()),
				key.getCycleType(),
				_combiner->usesTexture() ? 1U : 0U,
				_combiner->usesTile(0U) ? 1U : 0U,
				_combiner->usesTile(1U) ? 1U : 0U,
				_combiner->usesShade() ? 1U : 0U,
				flags);
			++logCount;
		}
	}
	return flags;
}

} // namespace combiner
} // namespace vulkan
