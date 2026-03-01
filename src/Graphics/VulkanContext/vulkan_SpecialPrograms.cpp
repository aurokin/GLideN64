#include "vulkan_SpecialPrograms.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>

#include <Config.h>
#include <PaletteTexture.h>
#include <gDP.h>

namespace vulkan {
namespace special_programs {

namespace {

class VulkanSpecialShaderProgram : public graphics::ShaderProgram
{
public:
	VulkanSpecialShaderProgram(bool _usesTexture, bool _usesTile0, bool _usesTile1, bool _usesShade)
		: m_usesTexture(_usesTexture)
		, m_usesTile0(_usesTile0)
		, m_usesTile1(_usesTile1)
		, m_usesShade(_usesShade)
	{
	}

	void activate() override {}
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

private:
	bool m_usesTexture = false;
	bool m_usesTile0 = false;
	bool m_usesTile1 = false;
	bool m_usesShade = false;
};

class VulkanTexrectDrawerClearShaderProgram final : public VulkanSpecialShaderProgram
{
public:
	VulkanTexrectDrawerClearShaderProgram()
		: VulkanSpecialShaderProgram(false, false, false, true)
	{
	}
};

class VulkanTexrectDrawerShaderProgram final : public graphics::TexrectDrawerShaderProgram
{
public:
	void activate() override {}
	void update(bool _force) override { (void)_force; }
	const CombinerKey & getKey() const override { return CombinerKey::getEmpty(); }
	bool usesTexture() const override { return true; }
	bool usesTile(u32 _t) const override { return _t == 0U; }
	bool usesShade() const override { return false; }
	bool usesLOD() const override { return false; }
	bool usesHwLighting() const override { return false; }
	bool getBinaryForm(std::vector<char> & _buffer) override { _buffer.clear(); return false; }
	void setTextureSize(u32 _width, u32 _height) override
	{
		m_textureWidth = _width > 0U ? _width : 1U;
		m_textureHeight = _height > 0U ? _height : 1U;
	}
	void setEnableAlphaTest(int _enable) override
	{
		m_enableAlphaTest = (_enable != 0) ? 1U : 0U;
	}
	u32 alphaTestEnabled() const { return m_enableAlphaTest; }
	u32 textureWidth() const { return m_textureWidth; }
	u32 textureHeight() const { return m_textureHeight; }
	u32 filterMode() const
	{
		switch (config.texture.bilinearMode) {
		case BILINEAR_STANDARD:
		case BILINEAR_STANDARD_WITH_COLOR_BLEEDING_AND_PREMULTIPLIED_ALPHA:
			return 2U;
		case BILINEAR_3POINT:
		case BILINEAR_3POINT_WITH_COLOR_BLEEDING:
		default:
			return 1U;
		}
	}

private:
	u32 m_enableAlphaTest = 0U;
	u32 m_textureWidth = 1U;
	u32 m_textureHeight = 1U;
};

class VulkanTexrectColorDepthCopyShaderProgram final : public VulkanSpecialShaderProgram
{
public:
	VulkanTexrectColorDepthCopyShaderProgram()
		: VulkanSpecialShaderProgram(true, true, true, false)
	{
	}
};

class VulkanDepthFogShaderProgram final : public VulkanSpecialShaderProgram
{
public:
	VulkanDepthFogShaderProgram()
		: VulkanSpecialShaderProgram(true, true, false, false)
	{
	}

	void activate() override
	{
		// Upstream updates the palette texture right before depth-fog draws.
		g_paletteTexture.update();
	}
};

class VulkanGammaCorrectionShaderProgram final : public VulkanSpecialShaderProgram
{
public:
	VulkanGammaCorrectionShaderProgram()
		: VulkanSpecialShaderProgram(true, true, false, false)
	{
		m_gammaLevel = (config.gammaCorrection.force != 0) ? config.gammaCorrection.level : 2.0f;
		if (m_gammaLevel <= 0.0f)
			m_gammaLevel = 2.0f;
	}

	f32 gammaLevel() const { return m_gammaLevel; }

private:
	f32 m_gammaLevel = 2.0f;
};

class VulkanFXAAShaderProgram final : public VulkanSpecialShaderProgram
{
public:
	VulkanFXAAShaderProgram()
		: VulkanSpecialShaderProgram(true, true, false, false)
	{
	}
};

class VulkanTextDrawerShaderProgram final : public graphics::TextDrawerShaderProgram
{
public:
	VulkanTextDrawerShaderProgram()
	{
		std::memcpy(m_color, config.font.colorf, sizeof(m_color));
	}

	void activate() override {}
	void update(bool _force) override { (void)_force; }
	const CombinerKey & getKey() const override { return CombinerKey::getEmpty(); }
	bool usesTexture() const override { return true; }
	bool usesTile(u32 _t) const override { return _t == 0U; }
	bool usesShade() const override { return false; }
	bool usesLOD() const override { return false; }
	bool usesHwLighting() const override { return false; }
	bool getBinaryForm(std::vector<char> & _buffer) override { _buffer.clear(); return false; }
	void setTextColor(float * _color) override
	{
		if (_color == nullptr)
			return;
		std::memcpy(m_color, _color, sizeof(m_color));
	}
	const f32 * textColor() const { return m_color; }

private:
	f32 m_color[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
};

} // namespace

graphics::ShaderProgram * createDepthFogShader()
{
	return new VulkanDepthFogShaderProgram;
}

graphics::TexrectDrawerShaderProgram * createTexrectDrawerDrawShader()
{
	return new VulkanTexrectDrawerShaderProgram;
}

graphics::ShaderProgram * createTexrectDrawerClearShader()
{
	return new VulkanTexrectDrawerClearShaderProgram;
}

graphics::ShaderProgram * createTexrectUpscaleCopyShader()
{
	return new VulkanSpecialShaderProgram(true, true, false, false);
}

graphics::ShaderProgram * createTexrectColorAndDepthUpscaleCopyShader()
{
	return new VulkanTexrectColorDepthCopyShaderProgram;
}

graphics::ShaderProgram * createTexrectDownscaleCopyShader()
{
	return new VulkanSpecialShaderProgram(true, true, false, false);
}

graphics::ShaderProgram * createTexrectColorAndDepthDownscaleCopyShader()
{
	return new VulkanTexrectColorDepthCopyShaderProgram;
}

graphics::ShaderProgram * createGammaCorrectionShader()
{
	return new VulkanGammaCorrectionShaderProgram;
}

graphics::ShaderProgram * createFXAAShader()
{
	return new VulkanFXAAShaderProgram;
}

graphics::TextDrawerShaderProgram * createTextDrawerShader()
{
	return new VulkanTextDrawerShaderProgram;
}

bool isTexrectClearProgram(const graphics::CombinerProgram * _combiner)
{
	return dynamic_cast<const VulkanTexrectDrawerClearShaderProgram *>(_combiner) != nullptr;
}

void applyRectSpecialProgramState(const graphics::CombinerProgram * _combiner, vulkan::DrawPacket & _packet)
{
	static const bool disableSpecialTexrect = std::getenv("REALITYVK_VK_DISABLE_SPECIAL_TEXRECT") != nullptr;
	static const bool disableSpecialGamma = std::getenv("REALITYVK_VK_DISABLE_SPECIAL_GAMMA") != nullptr;
	static const bool disableSpecialFXAA = std::getenv("REALITYVK_VK_DISABLE_SPECIAL_FXAA") != nullptr;

	const auto * texrectDrawProgram = dynamic_cast<const VulkanTexrectDrawerShaderProgram *>(_combiner);
	if (texrectDrawProgram != nullptr && !disableSpecialTexrect) {
		_packet.shaderFlags |= vulkan::draw_shader_flags::kSpecialTexrectDraw;
		_packet.texrectAlphaTest = texrectDrawProgram->alphaTestEnabled();
		_packet.texrectFilterMode = texrectDrawProgram->filterMode();
		_packet.texrectTextureWidth = static_cast<f32>(std::max<u32>(1U, texrectDrawProgram->textureWidth()));
		_packet.texrectTextureHeight = static_cast<f32>(std::max<u32>(1U, texrectDrawProgram->textureHeight()));
	}

	if (dynamic_cast<const VulkanTexrectColorDepthCopyShaderProgram *>(_combiner) != nullptr)
		_packet.shaderFlags |= vulkan::draw_shader_flags::kSpecialDepthFromTexture1;

	const auto * gammaProgram = dynamic_cast<const VulkanGammaCorrectionShaderProgram *>(_combiner);
	if (gammaProgram != nullptr && !disableSpecialGamma) {
		_packet.shaderFlags |= vulkan::draw_shader_flags::kSpecialGammaCorrection;
		_packet.gammaLevel = std::max(0.001f, gammaProgram->gammaLevel());
	}

	if (!disableSpecialFXAA && dynamic_cast<const VulkanFXAAShaderProgram *>(_combiner) != nullptr)
		_packet.shaderFlags |= vulkan::draw_shader_flags::kSpecialFXAA;

	if (dynamic_cast<const VulkanDepthFogShaderProgram *>(_combiner) != nullptr) {
		_packet.shaderFlags |= vulkan::draw_shader_flags::kSpecialDepthFog;
		_packet.fogColorR = gDP.fogColor.r;
		_packet.fogColorG = gDP.fogColor.g;
		_packet.fogColorB = gDP.fogColor.b;
		_packet.fogColorA = gDP.fogColor.a;
	}

	const auto * textProgram = dynamic_cast<const VulkanTextDrawerShaderProgram *>(_combiner);
	if (textProgram != nullptr) {
		_packet.shaderFlags |= vulkan::draw_shader_flags::kSpecialTextDraw;
		const f32 * color = textProgram->textColor();
		_packet.textColorR = color[0];
		_packet.textColorG = color[1];
		_packet.textColorB = color[2];
		_packet.textColorA = color[3];
	}
}

} // namespace special_programs
} // namespace vulkan
