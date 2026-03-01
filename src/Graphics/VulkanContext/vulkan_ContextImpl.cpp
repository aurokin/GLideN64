#include "vulkan_ContextImpl.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <cstdio>
#include <cinttypes>
#include <vector>
#include <Log.h>
#include <Graphics/ColorBufferReader.h>
#include <Graphics/CombinerProgram.h>
#include <Graphics/Parameters.h>
#include <Graphics/ShaderProgram.h>
#include <Config.h>
#include <PaletteTexture.h>
#include <gDP.h>
#include <gSP.h>
#ifdef MUPENPLUSAPI
#include <mupenplus/RealityVK_mupenplus.h>
#endif
#include "vulkan_BasicColorShaders.h"
#include "vulkan_BasicTexturedShaders.h"

#if defined(OS_LINUX) && !defined(VK_USE_PLATFORM_XLIB_KHR)
#define VK_USE_PLATFORM_XLIB_KHR 1
#endif

#if defined(OS_WINDOWS) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif

#if __has_include(<vulkan/vulkan.h>)
#define REALITYVK_VULKAN_HEADERS_AVAILABLE 1
#include <vulkan/vulkan.h>
#else
#define REALITYVK_VULKAN_HEADERS_AVAILABLE 0
#endif

#include "vulkan_DrawRecorder.h"
#include "vulkan_DrawCommandEncoder.h"
#include "vulkan_DescriptorBinder.h"
#include "vulkan_DescriptorLayoutRegistry.h"
#include "vulkan_PipelineCache.h"
#include "vulkan_ProgramLibrary.h"
#include "vulkan_BindingState.h"
#include "vulkan_FramebufferStore.h"
#include "vulkan_ShaderKeyStorage.h"
#include "vulkan_TextureStore.h"
#include "vulkan_UploadArena.h"

namespace {

class VulkanPixelReadBuffer final : public graphics::PixelReadBuffer
{
public:
	using ReadPixelsFn = std::function<bool(
		s32,
		s32,
		u32,
		u32,
		graphics::Parameter,
		graphics::Parameter,
		std::vector<u8> &)>;

	VulkanPixelReadBuffer(size_t _sizeInBytes, const ReadPixelsFn & _readPixelsFn)
		: m_readPixelsFn(_readPixelsFn)
		, m_data(_sizeInBytes, 0U)
	{
	}

	void readPixels(s32 _x, s32 _y, u32 _width, u32 _height, graphics::Parameter _format, graphics::Parameter _type) override
	{
		std::vector<u8> readbackBytes;
		if (m_readPixelsFn == nullptr || !m_readPixelsFn(_x, _y, _width, _height, _format, _type, readbackBytes)) {
			std::fill(m_data.begin(), m_data.end(), 0U);
			return;
		}
		const size_t copyBytes = std::min(m_data.size(), readbackBytes.size());
		if (copyBytes > 0U)
			std::memcpy(m_data.data(), readbackBytes.data(), copyBytes);
		if (copyBytes < m_data.size())
			std::fill_n(m_data.data() + copyBytes, m_data.size() - copyBytes, 0U);
	}

	void * getDataRange(u32 _offset, u32 _range) override
	{
		if (_offset >= m_data.size())
			return nullptr;
		const size_t maxRange = m_data.size() - _offset;
		if (_range > maxRange)
			_range = static_cast<u32>(maxRange);
		return m_data.data() + _offset;
	}

	void closeReadBuffer() override {}
	void bind() override {}
	void unbind() override {}

private:
	ReadPixelsFn m_readPixelsFn;
	std::vector<u8> m_data;
};

class VulkanColorBufferReader final : public graphics::ColorBufferReader
{
public:
	using ReadTextureFn = std::function<bool(
		graphics::ObjectHandle,
		s32,
		s32,
		u32,
		u32,
		graphics::Parameter,
		graphics::Parameter,
		std::vector<u8> &,
		u32 &)>;

	VulkanColorBufferReader(CachedTexture * _pTexture, const ReadTextureFn & _readTextureFn)
		: ColorBufferReader(_pTexture)
		, m_readTextureFn(_readTextureFn)
	{
	}

	void cleanUp() override
	{
		m_readbackData.clear();
	}

private:
	const u8 * _readPixels(const ReadColorBufferParams & _params, u32 & _heightOffset, u32 & _stride) override
	{
		_heightOffset = 0;
		_stride = _params.width;
		m_readbackData.clear();
		if (m_readTextureFn == nullptr) {
			return nullptr;
		}

		u32 stridePixels = 0U;
		const bool readOk = m_readTextureFn(
			m_pTexture->name,
			_params.x0,
			_params.y0,
			_params.width,
			_params.height,
			_params.colorFormat,
			_params.colorType,
			m_readbackData,
			stridePixels);
		if (!readOk || m_readbackData.empty()) {
			return nullptr;
		}

		if (stridePixels != 0U)
			_stride = stridePixels;
		return m_readbackData.data();
	}

	ReadTextureFn m_readTextureFn;
	std::vector<u8> m_readbackData;
};

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
	static u32 _expandColorA(u32 _encoded)
	{
		static const std::array<u32, 16> kExpanded = {
			G_GCI_COMBINED, G_GCI_TEXEL0, G_GCI_TEXEL1, G_GCI_PRIMITIVE,
			G_GCI_SHADE, G_GCI_ENVIRONMENT, G_GCI_ONE, G_GCI_NOISE,
			G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO,
			G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO
		};
		return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
	}

	static u32 _expandColorB(u32 _encoded)
	{
		static const std::array<u32, 16> kExpanded = {
			G_GCI_COMBINED, G_GCI_TEXEL0, G_GCI_TEXEL1, G_GCI_PRIMITIVE,
			G_GCI_SHADE, G_GCI_ENVIRONMENT, G_GCI_CENTER, G_GCI_K4,
			G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO,
			G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO
		};
		return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
	}

	static u32 _expandColorM(u32 _encoded)
	{
		static const std::array<u32, 32> kExpanded = {
			G_GCI_COMBINED, G_GCI_TEXEL0, G_GCI_TEXEL1, G_GCI_PRIMITIVE,
			G_GCI_SHADE, G_GCI_ENVIRONMENT, G_GCI_SCALE, G_GCI_COMBINED_ALPHA,
			G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA, G_GCI_SHADE_ALPHA,
			G_GCI_ENV_ALPHA, G_GCI_LOD_FRACTION, G_GCI_PRIM_LOD_FRAC, G_GCI_K5,
			G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO,
			G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO,
			G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO,
			G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO
		};
		return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
	}

	static u32 _expandColorAOut(u32 _encoded)
	{
		static const std::array<u32, 8> kExpanded = {
			G_GCI_COMBINED, G_GCI_TEXEL0, G_GCI_TEXEL1, G_GCI_PRIMITIVE,
			G_GCI_SHADE, G_GCI_ENVIRONMENT, G_GCI_ONE, G_GCI_ZERO
		};
		return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
	}

	static u32 _expandAlphaA(u32 _encoded)
	{
		static const std::array<u32, 8> kExpanded = {
			G_GCI_COMBINED, G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA,
			G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA, G_GCI_ONE, G_GCI_ZERO
		};
		return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
	}

	static u32 _expandAlphaB(u32 _encoded)
	{
		static const std::array<u32, 8> kExpanded = {
			G_GCI_COMBINED, G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA,
			G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA, G_GCI_ONE, G_GCI_ZERO
		};
		return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
	}

	static u32 _expandAlphaM(u32 _encoded)
	{
		static const std::array<u32, 8> kExpanded = {
			G_GCI_LOD_FRACTION, G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA,
			G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA, G_GCI_PRIM_LOD_FRAC, G_GCI_ZERO
		};
		return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
	}

	static u32 _expandAlphaAOut(u32 _encoded)
	{
		static const std::array<u32, 8> kExpanded = {
			G_GCI_COMBINED, G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA,
			G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA, G_GCI_ONE, G_GCI_ZERO
		};
		return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
	}

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
		_markExpandedInput(_expandColorA(decoded.saRGB0));
		_markExpandedInput(_expandColorB(decoded.sbRGB0));
		_markExpandedInput(_expandColorM(decoded.mRGB0));
		_markExpandedInput(_expandColorAOut(decoded.aRGB0));
		_markExpandedInput(_expandAlphaA(decoded.saA0));
		_markExpandedInput(_expandAlphaB(decoded.sbA0));
		_markExpandedInput(_expandAlphaM(decoded.mA0));
		_markExpandedInput(_expandAlphaAOut(decoded.aA0));
		if (m_key.getCycleType() == 1U) {
			_markExpandedInput(_expandColorA(decoded.saRGB1));
			_markExpandedInput(_expandColorB(decoded.sbRGB1));
			_markExpandedInput(_expandColorM(decoded.mRGB1));
			_markExpandedInput(_expandColorAOut(decoded.aRGB1));
			_markExpandedInput(_expandAlphaA(decoded.saA1));
			_markExpandedInput(_expandAlphaB(decoded.sbA1));
			_markExpandedInput(_expandAlphaM(decoded.mA1));
			_markExpandedInput(_expandAlphaAOut(decoded.aA1));
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

bool isVulkanTexrectDrawerClearProgram(const graphics::CombinerProgram * _combiner)
{
	return dynamic_cast<const VulkanTexrectDrawerClearShaderProgram *>(_combiner) != nullptr;
}

#if REALITYVK_VULKAN_HEADERS_AVAILABLE
constexpr VkDeviceSize kInitialFrameVertexBufferSize = 64U * 1024U;

u32 findMemoryTypeIndex(VkPhysicalDevice _physicalDevice, u32 _typeFilter, VkMemoryPropertyFlags _requiredProperties)
{
	VkPhysicalDeviceMemoryProperties memoryProperties{};
	vkGetPhysicalDeviceMemoryProperties(_physicalDevice, &memoryProperties);
	for (u32 i = 0; i < memoryProperties.memoryTypeCount; ++i) {
		if ((_typeFilter & (1U << i)) == 0U)
			continue;
		if ((memoryProperties.memoryTypes[i].propertyFlags & _requiredProperties) == _requiredProperties)
			return i;
	}
	return UINT32_MAX;
}

bool hasStencilComponent(VkFormat _format)
{
	return _format == VK_FORMAT_D24_UNORM_S8_UINT || _format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

bool hasDepthComponent(VkFormat _format)
{
	switch (_format) {
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_X8_D24_UNORM_PACK32:
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return true;
	default:
		return false;
	}
}

VkFormat pickDepthFormat(VkPhysicalDevice _physicalDevice)
{
	const VkFormat candidates[] = {
		VK_FORMAT_D32_SFLOAT,
		VK_FORMAT_D24_UNORM_S8_UINT,
		VK_FORMAT_D16_UNORM
	};
	for (VkFormat format : candidates) {
		VkFormatProperties properties{};
		vkGetPhysicalDeviceFormatProperties(_physicalDevice, format, &properties);
		if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
			return format;
	}
	return VK_FORMAT_UNDEFINED;
}

const char * packetSourceName(u32 _source);

void attachPacketBindings(const vulkan::BindingState & _bindingState, vulkan::DrawPacket & _packet)
{
	static const bool debugBindings = std::getenv("REALITYVK_VK_DEBUG_BINDINGS") != nullptr;
	static const bool forceUntextured = std::getenv("REALITYVK_VK_DEBUG_FORCE_UNTEXTURED") != nullptr;
	static const bool forceTexture0Only = std::getenv("REALITYVK_VK_DEBUG_FORCE_TEXTURE0_ONLY") != nullptr;
	static const bool forceTexturedNoSample = std::getenv("REALITYVK_VK_DEBUG_FORCE_TEXTURED_NO_SAMPLE") != nullptr;
	static const char * forcedTextureHandleEnv = std::getenv("REALITYVK_VK_DEBUG_FORCE_TEXTURE_HANDLE");
	static const u32 forcedTextureHandle = forcedTextureHandleEnv != nullptr && forcedTextureHandleEnv[0] != '\0'
		? static_cast<u32>(std::strtoul(forcedTextureHandleEnv, nullptr, 10))
		: 0U;
	static bool loggedDebugConfig = false;
	if (!loggedDebugConfig
		&& (debugBindings || forceUntextured || forceTexture0Only || forceTexturedNoSample || forcedTextureHandle != 0U)) {
		LOG(
			LOG_WARNING,
			"VK bindings debug config: enabled=%u forceUntextured=%u forceTexture0Only=%u forceTexturedNoSample=%u forcedTextureHandle=%u",
			debugBindings ? 1U : 0U,
			forceUntextured ? 1U : 0U,
			forceTexture0Only ? 1U : 0U,
			forceTexturedNoSample ? 1U : 0U,
			forcedTextureHandle);
		loggedDebugConfig = true;
	}
	_packet.textureSlotMask = 0U;
	std::array<vulkan::BindingState::TextureBinding, vulkan::binding_limits::kTextureUnits> textureBindings{};
	u32 availableTextureUnitMask = 0U;
	_bindingState.forEachTextureBinding([&textureBindings, &availableTextureUnitMask](u32 _unit, const vulkan::BindingState::TextureBinding & _binding) {
		if (_unit >= vulkan::binding_limits::kTextureUnits)
			return;
		textureBindings[_unit] = _binding;
		availableTextureUnitMask |= (1U << _unit);
	});
	auto assignTextureInput = [&](u32 _descriptorIndex, u32 _unit) {
		if (_descriptorIndex >= vulkan::binding_limits::kTextureUnits || _unit >= vulkan::binding_limits::kTextureUnits)
			return;
		if ((availableTextureUnitMask & (1U << _unit)) == 0U)
			return;
		const vulkan::BindingState::TextureBinding & binding = textureBindings[_unit];
		if (!binding.texture.isNotNull())
			return;
		vulkan::TextureSlotReference & textureRef = _packet.textureSlots[_descriptorIndex];
		textureRef.unit = _descriptorIndex;
		textureRef.texture = binding.texture;
		textureRef.target = binding.target;
		_packet.textureSlotMask |= (1U << _descriptorIndex);
	};
	if ((_packet.shaderFlags & vulkan::draw_shader_flags::kTexture0) != 0U)
		assignTextureInput(0, _packet.textureUnit0);
	if ((_packet.shaderFlags & vulkan::draw_shader_flags::kTexture1) != 0U)
		assignTextureInput(1, _packet.textureUnit1);
	if ((_packet.shaderFlags & vulkan::draw_shader_flags::kSpecialDepthFog) != 0U) {
		assignTextureInput(3U, static_cast<u32>(graphics::textureIndices::ZLUTTex));
		assignTextureInput(4U, static_cast<u32>(graphics::textureIndices::PaletteTex));
	}
	if ((_packet.shaderFlags & vulkan::draw_shader_flags::kTexture0) != 0U && (_packet.textureSlotMask & (1U << 0)) == 0U)
		_packet.shaderFlags &= ~vulkan::draw_shader_flags::kTexture0;
	if ((_packet.shaderFlags & vulkan::draw_shader_flags::kTexture1) != 0U && (_packet.textureSlotMask & (1U << 1)) == 0U)
		_packet.shaderFlags &= ~vulkan::draw_shader_flags::kTexture1;
	if ((_packet.shaderFlags & vulkan::draw_shader_flags::kSpecialDepthFog) != 0U
		&& ((_packet.textureSlotMask & (1U << 3U)) == 0U || (_packet.textureSlotMask & (1U << 4U)) == 0U)) {
		_packet.shaderFlags &= ~vulkan::draw_shader_flags::kSpecialDepthFog;
	}
	if (forceUntextured) {
		_packet.textureSlotMask = 0U;
		_packet.shaderFlags &= ~(vulkan::draw_shader_flags::kTexture0 | vulkan::draw_shader_flags::kTexture1);
	}
	if (forceTexture0Only) {
		_packet.textureSlotMask &= ~(1U << 1);
		_packet.shaderFlags &= ~vulkan::draw_shader_flags::kTexture1;
	}
	if (forceTexturedNoSample && _packet.textureSlotMask != 0U) {
		_packet.shaderFlags &= ~(vulkan::draw_shader_flags::kTexture0 | vulkan::draw_shader_flags::kTexture1);
		_packet.shaderFlags |= vulkan::draw_shader_flags::kShade;
	}
	if (forcedTextureHandle != 0U) {
		_packet.textureSlots[0].unit = 0U;
		_packet.textureSlots[0].texture = graphics::ObjectHandle(forcedTextureHandle);
		_packet.textureSlotMask |= (1U << 0);
		_packet.textureSlotMask &= ~(1U << 1);
		_packet.shaderFlags &= ~(vulkan::draw_shader_flags::kTexture1);
		_packet.shaderFlags |= vulkan::draw_shader_flags::kTexture0;
		for (vulkan::DrawVertex & vertex : _packet.vertices) {
			vertex.s0 = 0.5f;
			vertex.t0 = 0.5f;
		}
	}
		if (debugBindings) {
			static const u32 packetBindingLogLimit = []() -> u32 {
				const char * env = std::getenv("REALITYVK_VK_DEBUG_BINDINGS_PACKET_LIMIT");
				if (env == nullptr || env[0] == '\0')
					return 96U;
				return static_cast<u32>(std::strtoul(env, nullptr, 10));
			}();
			static u32 packetBindingLogCount = 0U;
			if (packetBindingLogCount < packetBindingLogLimit && _packet.textureSlotMask != 0U) {
			const u32 handle0 = (_packet.textureSlotMask & (1U << 0)) != 0U
				? static_cast<u32>(_packet.textureSlots[0].texture)
				: 0U;
			const u32 handle1 = (_packet.textureSlotMask & (1U << 1)) != 0U
				? static_cast<u32>(_packet.textureSlots[1].texture)
				: 0U;
			const u32 bound0 = textureBindings[0].texture.isNotNull() ? static_cast<u32>(textureBindings[0].texture) : 0U;
			const u32 bound1 = textureBindings[1].texture.isNotNull() ? static_cast<u32>(textureBindings[1].texture) : 0U;
			const u32 bound2 = textureBindings[2].texture.isNotNull() ? static_cast<u32>(textureBindings[2].texture) : 0U;
			const u32 bound3 = textureBindings[3].texture.isNotNull() ? static_cast<u32>(textureBindings[3].texture) : 0U;
			f32 minS0 = 0.0f;
			f32 maxS0 = 0.0f;
			f32 minT0 = 0.0f;
			f32 maxT0 = 0.0f;
			f32 minS1 = 0.0f;
			f32 maxS1 = 0.0f;
			f32 minT1 = 0.0f;
			f32 maxT1 = 0.0f;
			if (!_packet.vertices.empty()) {
				minS0 = maxS0 = _packet.vertices[0].s0;
				minT0 = maxT0 = _packet.vertices[0].t0;
				minS1 = maxS1 = _packet.vertices[0].s1;
				minT1 = maxT1 = _packet.vertices[0].t1;
				for (const vulkan::DrawVertex & vertex : _packet.vertices) {
					minS0 = std::min(minS0, vertex.s0);
					maxS0 = std::max(maxS0, vertex.s0);
					minT0 = std::min(minT0, vertex.t0);
					maxT0 = std::max(maxT0, vertex.t0);
					minS1 = std::min(minS1, vertex.s1);
					maxS1 = std::max(maxS1, vertex.s1);
					minT1 = std::min(minT1, vertex.t1);
					maxT1 = std::max(maxT1, vertex.t1);
				}
			}
				LOG(
					LOG_WARNING,
					"VK bindings debug: packet id=%llu src=%s texrect=%u mux=0x%016llx cycle=%u shaderFlags=0x%08x textureMask=0x%08x unit0=%u handle0=%u unit1=%u handle1=%u bound=[u0:%u u1:%u u2:%u u3:%u] tex0[s=%0.3f..%0.3f t=%0.3f..%0.3f] tex1[s=%0.3f..%0.3f t=%0.3f..%0.3f]",
					static_cast<unsigned long long>(_packet.debugPacketId),
					packetSourceName(_packet.debugSource),
					_packet.debugTexrect ? 1U : 0U,
					static_cast<unsigned long long>(_packet.debugCombinerMux),
					_packet.debugCombinerCycleType,
					_packet.shaderFlags,
					_packet.textureSlotMask,
					_packet.textureSlots[0].unit,
				handle0,
				_packet.textureSlots[1].unit,
				handle1,
				bound0,
				bound1,
				bound2,
				bound3,
				minS0,
				maxS0,
				minT0,
				maxT0,
				minS1,
				maxS1,
				minT1,
				maxT1);
			++packetBindingLogCount;
		}
		const bool requestedTexture0 = (_packet.shaderFlags & vulkan::draw_shader_flags::kTexture0) != 0U;
		const bool requestedTexture1 = (_packet.shaderFlags & vulkan::draw_shader_flags::kTexture1) != 0U;
		if ((requestedTexture0 || requestedTexture1) && _packet.textureSlotMask == 0U) {
			LOG(
				LOG_WARNING,
				"VK bindings debug: requested textures (t0=%u,t1=%u) but no bound texture slots available (unitMask=0x%08x).",
				requestedTexture0 ? 1U : 0U,
				requestedTexture1 ? 1U : 0U,
				availableTextureUnitMask);
		}
	}

	_packet.imageSlotMask = 0U;
	_bindingState.forEachImageBinding([&_packet](u32 _unit, const vulkan::BindingState::ImageBinding & _binding) {
		if (_unit >= vulkan::binding_limits::kImageUnits)
			return;
		vulkan::ImageSlotReference & imageRef = _packet.imageSlots[_unit];
		imageRef.unit = _unit;
		imageRef.texture = _binding.texture;
		imageRef.accessMode = _binding.accessMode;
		imageRef.textureFormat = _binding.textureFormat;
		_packet.imageSlotMask |= (1U << _unit);
	});
}

void normalizePacketTextureCoordinates(const vulkan::TextureStore & _textureStore, vulkan::DrawPacket & _packet)
{
	static const bool debugBindings = std::getenv("REALITYVK_VK_DEBUG_BINDINGS") != nullptr;
	static const bool disableNormalize = std::getenv("REALITYVK_VK_DISABLE_TEXCOORD_NORMALIZE") != nullptr;
	static const bool enableFixedPointTexcoordScale = std::getenv("REALITYVK_VK_ENABLE_TEXCOORD_FIXEDPOINT_SCALE") != nullptr;
	if (disableNormalize)
		return;

	auto normalizeSlot = [&](u32 _slot, bool _useFirstTexCoordSet) {
		if ((_packet.textureSlotMask & (1U << _slot)) == 0U)
			return;
		const vulkan::TextureSlotReference & slotRef = _packet.textureSlots[_slot];
		const vulkan::TextureStore::TextureResource * texture = _textureStore.getTexture(slotRef.texture);
		if (texture == nullptr || texture->width == 0U || texture->height == 0U)
			return;
		// Convert texel-space coordinates against the backing image dimensions.
		// Using tracked content bounds here can over-normalize and effectively zoom samples.
		const u32 texWidth = texture->width;
		const u32 texHeight = texture->height;
		if (texWidth == 0U || texHeight == 0U)
			return;

		f32 maxAbsS = 0.0f;
		f32 maxAbsT = 0.0f;
		for (const vulkan::DrawVertex & vertex : _packet.vertices) {
			const f32 s = _useFirstTexCoordSet ? vertex.s0 : vertex.s1;
			const f32 t = _useFirstTexCoordSet ? vertex.t0 : vertex.t1;
			maxAbsS = std::max(maxAbsS, std::abs(s));
			maxAbsT = std::max(maxAbsT, std::abs(t));
		}

		// N64/RealityVK feeds many texture coordinates in texel space.
		// Convert likely texel-space values to normalized UVs for sampler2D shaders.
		if (maxAbsS <= 2.0f && maxAbsT <= 2.0f)
			return;
			f32 texcoordScale = 1.0f;
			// Some N64 texrect paths still deliver s10.5 fixed-point texture coordinates.
			// Apply an extra 1/32 scale when the coordinates are implausibly large for the
			// currently bound texture dimensions.
			if (enableFixedPointTexcoordScale
				&& (maxAbsS > static_cast<f32>(texWidth * 2U)
					|| maxAbsT > static_cast<f32>(texHeight * 2U))) {
				texcoordScale = 1.0f / 32.0f;
			}
			const f32 invW = texcoordScale / static_cast<f32>(texWidth);
			const f32 invH = texcoordScale / static_cast<f32>(texHeight);
		for (vulkan::DrawVertex & vertex : _packet.vertices) {
			if (_useFirstTexCoordSet) {
				vertex.s0 *= invW;
				vertex.t0 *= invH;
			} else {
				vertex.s1 *= invW;
				vertex.t1 *= invH;
			}
		}
			if (debugBindings) {
				static const u32 normalizeLogLimit = []() -> u32 {
					const char * env = std::getenv("REALITYVK_VK_DEBUG_BINDINGS_NORMALIZE_LIMIT");
					if (env == nullptr || env[0] == '\0')
						return 96U;
					return static_cast<u32>(std::strtoul(env, nullptr, 10));
				}();
				static u32 normalizeLogCount = 0U;
				if (normalizeLogCount < normalizeLogLimit) {
				f32 minS = _useFirstTexCoordSet ? _packet.vertices[0].s0 : _packet.vertices[0].s1;
				f32 maxS = minS;
				f32 minT = _useFirstTexCoordSet ? _packet.vertices[0].t0 : _packet.vertices[0].t1;
				f32 maxT = minT;
				for (const vulkan::DrawVertex & vertex : _packet.vertices) {
					const f32 s = _useFirstTexCoordSet ? vertex.s0 : vertex.s1;
					const f32 t = _useFirstTexCoordSet ? vertex.t0 : vertex.t1;
					minS = std::min(minS, s);
					maxS = std::max(maxS, s);
					minT = std::min(minT, t);
					maxT = std::max(maxT, t);
				}
					LOG(
						LOG_WARNING,
						"VK bindings debug: normalized texcoords slot=%u handle=%u preMaxAbs=[%0.3f,%0.3f] post=[s=%0.3f..%0.3f t=%0.3f..%0.3f] texSize=%ux%u contentSize=%ux%u",
						_slot,
						static_cast<u32>(slotRef.texture),
						maxAbsS,
						maxAbsT,
						minS,
						maxS,
						minT,
						maxT,
						texture->width,
						texture->height,
						texture->contentWidth,
						texture->contentHeight);
					++normalizeLogCount;
				}
			}
		};

	normalizeSlot(0U, true);
	normalizeSlot(1U, false);
}

u32 resolveShaderFlags(const graphics::CombinerProgram * _combiner, bool _defaultShade, bool _defaultTexture0)
{
	static const bool forceShade = std::getenv("REALITYVK_VK_DEBUG_FORCE_SHADE") != nullptr;
	static const bool debugCombiners = std::getenv("REALITYVK_VK_DEBUG_COMBINERS") != nullptr;
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

enum class CombinerDirectOutputMode : u8 {
	kNone = 0U,
	kZero,
	kOne,
	kShade,
	kTexel0,
	kTexel1,
	kPrimitive,
	kEnvironment
};

u32 expandCombinerColorAForSolidCheck(u32 _encoded)
{
	static const std::array<u32, 16> kExpanded = {
		G_GCI_COMBINED, G_GCI_TEXEL0, G_GCI_TEXEL1, G_GCI_PRIMITIVE,
		G_GCI_SHADE, G_GCI_ENVIRONMENT, G_GCI_ONE, G_GCI_NOISE,
		G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO,
		G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO
	};
	return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
}

u32 expandCombinerColorBForSolidCheck(u32 _encoded)
{
	static const std::array<u32, 16> kExpanded = {
		G_GCI_COMBINED, G_GCI_TEXEL0, G_GCI_TEXEL1, G_GCI_PRIMITIVE,
		G_GCI_SHADE, G_GCI_ENVIRONMENT, G_GCI_CENTER, G_GCI_K4,
		G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO,
		G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO
	};
	return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
}

u32 expandCombinerColorMForSolidCheck(u32 _encoded)
{
	static const std::array<u32, 32> kExpanded = {
		G_GCI_COMBINED, G_GCI_TEXEL0, G_GCI_TEXEL1, G_GCI_PRIMITIVE,
		G_GCI_SHADE, G_GCI_ENVIRONMENT, G_GCI_SCALE, G_GCI_COMBINED_ALPHA,
		G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA, G_GCI_SHADE_ALPHA,
		G_GCI_ENV_ALPHA, G_GCI_LOD_FRACTION, G_GCI_PRIM_LOD_FRAC, G_GCI_K5,
		G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO,
		G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO,
		G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO,
		G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO, G_GCI_ZERO
	};
	return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
}

u32 expandCombinerColorDForSolidCheck(u32 _encoded)
{
	static const std::array<u32, 8> kExpanded = {
		G_GCI_COMBINED, G_GCI_TEXEL0, G_GCI_TEXEL1, G_GCI_PRIMITIVE,
		G_GCI_SHADE, G_GCI_ENVIRONMENT, G_GCI_ONE, G_GCI_ZERO
	};
	return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
}

u32 expandCombinerAlphaMForSolidCheck(u32 _encoded)
{
	static const std::array<u32, 8> kExpanded = {
		G_GCI_LOD_FRACTION, G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA,
		G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA, G_GCI_PRIM_LOD_FRAC, G_GCI_ZERO
	};
	return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
}

u32 expandCombinerAlphaAForSolidCheck(u32 _encoded)
{
	static const std::array<u32, 8> kExpanded = {
		G_GCI_COMBINED, G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA,
		G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA, G_GCI_ONE, G_GCI_ZERO
	};
	return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
}

u32 expandCombinerAlphaBForSolidCheck(u32 _encoded)
{
	static const std::array<u32, 8> kExpanded = {
		G_GCI_COMBINED, G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA,
		G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA, G_GCI_ONE, G_GCI_ZERO
	};
	return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
}

u32 expandCombinerAlphaDForSolidCheck(u32 _encoded)
{
	static const std::array<u32, 8> kExpanded = {
		G_GCI_COMBINED, G_GCI_TEXEL0_ALPHA, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA,
		G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA, G_GCI_ONE, G_GCI_ZERO
	};
	return _encoded < kExpanded.size() ? kExpanded[_encoded] : G_GCI_ZERO;
}

CombinerDirectOutputMode detectCombinerDirectOutputMode(const CombinerKey & _key)
{
	if (_key == CombinerKey::getEmpty())
		return CombinerDirectOutputMode::kNone;

	const u32 cycleType = _key.getCycleType();
	if (cycleType == G_CYC_COPY || cycleType == G_CYC_FILL)
		return CombinerDirectOutputMode::kNone;

	gDPCombine decoded{};
	decoded.mux = _key.getMux();
	const bool secondCycle = cycleType == G_CYC_2CYCLE;
	const u32 colorM = secondCycle
		? expandCombinerColorMForSolidCheck(decoded.mRGB1)
		: expandCombinerColorMForSolidCheck(decoded.mRGB0);
	const u32 colorD = secondCycle
		? expandCombinerColorDForSolidCheck(decoded.aRGB1)
		: expandCombinerColorDForSolidCheck(decoded.aRGB0);
	const u32 alphaM = secondCycle
		? expandCombinerAlphaMForSolidCheck(decoded.mA1)
		: expandCombinerAlphaMForSolidCheck(decoded.mA0);
	const u32 alphaD = secondCycle
		? expandCombinerAlphaDForSolidCheck(decoded.aA1)
		: expandCombinerAlphaDForSolidCheck(decoded.aA0);
	if (colorM != G_GCI_ZERO || alphaM != G_GCI_ZERO)
		return CombinerDirectOutputMode::kNone;

	if (colorD == G_GCI_ZERO && alphaD == G_GCI_ZERO)
		return CombinerDirectOutputMode::kZero;
	if (colorD == G_GCI_ONE && alphaD == G_GCI_ONE)
		return CombinerDirectOutputMode::kOne;
	if (colorD == G_GCI_SHADE && alphaD == G_GCI_SHADE_ALPHA)
		return CombinerDirectOutputMode::kShade;
	if (colorD == G_GCI_TEXEL0 && alphaD == G_GCI_TEXEL0_ALPHA)
		return CombinerDirectOutputMode::kTexel0;
	if (colorD == G_GCI_TEXEL1 && alphaD == G_GCI_TEXEL1_ALPHA)
		return CombinerDirectOutputMode::kTexel1;

	if (colorD == G_GCI_PRIMITIVE && alphaD == G_GCI_PRIMITIVE_ALPHA)
		return CombinerDirectOutputMode::kPrimitive;
	if (colorD == G_GCI_ENVIRONMENT && alphaD == G_GCI_ENV_ALPHA)
		return CombinerDirectOutputMode::kEnvironment;
	return CombinerDirectOutputMode::kNone;
}

enum class CombinerConstantModulateMode : u8 {
	kNone = 0U,
	kPrimitiveTexel0,
	kPrimitiveTexel1,
	kEnvironmentTexel0,
	kEnvironmentTexel1,
	kPrimitiveShade,
	kEnvironmentShade
};

enum class CombinerConstantAddMode : u8 {
	kNone = 0U,
	kPrimitiveTexel0,
	kPrimitiveTexel1,
	kPrimitiveShade,
	kEnvironmentTexel0,
	kEnvironmentTexel1,
	kEnvironmentShade
};

enum class CombinerDirectAlphaMode : u8 {
	kNone = 0U,
	kZero,
	kOne,
	kShade,
	kTexel0,
	kTexel1,
	kPrimitive,
	kEnvironment
};

CombinerConstantModulateMode detectCombinerConstantModulateMode(const CombinerKey & _key)
{
	if (_key == CombinerKey::getEmpty())
		return CombinerConstantModulateMode::kNone;

	const u32 cycleType = _key.getCycleType();
	if (cycleType == G_CYC_COPY || cycleType == G_CYC_FILL)
		return CombinerConstantModulateMode::kNone;

	gDPCombine decoded{};
	decoded.mux = _key.getMux();
	const bool secondCycle = cycleType == G_CYC_2CYCLE;
	const u32 colorA = secondCycle
		? expandCombinerColorAForSolidCheck(decoded.saRGB1)
		: expandCombinerColorAForSolidCheck(decoded.saRGB0);
	const u32 colorB = secondCycle
		? expandCombinerColorBForSolidCheck(decoded.sbRGB1)
		: expandCombinerColorBForSolidCheck(decoded.sbRGB0);
	const u32 colorM = secondCycle
		? expandCombinerColorMForSolidCheck(decoded.mRGB1)
		: expandCombinerColorMForSolidCheck(decoded.mRGB0);
	const u32 colorD = secondCycle
		? expandCombinerColorDForSolidCheck(decoded.aRGB1)
		: expandCombinerColorDForSolidCheck(decoded.aRGB0);
	const u32 alphaA = secondCycle
		? expandCombinerAlphaAForSolidCheck(decoded.saA1)
		: expandCombinerAlphaAForSolidCheck(decoded.saA0);
	const u32 alphaB = secondCycle
		? expandCombinerAlphaBForSolidCheck(decoded.sbA1)
		: expandCombinerAlphaBForSolidCheck(decoded.sbA0);
	const u32 alphaM = secondCycle
		? expandCombinerAlphaMForSolidCheck(decoded.mA1)
		: expandCombinerAlphaMForSolidCheck(decoded.mA0);
	const u32 alphaD = secondCycle
		? expandCombinerAlphaDForSolidCheck(decoded.aA1)
		: expandCombinerAlphaDForSolidCheck(decoded.aA0);

	if (colorB != G_GCI_ZERO
		|| colorD != G_GCI_ZERO
		|| alphaB != G_GCI_ZERO
		|| alphaD != G_GCI_ZERO) {
		return CombinerConstantModulateMode::kNone;
	}

	auto isModulatePattern = [&](u32 _texColor, u32 _constColor, u32 _texAlpha, u32 _constAlpha) -> bool {
		const bool colorMatch =
			((colorA == _texColor && colorM == _constColor) || (colorA == _constColor && colorM == _texColor));
		const bool alphaMatch =
			((alphaA == _texAlpha && alphaM == _constAlpha) || (alphaA == _constAlpha && alphaM == _texAlpha));
		return colorMatch && alphaMatch;
	};

	if (isModulatePattern(G_GCI_TEXEL0, G_GCI_PRIMITIVE, G_GCI_TEXEL0_ALPHA, G_GCI_PRIMITIVE_ALPHA))
		return CombinerConstantModulateMode::kPrimitiveTexel0;
	if (isModulatePattern(G_GCI_TEXEL1, G_GCI_PRIMITIVE, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA))
		return CombinerConstantModulateMode::kPrimitiveTexel1;
	if (isModulatePattern(G_GCI_TEXEL0, G_GCI_ENVIRONMENT, G_GCI_TEXEL0_ALPHA, G_GCI_ENV_ALPHA))
		return CombinerConstantModulateMode::kEnvironmentTexel0;
	if (isModulatePattern(G_GCI_TEXEL1, G_GCI_ENVIRONMENT, G_GCI_TEXEL1_ALPHA, G_GCI_ENV_ALPHA))
		return CombinerConstantModulateMode::kEnvironmentTexel1;
	if (isModulatePattern(G_GCI_SHADE, G_GCI_PRIMITIVE, G_GCI_SHADE_ALPHA, G_GCI_PRIMITIVE_ALPHA))
		return CombinerConstantModulateMode::kPrimitiveShade;
	if (isModulatePattern(G_GCI_SHADE, G_GCI_ENVIRONMENT, G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA))
		return CombinerConstantModulateMode::kEnvironmentShade;
	return CombinerConstantModulateMode::kNone;
}

CombinerConstantAddMode detectCombinerConstantAddMode(const CombinerKey & _key)
{
	if (_key == CombinerKey::getEmpty())
		return CombinerConstantAddMode::kNone;

	const u32 cycleType = _key.getCycleType();
	if (cycleType == G_CYC_COPY || cycleType == G_CYC_FILL)
		return CombinerConstantAddMode::kNone;

	gDPCombine decoded{};
	decoded.mux = _key.getMux();
	const bool secondCycle = cycleType == G_CYC_2CYCLE;
	const u32 colorA = secondCycle
		? expandCombinerColorAForSolidCheck(decoded.saRGB1)
		: expandCombinerColorAForSolidCheck(decoded.saRGB0);
	const u32 colorB = secondCycle
		? expandCombinerColorBForSolidCheck(decoded.sbRGB1)
		: expandCombinerColorBForSolidCheck(decoded.sbRGB0);
	const u32 colorM = secondCycle
		? expandCombinerColorMForSolidCheck(decoded.mRGB1)
		: expandCombinerColorMForSolidCheck(decoded.mRGB0);
	const u32 colorD = secondCycle
		? expandCombinerColorDForSolidCheck(decoded.aRGB1)
		: expandCombinerColorDForSolidCheck(decoded.aRGB0);
	const u32 alphaA = secondCycle
		? expandCombinerAlphaAForSolidCheck(decoded.saA1)
		: expandCombinerAlphaAForSolidCheck(decoded.saA0);
	const u32 alphaB = secondCycle
		? expandCombinerAlphaBForSolidCheck(decoded.sbA1)
		: expandCombinerAlphaBForSolidCheck(decoded.sbA0);
	const u32 alphaM = secondCycle
		? expandCombinerAlphaMForSolidCheck(decoded.mA1)
		: expandCombinerAlphaMForSolidCheck(decoded.mA0);
	const u32 alphaD = secondCycle
		? expandCombinerAlphaDForSolidCheck(decoded.aA1)
		: expandCombinerAlphaDForSolidCheck(decoded.aA0);

	if (colorB != G_GCI_ZERO
		|| alphaB != G_GCI_ZERO
		|| colorM != G_GCI_ONE
		|| alphaM != G_GCI_ONE) {
		return CombinerConstantAddMode::kNone;
	}

	auto matchesAddPattern = [&](u32 _sourceColor, u32 _constColor, u32 _sourceAlpha, u32 _constAlpha) -> bool {
		return colorA == _sourceColor
			&& colorD == _constColor
			&& alphaA == _sourceAlpha
			&& alphaD == _constAlpha;
	};

	if (matchesAddPattern(G_GCI_TEXEL0, G_GCI_PRIMITIVE, G_GCI_TEXEL0_ALPHA, G_GCI_PRIMITIVE_ALPHA))
		return CombinerConstantAddMode::kPrimitiveTexel0;
	if (matchesAddPattern(G_GCI_TEXEL1, G_GCI_PRIMITIVE, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA))
		return CombinerConstantAddMode::kPrimitiveTexel1;
	if (matchesAddPattern(G_GCI_SHADE, G_GCI_PRIMITIVE, G_GCI_SHADE_ALPHA, G_GCI_PRIMITIVE_ALPHA))
		return CombinerConstantAddMode::kPrimitiveShade;
	if (matchesAddPattern(G_GCI_TEXEL0, G_GCI_ENVIRONMENT, G_GCI_TEXEL0_ALPHA, G_GCI_ENV_ALPHA))
		return CombinerConstantAddMode::kEnvironmentTexel0;
	if (matchesAddPattern(G_GCI_TEXEL1, G_GCI_ENVIRONMENT, G_GCI_TEXEL1_ALPHA, G_GCI_ENV_ALPHA))
		return CombinerConstantAddMode::kEnvironmentTexel1;
	if (matchesAddPattern(G_GCI_SHADE, G_GCI_ENVIRONMENT, G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA))
		return CombinerConstantAddMode::kEnvironmentShade;
	return CombinerConstantAddMode::kNone;
}

CombinerDirectAlphaMode detectCombinerDirectAlphaMode(const CombinerKey & _key)
{
	if (_key == CombinerKey::getEmpty())
		return CombinerDirectAlphaMode::kNone;

	const u32 cycleType = _key.getCycleType();
	if (cycleType == G_CYC_COPY || cycleType == G_CYC_FILL)
		return CombinerDirectAlphaMode::kNone;

	gDPCombine decoded{};
	decoded.mux = _key.getMux();
	const bool secondCycle = cycleType == G_CYC_2CYCLE;
	const u32 alphaM = secondCycle
		? expandCombinerAlphaMForSolidCheck(decoded.mA1)
		: expandCombinerAlphaMForSolidCheck(decoded.mA0);
	const u32 alphaD = secondCycle
		? expandCombinerAlphaDForSolidCheck(decoded.aA1)
		: expandCombinerAlphaDForSolidCheck(decoded.aA0);
	if (alphaM != G_GCI_ZERO)
		return CombinerDirectAlphaMode::kNone;

	switch (alphaD) {
	case G_GCI_ZERO:
		return CombinerDirectAlphaMode::kZero;
	case G_GCI_ONE:
		return CombinerDirectAlphaMode::kOne;
	case G_GCI_SHADE_ALPHA:
		return CombinerDirectAlphaMode::kShade;
	case G_GCI_TEXEL0_ALPHA:
		return CombinerDirectAlphaMode::kTexel0;
	case G_GCI_TEXEL1_ALPHA:
		return CombinerDirectAlphaMode::kTexel1;
	case G_GCI_PRIMITIVE_ALPHA:
		return CombinerDirectAlphaMode::kPrimitive;
	case G_GCI_ENV_ALPHA:
		return CombinerDirectAlphaMode::kEnvironment;
	default:
		return CombinerDirectAlphaMode::kNone;
	}
}

struct CombinerTexturedAlphaScaleInfo {
	bool valid = false;
	bool primaryIsTexel0 = true;
	CombinerDirectAlphaMode alphaScaleMode = CombinerDirectAlphaMode::kNone;
};

CombinerTexturedAlphaScaleInfo detectCombinerTexturedAlphaScaleInfo(const CombinerKey & _key)
{
	CombinerTexturedAlphaScaleInfo info{};
	if (_key == CombinerKey::getEmpty())
		return info;

	const u32 cycleType = _key.getCycleType();
	if (cycleType == G_CYC_COPY || cycleType == G_CYC_FILL)
		return info;

	gDPCombine decoded{};
	decoded.mux = _key.getMux();
	const bool secondCycle = cycleType == G_CYC_2CYCLE;
	const u32 colorB = secondCycle
		? expandCombinerColorBForSolidCheck(decoded.sbRGB1)
		: expandCombinerColorBForSolidCheck(decoded.sbRGB0);
	const u32 colorM = secondCycle
		? expandCombinerColorMForSolidCheck(decoded.mRGB1)
		: expandCombinerColorMForSolidCheck(decoded.mRGB0);
	const u32 colorD = secondCycle
		? expandCombinerColorDForSolidCheck(decoded.aRGB1)
		: expandCombinerColorDForSolidCheck(decoded.aRGB0);
	const u32 alphaA = secondCycle
		? expandCombinerAlphaAForSolidCheck(decoded.saA1)
		: expandCombinerAlphaAForSolidCheck(decoded.saA0);
	const u32 alphaB = secondCycle
		? expandCombinerAlphaBForSolidCheck(decoded.sbA1)
		: expandCombinerAlphaBForSolidCheck(decoded.sbA0);
	const u32 alphaM = secondCycle
		? expandCombinerAlphaMForSolidCheck(decoded.mA1)
		: expandCombinerAlphaMForSolidCheck(decoded.mA0);
	const u32 alphaD = secondCycle
		? expandCombinerAlphaDForSolidCheck(decoded.aA1)
		: expandCombinerAlphaDForSolidCheck(decoded.aA0);

	if (colorB != G_GCI_ZERO || colorM != G_GCI_ZERO)
		return info;
	if (alphaB != G_GCI_ZERO || alphaD != G_GCI_ZERO)
		return info;

	if (colorD == G_GCI_TEXEL0) {
		if (alphaA != G_GCI_TEXEL0_ALPHA)
			return info;
		info.primaryIsTexel0 = true;
	} else if (colorD == G_GCI_TEXEL1) {
		if (alphaA != G_GCI_TEXEL1_ALPHA)
			return info;
		info.primaryIsTexel0 = false;
	} else {
		return info;
	}

	switch (alphaM) {
	case G_GCI_PRIMITIVE_ALPHA:
		info.alphaScaleMode = CombinerDirectAlphaMode::kPrimitive;
		break;
	case G_GCI_ENV_ALPHA:
		info.alphaScaleMode = CombinerDirectAlphaMode::kEnvironment;
		break;
	case G_GCI_SHADE_ALPHA:
		info.alphaScaleMode = CombinerDirectAlphaMode::kShade;
		break;
	case G_GCI_ONE:
		info.alphaScaleMode = CombinerDirectAlphaMode::kOne;
		break;
	default:
		info.alphaScaleMode = CombinerDirectAlphaMode::kNone;
		break;
	}

	info.valid = info.alphaScaleMode != CombinerDirectAlphaMode::kNone;
	return info;
}

enum class CombinerPostModulateConstantSource : u8 {
	kNone = 0U,
	kPrimitive,
	kEnvironment
};

enum class CombinerTwoCyclePostModulateBase : u8 {
	kNone = 0U,
	kTexel0ShadeAlphaTexel0Shade,
	kTexel0ShadeAlphaTexel0,
	kShadeAlphaShade
};

struct CombinerTwoCyclePostModulateInfo {
	CombinerPostModulateConstantSource source = CombinerPostModulateConstantSource::kNone;
	CombinerTwoCyclePostModulateBase base = CombinerTwoCyclePostModulateBase::kNone;

	bool valid() const
	{
		return source != CombinerPostModulateConstantSource::kNone
			&& base != CombinerTwoCyclePostModulateBase::kNone;
	}
};

CombinerTwoCyclePostModulateInfo detectCombinerTwoCyclePostModulateInfo(const CombinerKey & _key)
{
	CombinerTwoCyclePostModulateInfo info{};
	if (_key == CombinerKey::getEmpty())
		return info;
	if (_key.getCycleType() != G_CYC_2CYCLE)
		return info;

	gDPCombine decoded{};
	decoded.mux = _key.getMux();

	const u32 colorA0 = expandCombinerColorAForSolidCheck(decoded.saRGB0);
	const u32 colorB0 = expandCombinerColorBForSolidCheck(decoded.sbRGB0);
	const u32 colorM0 = expandCombinerColorMForSolidCheck(decoded.mRGB0);
	const u32 colorD0 = expandCombinerColorDForSolidCheck(decoded.aRGB0);
	const u32 alphaA0 = expandCombinerAlphaAForSolidCheck(decoded.saA0);
	const u32 alphaB0 = expandCombinerAlphaBForSolidCheck(decoded.sbA0);
	const u32 alphaM0 = expandCombinerAlphaMForSolidCheck(decoded.mA0);
	const u32 alphaD0 = expandCombinerAlphaDForSolidCheck(decoded.aA0);

	const u32 colorA1 = expandCombinerColorAForSolidCheck(decoded.saRGB1);
	const u32 colorB1 = expandCombinerColorBForSolidCheck(decoded.sbRGB1);
	const u32 colorM1 = expandCombinerColorMForSolidCheck(decoded.mRGB1);
	const u32 colorD1 = expandCombinerColorDForSolidCheck(decoded.aRGB1);
	const u32 alphaA1 = expandCombinerAlphaAForSolidCheck(decoded.saA1);
	const u32 alphaB1 = expandCombinerAlphaBForSolidCheck(decoded.sbA1);
	const u32 alphaM1 = expandCombinerAlphaMForSolidCheck(decoded.mA1);
	const u32 alphaD1 = expandCombinerAlphaDForSolidCheck(decoded.aA1);

	if (colorA1 != G_GCI_COMBINED
		|| colorB1 != G_GCI_ZERO
		|| colorD1 != G_GCI_ZERO
		|| alphaA1 != G_GCI_ZERO
		|| alphaB1 != G_GCI_ZERO
		|| alphaM1 != G_GCI_ZERO
		|| alphaD1 != G_GCI_COMBINED) {
		return info;
	}

	if (colorM1 == G_GCI_PRIMITIVE) {
		info.source = CombinerPostModulateConstantSource::kPrimitive;
	} else if (colorM1 == G_GCI_ENVIRONMENT) {
		info.source = CombinerPostModulateConstantSource::kEnvironment;
	} else {
		return info;
	}

	if (colorA0 == G_GCI_TEXEL0
		&& colorB0 == G_GCI_ZERO
		&& colorM0 == G_GCI_SHADE
		&& colorD0 == G_GCI_ZERO) {
		if (alphaA0 == G_GCI_TEXEL0_ALPHA
			&& alphaB0 == G_GCI_ZERO
			&& alphaM0 == G_GCI_SHADE_ALPHA
			&& alphaD0 == G_GCI_ZERO) {
			info.base = CombinerTwoCyclePostModulateBase::kTexel0ShadeAlphaTexel0Shade;
			return info;
		}
		if (alphaA0 == G_GCI_ZERO
			&& alphaB0 == G_GCI_ZERO
			&& alphaM0 == G_GCI_ZERO
			&& alphaD0 == G_GCI_TEXEL0_ALPHA) {
			info.base = CombinerTwoCyclePostModulateBase::kTexel0ShadeAlphaTexel0;
			return info;
		}
	}

	if (colorA0 == G_GCI_ZERO
		&& colorB0 == G_GCI_ZERO
		&& colorM0 == G_GCI_ZERO
		&& colorD0 == G_GCI_SHADE
		&& alphaA0 == G_GCI_ZERO
		&& alphaB0 == G_GCI_ZERO
		&& alphaM0 == G_GCI_ZERO
		&& alphaD0 == G_GCI_SHADE_ALPHA) {
		info.base = CombinerTwoCyclePostModulateBase::kShadeAlphaShade;
		return info;
	}

	return info;
}

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
	static const bool disableCombinerSolidOverride = std::getenv("REALITYVK_VK_DISABLE_COMBINER_SOLID_OVERRIDE") != nullptr;
	if (disableCombinerSolidOverride)
		return;
	static const bool disableCombinerDirectOverride = std::getenv("REALITYVK_VK_DISABLE_COMBINER_DIRECT_OVERRIDE") != nullptr;
	static const bool disableCombinerModulateOverride = std::getenv("REALITYVK_VK_DISABLE_COMBINER_MODULATE_OVERRIDE") != nullptr;
	static const bool disableCombinerAddOverride = std::getenv("REALITYVK_VK_DISABLE_COMBINER_ADD_OVERRIDE") != nullptr;
	static const bool disableCombinerAlphaScaleOverride = std::getenv("REALITYVK_VK_DISABLE_COMBINER_ALPHA_SCALE_OVERRIDE") != nullptr;
	static const bool disableCombinerTwoCyclePostModulateOverride = std::getenv("REALITYVK_VK_DISABLE_COMBINER_TWO_CYCLE_POST_MODULATE_OVERRIDE") != nullptr;

	if (_combiner == nullptr || _packet.vertices.empty())
		return;
	const CombinerKey & key = _combiner->getKey();

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
	static const bool debugSkipMuxAlphaZeroOnly = std::getenv("REALITYVK_VK_DEBUG_SKIP_MUX_ALPHA_ZERO_ONLY") != nullptr;
	static const bool debugSkipMuxAlphaNonZeroOnly = std::getenv("REALITYVK_VK_DEBUG_SKIP_MUX_ALPHA_NONZERO_ONLY") != nullptr;
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
				return;
			}
		}
		if ((forceShadeMux != 0ULL && mux == forceShadeMux)
			|| (forceShadeMux2 != 0ULL && mux == forceShadeMux2)) {
			_packet.shaderFlags |= vulkan::draw_shader_flags::kShade;
			_packet.shaderFlags &= ~vulkan::draw_shader_flags::kShadeRGBOnly;
			return;
		}
		if ((forceShadeRgbMux != 0ULL && mux == forceShadeRgbMux)
			|| (forceShadeRgbMux2 != 0ULL && mux == forceShadeRgbMux2)) {
			_packet.shaderFlags |= vulkan::draw_shader_flags::kShade;
			_packet.shaderFlags |= vulkan::draw_shader_flags::kShadeRGBOnly;
			return;
		}
	}
	_packet.shaderFlags &= ~vulkan::draw_shader_flags::kShadeRGBOnly;

	CombinerDirectOutputMode mode = detectCombinerDirectOutputMode(key);
	CombinerConstantModulateMode modulateMode = detectCombinerConstantModulateMode(key);
	CombinerConstantAddMode addMode = detectCombinerConstantAddMode(key);
	const CombinerDirectAlphaMode alphaMode = detectCombinerDirectAlphaMode(key);
	const CombinerTexturedAlphaScaleInfo alphaScaleInfo =
		detectCombinerTexturedAlphaScaleInfo(key);
	const CombinerTwoCyclePostModulateInfo twoCyclePostModulateInfo =
		detectCombinerTwoCyclePostModulateInfo(key);
	if (disableCombinerDirectOverride)
		mode = CombinerDirectOutputMode::kNone;
	// Modulate override currently improves the Paper Mario intro gate signal on
	// the refreshed stock-reference path; keep it on by default with opt-out.
	if (disableCombinerModulateOverride)
		modulateMode = CombinerConstantModulateMode::kNone;
	if (disableCombinerAddOverride)
		addMode = CombinerConstantAddMode::kNone;
	const bool hasAlphaScaleInfo = alphaScaleInfo.valid && !disableCombinerAlphaScaleOverride;
	const bool hasTwoCyclePostModulateInfo = twoCyclePostModulateInfo.valid() && !disableCombinerTwoCyclePostModulateOverride;
	static const bool debugCombinerOverrides = std::getenv("REALITYVK_VK_DEBUG_COMBINER_OVERRIDES") != nullptr;
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
		static const bool debugCombinerCoverage = std::getenv("REALITYVK_VK_DEBUG_COMBINER_COVERAGE") != nullptr;
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
		return;
	}

	if (mode == CombinerDirectOutputMode::kShade && modulateMode == CombinerConstantModulateMode::kNone) {
		_packet.shaderFlags &= ~(vulkan::draw_shader_flags::kTexture0 | vulkan::draw_shader_flags::kTexture1);
		_packet.shaderFlags |= vulkan::draw_shader_flags::kShade;
		return;
	}

	if (mode == CombinerDirectOutputMode::kTexel0 && modulateMode == CombinerConstantModulateMode::kNone) {
		_packet.shaderFlags &= ~(vulkan::draw_shader_flags::kTexture1
			| vulkan::draw_shader_flags::kShade
			| vulkan::draw_shader_flags::kShadeRGBOnly);
		_packet.shaderFlags |= vulkan::draw_shader_flags::kTexture0;
		applyDirectAlphaOverrideForTextured(alphaMode, true, _packet);
		return;
	}

	if (mode == CombinerDirectOutputMode::kTexel1 && modulateMode == CombinerConstantModulateMode::kNone) {
		_packet.shaderFlags &= ~(vulkan::draw_shader_flags::kTexture0
			| vulkan::draw_shader_flags::kShade
			| vulkan::draw_shader_flags::kShadeRGBOnly);
		_packet.shaderFlags |= vulkan::draw_shader_flags::kTexture1;
		applyDirectAlphaOverrideForTextured(alphaMode, false, _packet);
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
			static const bool debugModulateState = std::getenv("REALITYVK_VK_DEBUG_MODULATE_STATE") != nullptr;
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
}

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

void applyStrictBlendMuxPacketState(bool _texrect, vulkan::DrawPacket & _packet)
{
	if (!shouldEnableStrictBlendMux(_texrect))
		return;

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
		_packet.shaderFlags &= ~vulkan::draw_shader_flags::kSpecialStrictBlendMux;
		_packet.blendMux1Packed = 0U;
		_packet.blendMux2Packed = 0U;
		_packet.blendParamsPacked = 0U;
		return;
	}
	if (strictOnlyParams >= 0 && static_cast<int>(_packet.blendParamsPacked) != strictOnlyParams) {
		_packet.shaderFlags &= ~vulkan::draw_shader_flags::kSpecialStrictBlendMux;
		_packet.blendMux1Packed = 0U;
		_packet.blendMux2Packed = 0U;
		_packet.blendParamsPacked = 0U;
		return;
	}

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

bool resolvePrimitiveType(graphics::DrawModeParam _mode, vulkan::PrimitiveType & _outPrimitive)
{
	if (_mode == graphics::drawmode::TRIANGLES) {
		_outPrimitive = vulkan::PrimitiveType::Triangles;
		return true;
	}
	if (_mode == graphics::drawmode::TRIANGLE_STRIP) {
		_outPrimitive = vulkan::PrimitiveType::TriangleStrip;
		return true;
	}
	if (_mode == graphics::drawmode::TRIANGLE_FAN) {
		_outPrimitive = vulkan::PrimitiveType::TriangleFan;
		return true;
	}
	return false;
}

const char * bufferTargetName(graphics::BufferTargetParam _target)
{
	if (_target == graphics::bufferTarget::FRAMEBUFFER)
		return "FRAMEBUFFER";
	if (_target == graphics::bufferTarget::DRAW_FRAMEBUFFER)
		return "DRAW_FRAMEBUFFER";
	if (_target == graphics::bufferTarget::READ_FRAMEBUFFER)
		return "READ_FRAMEBUFFER";
	return "UNKNOWN";
}

const char * bufferAttachmentName(graphics::BufferAttachmentParam _attachment)
{
	if (_attachment == graphics::bufferAttachment::COLOR_ATTACHMENT0)
		return "COLOR_ATTACHMENT0";
	if (_attachment == graphics::bufferAttachment::COLOR_ATTACHMENT1)
		return "COLOR_ATTACHMENT1";
	if (_attachment == graphics::bufferAttachment::COLOR_ATTACHMENT2)
		return "COLOR_ATTACHMENT2";
	if (_attachment == graphics::bufferAttachment::COLOR_ATTACHMENT3)
		return "COLOR_ATTACHMENT3";
	if (_attachment == graphics::bufferAttachment::COLOR_ATTACHMENT4)
		return "COLOR_ATTACHMENT4";
	if (_attachment == graphics::bufferAttachment::DEPTH_ATTACHMENT)
		return "DEPTH_ATTACHMENT";
	return "UNKNOWN";
}

bool isVkFboTraceEnabled()
{
	static const bool traceEnabled = std::getenv("REALITYVK_VK_TRACE_FBO") != nullptr;
	return traceEnabled;
}

bool isVkFboTraceVerboseEnabled()
{
	static const bool traceVerboseEnabled = std::getenv("REALITYVK_VK_TRACE_FBO_VERBOSE") != nullptr;
	return traceVerboseEnabled;
}

u32 vkFboTraceLimit()
{
	static const u32 traceLimit = []() -> u32 {
		const char * envLimit = std::getenv("REALITYVK_VK_TRACE_FBO_LIMIT");
		if (envLimit == nullptr || envLimit[0] == '\0')
			return 2000U;
		const u32 parsed = static_cast<u32>(std::strtoul(envLimit, nullptr, 10));
		return parsed == 0U ? 2000U : parsed;
	}();
	return traceLimit;
}

void vkFboTrace(const char * _fmt, ...)
{
	if (!isVkFboTraceEnabled())
		return;
	static u32 emittedCount = 0U;
	const u32 limit = vkFboTraceLimit();
	if (emittedCount >= limit)
		return;
	++emittedCount;
	char message[768];
	va_list args;
	va_start(args, _fmt);
	std::vsnprintf(message, sizeof(message), _fmt, args);
	va_end(args);
	LOG(LOG_WARNING, "VK FBO TRACE: %s", message);
	if (emittedCount == limit) {
		LOG(LOG_WARNING, "VK FBO TRACE: limit reached (%u); suppressing further FBO trace logs.", limit);
	}
}

u64 & offscreenSkippedDrawCalls()
{
	static u64 value = 0U;
	return value;
}

u64 & offscreenSkippedVertices()
{
	static u64 value = 0U;
	return value;
}

enum class DepthBlitFailureReason : u8 {
	kNone = 0,
	UnsupportedDefaultTarget,
	MissingAttachment,
	MissingTextureHandle,
	InvalidTextureResource,
	FormatMismatch,
	EmptyRegion,
	BackendFailure
};

struct DepthBlitStats {
	u64 attempts = 0U;
	u64 successes = 0U;
	u64 failures = 0U;
	u64 unsupportedDefaultTarget = 0U;
	u64 missingAttachment = 0U;
	u64 missingTextureHandle = 0U;
	u64 invalidTextureResource = 0U;
	u64 formatMismatch = 0U;
	u64 emptyRegion = 0U;
	u64 backendFailure = 0U;
};

DepthBlitStats & depthBlitStats()
{
	static DepthBlitStats stats{};
	return stats;
}

void recordDepthBlitFailure(DepthBlitFailureReason _reason)
{
	DepthBlitStats & stats = depthBlitStats();
	++stats.failures;
	switch (_reason) {
	case DepthBlitFailureReason::UnsupportedDefaultTarget:
		++stats.unsupportedDefaultTarget;
		break;
	case DepthBlitFailureReason::MissingAttachment:
		++stats.missingAttachment;
		break;
	case DepthBlitFailureReason::MissingTextureHandle:
		++stats.missingTextureHandle;
		break;
	case DepthBlitFailureReason::InvalidTextureResource:
		++stats.invalidTextureResource;
		break;
	case DepthBlitFailureReason::FormatMismatch:
		++stats.formatMismatch;
		break;
	case DepthBlitFailureReason::EmptyRegion:
		++stats.emptyRegion;
		break;
	case DepthBlitFailureReason::BackendFailure:
		++stats.backendFailure;
		break;
	case DepthBlitFailureReason::kNone:
	default:
		break;
	}
}

const char * depthBlitFailureReasonName(DepthBlitFailureReason _reason)
{
	switch (_reason) {
	case DepthBlitFailureReason::kNone:
		return "none";
	case DepthBlitFailureReason::UnsupportedDefaultTarget:
		return "unsupported_default_target";
	case DepthBlitFailureReason::MissingAttachment:
		return "missing_attachment";
	case DepthBlitFailureReason::MissingTextureHandle:
		return "missing_texture_handle";
	case DepthBlitFailureReason::InvalidTextureResource:
		return "invalid_texture_resource";
	case DepthBlitFailureReason::FormatMismatch:
		return "format_mismatch";
	case DepthBlitFailureReason::EmptyRegion:
		return "empty_region";
	case DepthBlitFailureReason::BackendFailure:
		return "backend_failure";
	default:
		return "unknown";
	}
}

const std::array<graphics::BufferAttachmentParam, 5> & colorAttachmentCandidates()
{
	static const std::array<graphics::BufferAttachmentParam, 5> candidates = {
		graphics::bufferAttachment::COLOR_ATTACHMENT0,
		graphics::bufferAttachment::COLOR_ATTACHMENT1,
		graphics::bufferAttachment::COLOR_ATTACHMENT2,
		graphics::bufferAttachment::COLOR_ATTACHMENT3,
		graphics::bufferAttachment::COLOR_ATTACHMENT4
	};
	return candidates;
}

bool isColorAttachment(graphics::BufferAttachmentParam _attachment)
{
	for (graphics::BufferAttachmentParam candidate : colorAttachmentCandidates()) {
		if (candidate == _attachment)
			return true;
	}
	return false;
}

const vulkan::FramebufferStore::FramebufferAttachment * resolveColorAttachment(
	const vulkan::FramebufferStore & _store,
	graphics::ObjectHandle _framebuffer,
	graphics::BufferAttachmentParam _preferredAttachment,
	graphics::BufferAttachmentParam * _resolvedAttachment,
	u32 _maxColorAttachments = 5U)
{
	if (_resolvedAttachment != nullptr)
		*_resolvedAttachment = graphics::bufferAttachment::COLOR_ATTACHMENT0;
	if (!_framebuffer.isNotNull())
		return nullptr;
	const auto & candidates = colorAttachmentCandidates();
	const u32 availableCandidates = static_cast<u32>(candidates.size());
	const u32 primaryCandidateCount = std::max<u32>(
		1U,
		std::min<u32>(_maxColorAttachments, availableCandidates));

	auto isPrimaryCandidate = [&](graphics::BufferAttachmentParam _attachment) -> bool {
		for (u32 i = 0U; i < primaryCandidateCount; ++i) {
			if (candidates[i] == _attachment)
				return true;
		}
		return false;
	};

	auto tryAttachment = [&](graphics::BufferAttachmentParam _attachment) -> const vulkan::FramebufferStore::FramebufferAttachment * {
		const vulkan::FramebufferStore::FramebufferAttachment * attachment = _store.getAttachment(_framebuffer, _attachment);
		if (attachment == nullptr || !attachment->textureHandle.isNotNull())
			return nullptr;
		if (_resolvedAttachment != nullptr)
			*_resolvedAttachment = _attachment;
		return attachment;
	};

	if (isColorAttachment(_preferredAttachment) && isPrimaryCandidate(_preferredAttachment)) {
		if (const vulkan::FramebufferStore::FramebufferAttachment * preferred = tryAttachment(_preferredAttachment))
			return preferred;
	}
	for (u32 i = 0U; i < primaryCandidateCount; ++i) {
		const graphics::BufferAttachmentParam candidate = candidates[i];
		if (candidate == _preferredAttachment)
			continue;
		if (const vulkan::FramebufferStore::FramebufferAttachment * attachment = tryAttachment(candidate))
			return attachment;
	}
	for (u32 i = primaryCandidateCount; i < availableCandidates; ++i) {
		const graphics::BufferAttachmentParam candidate = candidates[i];
		if (candidate == _preferredAttachment)
			continue;
		if (const vulkan::FramebufferStore::FramebufferAttachment * attachment = tryAttachment(candidate))
			return attachment;
	}
	return nullptr;
}

bool maskIncludes(graphics::Parameter _mask, graphics::BlitMaskParam _flag)
{
	return (static_cast<u32>(_mask) & static_cast<u32>(_flag)) != 0U;
}

const char * transformModeName(vulkan::VertexTransformMode _mode)
{
	if (_mode == vulkan::VertexTransformMode::Rect)
		return "rect";
	return "triangle";
}

enum : u32 {
	kPacketSourceUnknown = 0U,
	kPacketSourceTriangles = 1U,
	kPacketSourceRects = 2U,
	kPacketSourceLines = 3U,
	kPacketSourceBlitDefault = 4U,
	kPacketSourceFallbackFbo = 5U
};

u64 nextVkDrawPacketId()
{
	static u64 value = 1U;
	return value++;
}

const char * packetSourceName(u32 _source)
{
	switch (_source) {
	case kPacketSourceTriangles:
		return "triangles";
	case kPacketSourceRects:
		return "rects";
	case kPacketSourceLines:
		return "lines";
	case kPacketSourceBlitDefault:
		return "blit_default";
	case kPacketSourceFallbackFbo:
		return "fallback_fbo";
	default:
		return "unknown";
	}
}

void assignPacketDebugMetadata(
	vulkan::DrawPacket & _packet,
	u32 _source,
	const graphics::CombinerProgram * _combiner,
	bool _texrect)
{
	_packet.debugPacketId = nextVkDrawPacketId();
	_packet.debugSource = _source;
	_packet.debugTexrect = _texrect;
	if (_combiner != nullptr) {
		const CombinerKey & key = _combiner->getKey();
		_packet.debugCombinerMux = static_cast<u64>(key.getMux());
		_packet.debugCombinerCycleType = key.getCycleType();
	} else {
		_packet.debugCombinerMux = 0U;
		_packet.debugCombinerCycleType = 0U;
	}
}

void appendTriangleVertex(const SPVertex & _src, bool _flatColors, vulkan::DrawPacket & _packet)
{
	vulkan::DrawVertex dst{};
	dst.x = _src.x;
	dst.y = _src.y;
	dst.z = _src.z;
	dst.w = _src.w;
	if (_flatColors) {
		dst.r = _src.flat_r;
		dst.g = _src.flat_g;
		dst.b = _src.flat_b;
		dst.a = _src.flat_a;
	} else {
		dst.r = _src.r;
		dst.g = _src.g;
		dst.b = _src.b;
		dst.a = _src.a;
	}
	dst.s0 = _src.s;
	dst.t0 = _src.t;
	dst.s1 = _src.s;
	dst.t1 = _src.t;
	dst.bc0 = _src.bc0;
	dst.bc1 = _src.bc1;
	dst.modify = _src.modify;
	_packet.vertices.push_back(dst);
}

void appendRectVertex(const RectVertex & _src, vulkan::DrawPacket & _packet)
{
	vulkan::DrawVertex dst{};
	dst.x = _src.x;
	dst.y = _src.y;
	dst.z = _src.z;
	dst.w = _src.w;
	// Upstream GL path feeds rect shading through uRectColor. Mirror that by
	// stamping per-vertex rect color from gDP.rectColor in Vulkan packets.
	dst.r = gDP.rectColor.r;
	dst.g = gDP.rectColor.g;
	dst.b = gDP.rectColor.b;
	dst.a = gDP.rectColor.a;
	dst.s0 = _src.s0;
	dst.t0 = _src.t0;
	dst.s1 = _src.s1;
	dst.t1 = _src.t1;
	dst.bc0 = _src.bc0;
	dst.bc1 = _src.bc1;
	dst.modify = 0U;
	_packet.vertices.push_back(dst);
}

void appendLineVertex(const SPVertex & _src, vulkan::DrawPacket & _packet)
{
	vulkan::DrawVertex dst{};
	dst.x = _src.x;
	dst.y = _src.y;
	dst.z = _src.z;
	dst.w = _src.w;
	dst.r = _src.r;
	dst.g = _src.g;
	dst.b = _src.b;
	dst.a = _src.a;
	dst.s0 = _src.s;
	dst.t0 = _src.t;
	dst.s1 = _src.s;
	dst.t1 = _src.t;
	dst.bc0 = _src.bc0;
	dst.bc1 = _src.bc1;
	dst.modify = _src.modify;
	_packet.vertices.push_back(dst);
}

void applyLegacyScreenTransform(f32 & _x, f32 & _y, f32 _w)
{
	constexpr f32 kHalfScreenSize = 320.0f;
	_x -= kHalfScreenSize * _w;
	_y -= kHalfScreenSize * _w;
	_x /= kHalfScreenSize;
	_y /= kHalfScreenSize;
}

bool normalizeVertexWithRasterViewport(
	const vulkan::RasterState & _raster,
	const vulkan::DrawVertex & _src,
	vulkan::DrawVertex & _dst)
{
	if (!_raster.viewportValid || _raster.viewportWidth == 0 || _raster.viewportHeight == 0)
		return false;

	const f32 viewportX = static_cast<f32>(_raster.viewportX);
	const f32 viewportY = static_cast<f32>(_raster.viewportY);
	const f32 viewportWidth = std::max(1.0f, static_cast<f32>(std::abs(_raster.viewportWidth)));
	const f32 viewportHeight = std::max(1.0f, static_cast<f32>(std::abs(_raster.viewportHeight)));

	_dst.x = ((_src.x - viewportX) / viewportWidth) * 2.0f - 1.0f;
	_dst.y = ((_src.y - viewportY) / viewportHeight) * 2.0f - 1.0f;
	_dst.w = 1.0f;
	return true;
}

bool normalizeRectWithGspViewport(const vulkan::DrawVertex & _src, vulkan::DrawVertex & _dst)
{
	const f32 viewportWidth = std::abs(gSP.viewport.width);
	const f32 viewportHeight = std::abs(gSP.viewport.height);
	if (viewportWidth < 0.5f || viewportHeight < 0.5f)
		return false;

	_dst.x = ((_src.x - gSP.viewport.x) / viewportWidth) * 2.0f - 1.0f;
	_dst.y = ((_src.y - gSP.viewport.y) / viewportHeight) * 2.0f - 1.0f;
	_dst.w = 1.0f;
	return true;
}

bool normalizeRectWithDpScissor(const vulkan::DrawVertex & _src, vulkan::DrawVertex & _dst)
{
	f32 left = gDP.scissor.ulx;
	f32 top = gDP.scissor.uly;
	f32 width = std::abs(gDP.scissor.lrx - gDP.scissor.ulx);
	f32 height = std::abs(gDP.scissor.lry - gDP.scissor.uly);

	// Fill-cycle rect coordinates often live in color-image space (for example 320x240)
	// even when current scissor reports a much smaller UI viewport. Prefer color-image
	// normalization when scissor extents are implausibly small.
	f32 imageWidth = static_cast<f32>(gDP.colorImage.width);
	if (imageWidth < 0.5f)
		imageWidth = 320.0f;
	f32 imageHeight = imageWidth * 0.75f;
	if (imageHeight < 0.5f)
		imageHeight = 240.0f;

	const bool preferImageSpace =
		width < 0.5f
		|| height < 0.5f
		|| width < imageWidth * 0.5f
		|| height < imageHeight * 0.5f;
	if (preferImageSpace) {
		left = 0.0f;
		top = 0.0f;
		width = imageWidth;
		height = imageHeight;
	}

	_dst.x = ((_src.x - left) / width) * 2.0f - 1.0f;
	_dst.y = ((_src.y - top) / height) * 2.0f - 1.0f;
	_dst.w = 1.0f;
	return true;
}

bool normalizeRectWithColorImageSpace(const vulkan::DrawVertex & _src, vulkan::DrawVertex & _dst)
{
	f32 width = static_cast<f32>(gDP.colorImage.width);
	if (width < 0.5f)
		width = 320.0f;
	f32 height = width * 0.75f;
	if (height < 0.5f)
		height = 240.0f;

	_dst.x = (_src.x / width) * 2.0f - 1.0f;
	_dst.y = (_src.y / height) * 2.0f - 1.0f;
	_dst.w = 1.0f;
	return true;
}

vulkan::DrawVertex normalizeFallbackVertex(const vulkan::DrawPacket & _packet, const vulkan::DrawVertex & _src)
{
	static const bool disablePositionNormalize = std::getenv("REALITYVK_VK_DISABLE_POSITION_NORMALIZE") != nullptr;
	static const bool disableForceRasterRectTransform = std::getenv("REALITYVK_VK_DISABLE_FORCE_RASTER_RECT_TRANSFORM") != nullptr;
	static const bool debugRectNormalize = std::getenv("REALITYVK_VK_DEBUG_RECT_NORMALIZE") != nullptr;
	static const u32 debugRectNormalizeLimit = []() -> u32 {
		const char * env = std::getenv("REALITYVK_VK_DEBUG_RECT_NORMALIZE_LIMIT");
		if (env == nullptr || env[0] == '\0')
			return 96U;
		return static_cast<u32>(std::strtoul(env, nullptr, 10));
	}();
	static u32 debugRectNormalizeCount = 0U;
	if (disablePositionNormalize)
		return _src;

	vulkan::DrawVertex dst = _src;
	const bool forceRasterRectTransform = _packet.forceRasterRectTransform && !disableForceRasterRectTransform;
	const bool clipLikeVertex = std::abs(_src.x) <= 2.0f && std::abs(_src.y) <= 2.0f && std::abs(_src.w) <= 2.0f;
	const bool trustClipLikeVertex = _packet.positionsNormalized || !forceRasterRectTransform;
	const bool fillCycleRectNeedsNormalization =
		_packet.transformMode == vulkan::VertexTransformMode::Rect
		&& !_packet.positionsNormalized
		&& !_packet.debugTexrect
		&& _packet.debugCombinerCycleType == G_CYC_FILL
		&& _packet.textureSlotMask == 0U;
	if (clipLikeVertex && trustClipLikeVertex && !fillCycleRectNeedsNormalization)
		return dst;

	// Some paths feed homogeneous coordinates with large w and clip-space-scaled x/y.
	// Recover NDC first so triangle-mode legacy transform does not explode them off-screen.
	const f32 absW = std::abs(_src.w);
	if (absW > 2.0f) {
		const f32 conservativeW = absW * 1.25f;
		if (std::abs(_src.x) <= conservativeW && std::abs(_src.y) <= conservativeW) {
			const f32 invW = 1.0f / _src.w;
			dst.x = _src.x * invW;
			dst.y = _src.y * invW;
			dst.z = _src.z * invW;
			dst.w = 1.0f;
			return dst;
		}
	}

	if (_packet.transformMode == vulkan::VertexTransformMode::Triangle) {
		const bool modifyXY = (_src.modify & MODIFY_XY) != 0U;
		const bool modifyZ = (_src.modify & MODIFY_Z) != 0U;
		if (modifyXY) {
			dst.x = _src.x * _src.w;
			dst.y = _src.y * _src.w;
		} else {
			dst.x = _src.x * gSP.viewport.vscale[0] + gSP.viewport.vtrans[0] * _src.w;
			dst.y = _src.y * (-gSP.viewport.vscale[1]) + gSP.viewport.vtrans[1] * _src.w;
			dst.x = std::floor(dst.x * 4.0f) * 0.25f;
			dst.y = std::floor(dst.y * 4.0f) * 0.25f;
		}
		if (modifyZ)
			dst.z = _src.z * _src.w;
		applyLegacyScreenTransform(dst.x, dst.y, _src.w);
		// Legacy triangle transform can occasionally explode when input vertices
		// are already close to window/clip space; fall back to viewport remap.
		if (std::abs(dst.x) <= 8.0f && std::abs(dst.y) <= 8.0f)
			return dst;
		dst = _src;
	}

	if (_packet.transformMode == vulkan::VertexTransformMode::Rect) {
		const bool fillCycleRect = !_packet.debugTexrect && _packet.debugCombinerCycleType == G_CYC_FILL;
		if (fillCycleRect) {
			f32 imageWidth = static_cast<f32>(gDP.colorImage.width);
			if (imageWidth < 0.5f)
				imageWidth = 320.0f;
			f32 imageHeight = imageWidth * 0.75f;
			if (imageHeight < 0.5f)
				imageHeight = 240.0f;

			if (!_packet.vertices.empty()) {
				f32 minX = _packet.vertices[0].x;
				f32 maxX = minX;
				f32 minY = _packet.vertices[0].y;
				f32 maxY = minY;
				for (const vulkan::DrawVertex & packetVertex : _packet.vertices) {
					minX = std::min(minX, packetVertex.x);
					maxX = std::max(maxX, packetVertex.x);
					minY = std::min(minY, packetVertex.y);
					maxY = std::max(maxY, packetVertex.y);
				}
				const f32 spanX = maxX - minX;
				const f32 spanY = maxY - minY;
				const bool fullWidthThinBand = spanX >= imageWidth * 0.9f && spanY <= imageHeight * 0.25f;
				if (fullWidthThinBand && normalizeRectWithColorImageSpace(_src, dst)) {
					if (debugRectNormalize && debugRectNormalizeCount < debugRectNormalizeLimit) {
						LOG(LOG_WARNING, "VK rect normalize debug: branch=color-image-fill-band result=[%0.3f,%0.3f]", dst.x, dst.y);
						++debugRectNormalizeCount;
					}
					return dst;
				}
			}

			if (normalizeRectWithDpScissor(_src, dst)) {
				if (debugRectNormalize && debugRectNormalizeCount < debugRectNormalizeLimit) {
					LOG(LOG_WARNING, "VK rect normalize debug: branch=dp-scissor-fill result=[%0.3f,%0.3f]", dst.x, dst.y);
					++debugRectNormalizeCount;
				}
				return dst;
			}
		}

		if (debugRectNormalize && debugRectNormalizeCount < debugRectNormalizeLimit) {
			LOG(
				LOG_WARNING,
					"VK rect normalize debug: forceRaster=%u src=[%0.3f,%0.3f] rasterViewport=%d,%d,%d,%d valid=%u gspViewport=[x=%0.3f y=%0.3f w=%0.3f h=%0.3f]",
					forceRasterRectTransform ? 1U : 0U,
					_src.x,
					_src.y,
				_packet.state.raster.viewportX,
				_packet.state.raster.viewportY,
				_packet.state.raster.viewportWidth,
				_packet.state.raster.viewportHeight,
				_packet.state.raster.viewportValid ? 1U : 0U,
				gSP.viewport.x,
				gSP.viewport.y,
				gSP.viewport.width,
				gSP.viewport.height);
		}
			if (forceRasterRectTransform) {
			if (normalizeVertexWithRasterViewport(_packet.state.raster, _src, dst)) {
				if (debugRectNormalize && debugRectNormalizeCount < debugRectNormalizeLimit) {
					LOG(LOG_WARNING, "VK rect normalize debug: branch=raster result=[%0.3f,%0.3f]", dst.x, dst.y);
					++debugRectNormalizeCount;
				}
				return dst;
			}
			if (normalizeRectWithGspViewport(_src, dst)) {
				if (debugRectNormalize && debugRectNormalizeCount < debugRectNormalizeLimit) {
					LOG(LOG_WARNING, "VK rect normalize debug: branch=gsp-fallback result=[%0.3f,%0.3f]", dst.x, dst.y);
					++debugRectNormalizeCount;
				}
				return dst;
			}
			applyLegacyScreenTransform(dst.x, dst.y, _src.w);
			if (debugRectNormalize && debugRectNormalizeCount < debugRectNormalizeLimit) {
				LOG(LOG_WARNING, "VK rect normalize debug: branch=legacy-fallback result=[%0.3f,%0.3f]", dst.x, dst.y);
				++debugRectNormalizeCount;
			}
			return dst;
		}
		// Rect vertices usually arrive in viewport space and should be normalized
		// against the active RSP viewport instead of a fixed 640x640 legacy basis.
		if (normalizeRectWithGspViewport(_src, dst)) {
			if (debugRectNormalize && debugRectNormalizeCount < debugRectNormalizeLimit) {
				LOG(LOG_WARNING, "VK rect normalize debug: branch=gsp result=[%0.3f,%0.3f]", dst.x, dst.y);
				++debugRectNormalizeCount;
			}
			return dst;
		}
		if (normalizeVertexWithRasterViewport(_packet.state.raster, _src, dst)) {
			if (debugRectNormalize && debugRectNormalizeCount < debugRectNormalizeLimit) {
				LOG(LOG_WARNING, "VK rect normalize debug: branch=raster-fallback result=[%0.3f,%0.3f]", dst.x, dst.y);
				++debugRectNormalizeCount;
			}
			return dst;
		}
		applyLegacyScreenTransform(dst.x, dst.y, _src.w);
		if (debugRectNormalize && debugRectNormalizeCount < debugRectNormalizeLimit) {
			LOG(LOG_WARNING, "VK rect normalize debug: branch=legacy result=[%0.3f,%0.3f]", dst.x, dst.y);
			++debugRectNormalizeCount;
		}
		return dst;
	}

	if (!normalizeVertexWithRasterViewport(_packet.state.raster, _src, dst))
		return dst;
	return dst;
}

void normalizePacketPositions(vulkan::DrawPacket & _packet)
{
	static const bool debugPositions = std::getenv("REALITYVK_VK_DEBUG_POSITIONS") != nullptr;
	f32 preMinX = 0.0f;
	f32 preMaxX = 0.0f;
	f32 preMinY = 0.0f;
	f32 preMaxY = 0.0f;
	if (debugPositions && !_packet.vertices.empty()) {
		preMinX = preMaxX = _packet.vertices[0].x;
		preMinY = preMaxY = _packet.vertices[0].y;
		for (const vulkan::DrawVertex & vertex : _packet.vertices) {
			preMinX = std::min(preMinX, vertex.x);
			preMaxX = std::max(preMaxX, vertex.x);
			preMinY = std::min(preMinY, vertex.y);
			preMaxY = std::max(preMaxY, vertex.y);
		}
	}

	for (vulkan::DrawVertex & vertex : _packet.vertices) {
		vertex = normalizeFallbackVertex(_packet, vertex);
	}
	_packet.positionsNormalized = true;

	if (debugPositions && !_packet.vertices.empty()) {
		static const u32 positionLogLimit = []() -> u32 {
			const char * env = std::getenv("REALITYVK_VK_DEBUG_POSITIONS_LIMIT");
			if (env == nullptr || env[0] == '\0')
				return 128U;
			return static_cast<u32>(std::strtoul(env, nullptr, 10));
		}();
		static u32 positionLogCount = 0U;
		if (positionLogCount < positionLogLimit) {
			f32 postMinX = _packet.vertices[0].x;
			f32 postMaxX = postMinX;
			f32 postMinY = _packet.vertices[0].y;
			f32 postMaxY = postMinY;
			for (const vulkan::DrawVertex & vertex : _packet.vertices) {
				postMinX = std::min(postMinX, vertex.x);
				postMaxX = std::max(postMaxX, vertex.x);
				postMinY = std::min(postMinY, vertex.y);
				postMaxY = std::max(postMaxY, vertex.y);
			}
			LOG(
				LOG_WARNING,
				"VK position debug: pre=[x=%0.3f..%0.3f y=%0.3f..%0.3f] post=[x=%0.3f..%0.3f y=%0.3f..%0.3f] viewport=%d,%d,%d,%d valid=%u scissor=%d,%d,%d,%d enabled=%u",
				preMinX,
				preMaxX,
				preMinY,
				preMaxY,
				postMinX,
				postMaxX,
				postMinY,
				postMaxY,
				_packet.state.raster.viewportX,
				_packet.state.raster.viewportY,
				_packet.state.raster.viewportWidth,
				_packet.state.raster.viewportHeight,
				_packet.state.raster.viewportValid ? 1U : 0U,
				_packet.state.raster.scissorX,
				_packet.state.raster.scissorY,
				_packet.state.raster.scissorWidth,
				_packet.state.raster.scissorHeight,
				_packet.state.raster.scissorEnabled ? 1U : 0U);
			++positionLogCount;
		}
	}
}

#endif

} // namespace

namespace vulkan {

struct ContextImpl::VulkanState
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	struct FrameSync {
		VkSemaphore imageAvailable = VK_NULL_HANDLE;
		VkSemaphore renderFinished = VK_NULL_HANDLE;
		VkFence inFlight = VK_NULL_HANDLE;
	};

	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue graphicsQueue = VK_NULL_HANDLE;
	VkQueue presentQueue = VK_NULL_HANDLE;
	u32 graphicsQueueFamily = UINT32_MAX;
	u32 presentQueueFamily = UINT32_MAX;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
	VkExtent2D swapchainExtent = {};
	VkFormat depthFormat = VK_FORMAT_UNDEFINED;
	std::vector<VkImage> swapchainImages;
	std::vector<VkImageView> swapchainImageViews;
	std::vector<VkImage> swapchainDepthImages;
	std::vector<VkDeviceMemory> swapchainDepthMemories;
	std::vector<VkImageView> swapchainDepthImageViews;
	VkRenderPass renderPass = VK_NULL_HANDLE;
	std::vector<VkFramebuffer> swapchainFramebuffers;
	std::vector<VkFence> swapchainImageFences;
	VkCommandPool commandPool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> frameCommandBuffers;
	std::vector<FrameSync> frameSync;
	DrawRecorder drawRecorder;
	TextureStore textureStore;
	FramebufferStore framebufferStore;
	BindingState bindingState;
	UploadArena uploadArena;
	DescriptorLayoutRegistry descriptorLayoutRegistry;
	DescriptorBinder descriptorBinder;
	ProgramLibrary programLibrary;
	PipelineCache pipelineCache;
	u32 frameSyncIndex = 0;
	u32 lastPresentedImageIndex = UINT32_MAX;
	graphics::ObjectHandle lastNonDefaultDrawFramebuffer = graphics::ObjectHandle::null;
	graphics::ObjectHandle lastNonDefaultReadFramebuffer = graphics::ObjectHandle::null;
	u64 framebufferBindSerial = 0U;
	u64 lastNonDefaultDrawFramebufferSerial = 0U;
	u64 lastNonDefaultReadFramebufferSerial = 0U;
	graphics::ObjectHandle lastColorBlitDrawFramebuffer = graphics::ObjectHandle::null;
	u64 lastColorBlitDrawFramebufferSerial = 0U;
	bool wideLinesEnabled = false;
	bool dualSrcBlendEnabled = false;
	bool surfaceExtensionEnabled = false;
	bool xlibSurfaceExtensionEnabled = false;
	bool win32SurfaceExtensionEnabled = false;
	bool swapchainExtensionEnabled = false;
#endif
};

ContextImpl::ContextImpl()
	: m_clampMode(graphics::ClampMode::ClippingEnabled)
	, m_textureUnpackAlignment(1)
	, m_maxTextureSize(4096)
	, m_maxMsaaLevel(1)
	, m_maxLineWidth(1.0f)
	, m_maxAnisotropy(1.0f)
	, m_nextHandle(1U)
	, m_coreReady(false)
	, m_presentationWindowInfo()
{
}

ContextImpl::~ContextImpl()
{
	destroy();
}

bool ContextImpl::hasVulkanSupport()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	return true;
#else
	return false;
#endif
}

void ContextImpl::setPresentationWindowInfo(const graphics::Context::PresentationWindowInfo & _info)
{
	const bool changed = m_presentationWindowInfo.system != _info.system
		|| m_presentationWindowInfo.display != _info.display
		|| m_presentationWindowInfo.window != _info.window
		|| m_presentationWindowInfo.width != _info.width
		|| m_presentationWindowInfo.height != _info.height;
	m_presentationWindowInfo = _info;

#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!changed || !m_vk || m_vk->instance == VK_NULL_HANDLE)
		return;

	if (m_vk->device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(m_vk->device);

	destroySwapchain();
	destroySurface();
	if (createSurface())
		createSwapchain();
#endif
}

void ContextImpl::init()
{
	initFramebufferFormats();
	if (!initializeVulkanCore()) {
		LOG(LOG_WARNING, "Vulkan core init failed. Backend remains in bootstrap mode.");
	}
}

void ContextImpl::destroy()
{
	shutdownVulkanCore();
}

void ContextImpl::setClampMode(graphics::ClampMode _mode)
{
	m_clampMode = _mode;
}

graphics::ClampMode ContextImpl::getClampMode()
{
	return m_clampMode;
}

void ContextImpl::enable(graphics::EnableParam _parameter, bool _enable)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.enable(_parameter, _enable);
#else
	(void)_parameter;
	(void)_enable;
#endif
}

u32 ContextImpl::isEnabled(graphics::EnableParam _parameter)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return 0U;
	return m_vk->drawRecorder.isEnabled(_parameter);
#else
	(void)_parameter;
	return 0U;
#endif
}

void ContextImpl::cullFace(graphics::CullModeParam _mode)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.cullFace(_mode);
#else
	(void)_mode;
#endif
}

void ContextImpl::enableDepthWrite(bool _enable)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.enableDepthWrite(_enable);
#else
	(void)_enable;
#endif
}

void ContextImpl::setDepthCompare(graphics::CompareParam _mode)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.setDepthCompare(_mode);
#else
	(void)_mode;
#endif
}

void ContextImpl::setViewport(s32 _x, s32 _y, s32 _width, s32 _height)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.setViewport(_x, _y, _width, _height);
#else
	(void)_x;
	(void)_y;
	(void)_width;
	(void)_height;
#endif
}

void ContextImpl::setScissor(s32 _x, s32 _y, s32 _width, s32 _height)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.setScissor(_x, _y, _width, _height);
#else
	(void)_x;
	(void)_y;
	(void)_width;
	(void)_height;
#endif
}

void ContextImpl::setBlending(graphics::BlendParam _sfactor, graphics::BlendParam _dfactor)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.setBlending(_sfactor, _dfactor);
#else
	(void)_sfactor;
	(void)_dfactor;
#endif
}

void ContextImpl::setBlendingSeparate(graphics::BlendParam _sfactorcolor, graphics::BlendParam _dfactorcolor, graphics::BlendParam _sfactoralpha, graphics::BlendParam _dfactoralpha)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.setBlendingSeparate(_sfactorcolor, _dfactorcolor, _sfactoralpha, _dfactoralpha);
#else
	(void)_sfactorcolor;
	(void)_dfactorcolor;
	(void)_sfactoralpha;
	(void)_dfactoralpha;
#endif
}

void ContextImpl::setBlendColor(f32 _red, f32 _green, f32 _blue, f32 _alpha)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.setBlendColor(_red, _green, _blue, _alpha);
#else
	(void)_red;
	(void)_green;
	(void)_blue;
	(void)_alpha;
#endif
}

void ContextImpl::clearColorBuffer(f32 _red, f32 _green, f32 _blue, f32 _alpha)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	if (isDefaultDrawFramebufferBound()) {
		m_vk->drawRecorder.setClearColor(_red, _green, _blue, _alpha);
		vkFboTrace(
			"op=clear_color target=default color=[%.3f,%.3f,%.3f,%.3f]",
			_red,
			_green,
			_blue,
			_alpha);
		return;
	}
	const graphics::ObjectHandle drawFramebuffer = m_vk->drawRecorder.drawFramebufferBinding();
	const FramebufferStore::FramebufferResource * framebuffer = m_vk->framebufferStore.getFramebuffer(drawFramebuffer);
	if (framebuffer == nullptr) {
		vkFboTrace(
			"op=clear_color target=fbo drawFbo=%u status=missing_framebuffer",
			static_cast<u32>(drawFramebuffer));
		return;
	}
	u32 clearedAttachments = 0U;
	for (const auto & attachmentEntry : framebuffer->attachments) {
		const FramebufferStore::FramebufferAttachment & attachment = attachmentEntry.second;
		if (!isColorAttachment(attachment.attachment))
			continue;
		if (!attachment.textureHandle.isNotNull())
			continue;
		const bool clearOk = m_vk->textureStore.clearTextureColor(attachment.textureHandle, _red, _green, _blue, _alpha);
		++clearedAttachments;
		vkFboTrace(
			"op=clear_color_attachment drawFbo=%u attachment=%s texture=%u ok=%u color=[%.3f,%.3f,%.3f,%.3f]",
			static_cast<u32>(drawFramebuffer),
			bufferAttachmentName(attachment.attachment),
			static_cast<u32>(attachment.textureHandle),
			clearOk ? 1U : 0U,
			_red,
			_green,
			_blue,
			_alpha);
	}
	vkFboTrace(
		"op=clear_color target=fbo drawFbo=%u touched=%u",
		static_cast<u32>(drawFramebuffer),
		clearedAttachments);
#else
	(void)_red;
	(void)_green;
	(void)_blue;
	(void)_alpha;
#endif
}

void ContextImpl::clearDepthBuffer()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	if (isDefaultDrawFramebufferBound()) {
		m_vk->drawRecorder.setClearDepth(1.0f);
		vkFboTrace("op=clear_depth target=default depth=1.000");
		return;
	}
	const graphics::ObjectHandle drawFramebuffer = m_vk->drawRecorder.drawFramebufferBinding();
	const FramebufferStore::FramebufferAttachment * depthAttachment = m_vk->framebufferStore.getAttachment(
		drawFramebuffer,
		graphics::bufferAttachment::DEPTH_ATTACHMENT);
	if (depthAttachment == nullptr || !depthAttachment->textureHandle.isNotNull()) {
		vkFboTrace(
			"op=clear_depth target=fbo drawFbo=%u status=missing_depth_attachment",
			static_cast<u32>(drawFramebuffer));
		return;
	}
	const bool clearOk = m_vk->textureStore.clearTextureDepth(depthAttachment->textureHandle, 1.0f);
	vkFboTrace(
		"op=clear_depth target=fbo drawFbo=%u texture=%u ok=%u depth=1.000",
		static_cast<u32>(drawFramebuffer),
		static_cast<u32>(depthAttachment->textureHandle),
		clearOk ? 1U : 0U);
#endif
}

void ContextImpl::setPolygonOffset(f32 _factor, f32 _units)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.setPolygonOffset(_factor, _units);
#else
	(void)_factor;
	(void)_units;
#endif
}

graphics::ObjectHandle ContextImpl::createTexture(graphics::Parameter _target)
{
	graphics::ObjectHandle handle = _allocateHandle();
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (m_vk)
		m_vk->textureStore.createTexture(handle, _target);
#else
	(void)_target;
#endif
	return handle;
}

void ContextImpl::deleteTexture(graphics::ObjectHandle _name)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->textureStore.deleteTexture(_name);
	m_vk->bindingState.clearTextureBindings(_name);
#else
	(void)_name;
#endif
}

void ContextImpl::init2DTexture(const graphics::Context::InitTextureParams & _params)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->textureStore.initTexture(_params, m_textureUnpackAlignment);
	if (_params.handle.isNotNull()) {
		graphics::Context::BindTextureParameters bindParams{};
		bindParams.texture = _params.handle;
		bindParams.textureUnitIndex = _params.textureUnitIndex;
		bindParams.target = _params.target;
		m_vk->bindingState.bindTexture(bindParams);
	}
#else
	(void)_params;
#endif
}

void ContextImpl::update2DTexture(const graphics::Context::UpdateTextureDataParams & _params)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->textureStore.updateTexture(_params, m_textureUnpackAlignment);
	if (_params.handle.isNotNull()) {
		graphics::Context::BindTextureParameters bindParams{};
		bindParams.texture = _params.handle;
		bindParams.textureUnitIndex = _params.textureUnitIndex;
		bindParams.target = graphics::textureTarget::TEXTURE_2D;
		m_vk->bindingState.bindTexture(bindParams);
	}
#else
	(void)_params;
#endif
}

void ContextImpl::setTextureParameters(const graphics::Context::TexParameters & _parameters)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->textureStore.setTextureParameters(_parameters);
	graphics::Context::BindTextureParameters bindParams{};
	bindParams.texture = _parameters.handle;
	bindParams.textureUnitIndex = _parameters.textureUnitIndex;
	bindParams.target = _parameters.target;
	m_vk->bindingState.bindTexture(bindParams);
#else
	(void)_parameters;
#endif
}

void ContextImpl::bindTexture(const graphics::Context::BindTextureParameters & _params)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	static const bool debugBindings = std::getenv("REALITYVK_VK_DEBUG_BINDINGS") != nullptr;
	if (debugBindings) {
		static u32 debugBindTextureLogCount = 0U;
		if (debugBindTextureLogCount < 64U) {
			LOG(
				LOG_WARNING,
				"VK bindings debug: bindTexture unit=%u texture=%u target=%u",
				static_cast<u32>(_params.textureUnitIndex),
				static_cast<u32>(_params.texture),
				static_cast<u32>(_params.target));
			++debugBindTextureLogCount;
		}
	}
	m_vk->textureStore.ensureTexture(_params.texture, _params.target);
	m_vk->bindingState.bindTexture(_params);
#else
	(void)_params;
#endif
}

void ContextImpl::setTextureUnpackAlignment(s32 _param)
{
	m_textureUnpackAlignment = _param;
}

s32 ContextImpl::getTextureUnpackAlignment() const
{
	return m_textureUnpackAlignment;
}

s32 ContextImpl::getMaxTextureSize() const
{
	return m_maxTextureSize;
}

f32 ContextImpl::getMaxAnisotropy() const
{
	return m_maxAnisotropy;
}

void ContextImpl::bindImageTexture(const graphics::Context::BindImageTextureParameters & _params)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->bindingState.bindImageTexture(_params);
#else
	(void)_params;
#endif
}

u32 ContextImpl::convertInternalTextureFormat(u32 _format) const
{
	return _format;
}

void ContextImpl::textureBarrier()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	// Vulkan image layout transitions and explicit offscreen submissions already
	// provide visibility; keep this as a conservative ordering point.
	if (m_vk->device != VK_NULL_HANDLE)
		(void)vkDeviceWaitIdle(m_vk->device);
	m_vk->drawRecorder.markAllStateDirty();
#endif
}

graphics::FramebufferTextureFormats * ContextImpl::getFramebufferTextureFormats()
{
	return new graphics::FramebufferTextureFormats(*m_fbTexFormats);
}

graphics::ObjectHandle ContextImpl::createFramebuffer()
{
	graphics::ObjectHandle handle = _allocateHandle();
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (m_vk)
		m_vk->framebufferStore.createFramebuffer(handle);
#endif
	return handle;
}

void ContextImpl::deleteFramebuffer(graphics::ObjectHandle _name)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->framebufferStore.deleteFramebuffer(_name);
#else
	(void)_name;
#endif
}

void ContextImpl::bindFramebuffer(graphics::BufferTargetParam _target, graphics::ObjectHandle _name)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.bindFramebuffer(_target, _name);
	if (_name.isNotNull()) {
		if (_target == graphics::bufferTarget::FRAMEBUFFER || _target == graphics::bufferTarget::DRAW_FRAMEBUFFER) {
			m_vk->lastNonDefaultDrawFramebuffer = _name;
			m_vk->lastNonDefaultDrawFramebufferSerial = ++m_vk->framebufferBindSerial;
		}
		if (_target == graphics::bufferTarget::FRAMEBUFFER || _target == graphics::bufferTarget::READ_FRAMEBUFFER) {
			m_vk->lastNonDefaultReadFramebuffer = _name;
			m_vk->lastNonDefaultReadFramebufferSerial = ++m_vk->framebufferBindSerial;
		}
	}
	vkFboTrace(
		"op=bind target=%s name=%u draw=%u read=%u",
		bufferTargetName(_target),
		static_cast<u32>(_name),
		static_cast<u32>(m_vk->drawRecorder.drawFramebufferBinding()),
		static_cast<u32>(m_vk->drawRecorder.readFramebufferBinding()));
#else
	(void)_target;
	(void)_name;
#endif
}

void ContextImpl::addFrameBufferRenderTarget(const graphics::Context::FrameBufferRenderTarget & _params)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->framebufferStore.addFramebufferRenderTarget(_params);
	vkFboTrace(
		"op=attach fbo=%u target=%s attachment=%s texture=%u textureTarget=%u",
		static_cast<u32>(_params.bufferHandle),
		bufferTargetName(_params.bufferTarget),
		bufferAttachmentName(_params.attachment),
		static_cast<u32>(_params.textureHandle),
		static_cast<u32>(_params.textureTarget));
#else
	(void)_params;
#endif
}

graphics::ObjectHandle ContextImpl::createRenderbuffer()
{
	graphics::ObjectHandle handle = _allocateHandle();
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (m_vk)
		m_vk->framebufferStore.createRenderbuffer(handle);
#endif
	return handle;
}

void ContextImpl::initRenderbuffer(const graphics::Context::InitRenderbufferParams & _params)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->framebufferStore.initRenderbuffer(_params);
#else
	(void)_params;
#endif
}

bool ContextImpl::blitFramebuffers(const graphics::Context::BlitFramebuffersParams & _params)
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	(void)_params;
	return false;
#else
	if (!m_vk)
		return false;
	vkFboTrace(
		"op=blit_begin readFbo=%u drawFbo=%u src=[%d,%d,%d,%d] dst=[%d,%d,%d,%d] mask=0x%x filter=0x%x",
		static_cast<u32>(_params.readBuffer),
		static_cast<u32>(_params.drawBuffer),
		_params.srcX0,
		_params.srcY0,
		_params.srcX1,
		_params.srcY1,
		_params.dstX0,
		_params.dstY0,
		_params.dstX1,
		_params.dstY1,
		static_cast<u32>(_params.mask),
		static_cast<u32>(_params.filter));
	m_vk->drawRecorder.bindFramebuffer(graphics::bufferTarget::READ_FRAMEBUFFER, _params.readBuffer);
	m_vk->drawRecorder.bindFramebuffer(graphics::bufferTarget::DRAW_FRAMEBUFFER, _params.drawBuffer);
	if (!_params.readBuffer.isNotNull())
		return false;
	const bool drawToDefault = _params.drawBuffer == graphics::ObjectHandle::defaultFramebuffer;
	if (!drawToDefault && !_params.drawBuffer.isNotNull())
		return false;

	const bool wantsColor = maskIncludes(_params.mask, graphics::blitMask::COLOR_BUFFER);
	const bool wantsDepth = maskIncludes(_params.mask, graphics::blitMask::DEPTH_BUFFER);
	if (!wantsColor && !wantsDepth)
		return false;
	const u32 readColorAttachmentLimit = m_vk->framebufferStore.getDrawBufferCount(_params.readBuffer);

		if (drawToDefault) {
			// Depth blit to the default framebuffer is handled by higher-level
			// fallback copy path. Do not enqueue partial color work and then fail.
			if (wantsDepth) {
				vkFboTrace(
					"op=blit_end readFbo=%u drawFbo=%u srcColorAttachment=%s colorOp=0 colorOk=0 depthOp=1 depthOk=0 depthReason=%s result=0 path=fallback_copy_required",
					static_cast<u32>(_params.readBuffer),
					static_cast<u32>(_params.drawBuffer),
					bufferAttachmentName(graphics::bufferAttachment::COLOR_ATTACHMENT0),
					depthBlitFailureReasonName(DepthBlitFailureReason::UnsupportedDefaultTarget));
				return false;
			}

			bool ok = true;
			bool colorOp = false;
			bool colorOk = false;
			graphics::BufferAttachmentParam srcColorAttachment = graphics::bufferAttachment::COLOR_ATTACHMENT0;
			bool depthOp = false;
		bool depthOk = false;
		DepthBlitFailureReason depthReason = DepthBlitFailureReason::kNone;

		if (wantsColor) {
			colorOp = true;
			const FramebufferStore::FramebufferAttachment * srcAttachment = resolveColorAttachment(
				m_vk->framebufferStore,
				_params.readBuffer,
				graphics::bufferAttachment::COLOR_ATTACHMENT0,
				&srcColorAttachment,
				readColorAttachmentLimit);
			const TextureStore::TextureResource * srcTexture = srcAttachment != nullptr
				? m_vk->textureStore.getTexture(srcAttachment->textureHandle)
				: nullptr;
			if (srcAttachment == nullptr
				|| !srcAttachment->textureHandle.isNotNull()
				|| srcTexture == nullptr
				|| srcTexture->width == 0U
				|| srcTexture->height == 0U) {
				ok = false;
			} else {
				DrawPacket packet{};
				packet.primitive = PrimitiveType::TriangleStrip;
				packet.transformMode = VertexTransformMode::Rect;
				packet.textureUnit0 = 0U;
				packet.textureUnit1 = 1U;
				packet.shaderFlags = vulkan::draw_shader_flags::kTexture0;
				assignPacketDebugMetadata(packet, kPacketSourceBlitDefault, nullptr, true);
				packet.textureSlotMask = (1U << 0);
				packet.textureSlots[0].unit = 0U;
				packet.textureSlots[0].texture = srcAttachment->textureHandle;
				packet.textureSlots[0].target = graphics::TextureTargetParam(static_cast<u32>(srcAttachment->textureTarget));
				packet.vertices.reserve(4U);

				auto addVertex = [&packet](f32 _x, f32 _y, f32 _s, f32 _t) {
					DrawVertex v{};
					v.x = _x;
					v.y = _y;
					v.z = 0.0f;
					v.w = 1.0f;
					v.r = 1.0f;
					v.g = 1.0f;
					v.b = 1.0f;
					v.a = 1.0f;
					v.s0 = _s;
					v.t0 = _t;
					v.s1 = _s;
					v.t1 = _t;
					packet.vertices.push_back(v);
				};

				const f32 dstX0 = static_cast<f32>(_params.dstX0);
				const f32 dstY0 = static_cast<f32>(_params.dstY0);
				const f32 dstX1 = static_cast<f32>(_params.dstX1);
				const f32 dstY1 = static_cast<f32>(_params.dstY1);
				const f32 srcX0 = static_cast<f32>(_params.srcX0);
				const f32 srcY0 = static_cast<f32>(_params.srcY0);
				const f32 srcX1 = static_cast<f32>(_params.srcX1);
				const f32 srcY1 = static_cast<f32>(_params.srcY1);
				addVertex(dstX0, dstY0, srcX0, srcY0);
				addVertex(dstX1, dstY0, srcX1, srcY0);
				addVertex(dstX0, dstY1, srcX0, srcY1);
				addVertex(dstX1, dstY1, srcX1, srcY1);

				m_vk->drawRecorder.applyStateToPacket(packet);
				packet.state.depth.testEnabled = false;
				packet.state.depth.writeEnabled = false;
				packet.state.depth.compare = CompareMode::kAlways;
				packet.state.blend.enabled = false;
				packet.state.cullMode = CullMode::kNone;
				packet.state.raster.scissorEnabled = false;
				packet.dirtyMask |= draw_dirty::kDepth | draw_dirty::kBlend | draw_dirty::kCull | draw_dirty::kScissor;

				normalizePacketTextureCoordinates(m_vk->textureStore, packet);
				normalizePacketPositions(packet);
				m_vk->drawRecorder.pushPacket(std::move(packet));
				colorOk = true;
			}
		}

		if (wantsDepth) {
			DepthBlitStats & stats = depthBlitStats();
			++stats.attempts;
			depthOp = true;
			depthOk = false;
			depthReason = DepthBlitFailureReason::UnsupportedDefaultTarget;
			recordDepthBlitFailure(depthReason);
			ok = false;
		}

		const DepthBlitStats & stats = depthBlitStats();
		vkFboTrace(
			"op=blit_end readFbo=%u drawFbo=%u srcColorAttachment=%s colorOp=%u colorOk=%u depthOp=%u depthOk=%u depthReason=%s result=%u path=enqueue_default depthStats=[attempts=%llu success=%llu fail=%llu]",
			static_cast<u32>(_params.readBuffer),
			static_cast<u32>(_params.drawBuffer),
			bufferAttachmentName(srcColorAttachment),
			colorOp ? 1U : 0U,
			colorOk ? 1U : 0U,
			depthOp ? 1U : 0U,
			depthOk ? 1U : 0U,
			depthBlitFailureReasonName(depthReason),
			ok ? 1U : 0U,
			static_cast<unsigned long long>(stats.attempts),
			static_cast<unsigned long long>(stats.successes),
			static_cast<unsigned long long>(stats.failures));
		return ok;
	}

	bool ok = true;
	bool colorOp = false;
	bool colorOk = false;
	graphics::BufferAttachmentParam srcColorAttachment = graphics::bufferAttachment::COLOR_ATTACHMENT0;
	graphics::BufferAttachmentParam dstColorAttachment = graphics::bufferAttachment::COLOR_ATTACHMENT0;
	const u32 drawColorAttachmentLimit = m_vk->framebufferStore.getDrawBufferCount(_params.drawBuffer);
	if (wantsColor) {
		colorOp = true;
		const FramebufferStore::FramebufferAttachment * srcAttachment = resolveColorAttachment(
			m_vk->framebufferStore,
			_params.readBuffer,
			graphics::bufferAttachment::COLOR_ATTACHMENT0,
			&srcColorAttachment,
			readColorAttachmentLimit);
		const FramebufferStore::FramebufferAttachment * dstAttachment = resolveColorAttachment(
			m_vk->framebufferStore,
			_params.drawBuffer,
			srcColorAttachment,
			&dstColorAttachment,
			drawColorAttachmentLimit);
		if (srcAttachment == nullptr
			|| dstAttachment == nullptr
			|| !srcAttachment->textureHandle.isNotNull()
			|| !dstAttachment->textureHandle.isNotNull()) {
			ok = false;
		} else {
			colorOk = m_vk->textureStore.blitTexture(
				srcAttachment->textureHandle,
				dstAttachment->textureHandle,
				_params.srcX0,
				_params.srcY0,
				_params.srcX1,
				_params.srcY1,
				_params.dstX0,
				_params.dstY0,
				_params.dstX1,
				_params.dstY1,
				graphics::TextureParam(static_cast<u32>(_params.filter)),
				false);
			ok = ok && colorOk;
			if (colorOk && _params.drawBuffer.isNotNull()) {
				m_vk->lastColorBlitDrawFramebuffer = _params.drawBuffer;
				m_vk->lastColorBlitDrawFramebufferSerial = ++m_vk->framebufferBindSerial;
			}
		}
	}

	bool depthOp = false;
	bool depthOk = false;
	DepthBlitFailureReason depthReason = DepthBlitFailureReason::kNone;
	if (wantsDepth) {
		depthOp = true;
		DepthBlitStats & stats = depthBlitStats();
		++stats.attempts;
		const FramebufferStore::FramebufferAttachment * srcAttachment = m_vk->framebufferStore.getAttachment(
			_params.readBuffer,
			graphics::bufferAttachment::DEPTH_ATTACHMENT);
		const FramebufferStore::FramebufferAttachment * dstAttachment = m_vk->framebufferStore.getAttachment(
			_params.drawBuffer,
			graphics::bufferAttachment::DEPTH_ATTACHMENT);
		graphics::ObjectHandle srcDepthTextureHandle = graphics::ObjectHandle::null;
		graphics::ObjectHandle dstDepthTextureHandle = graphics::ObjectHandle::null;
		s32 depthSrcX0 = _params.srcX0;
		s32 depthSrcY0 = _params.srcY0;
		s32 depthSrcX1 = _params.srcX1;
		s32 depthSrcY1 = _params.srcY1;
		s32 depthDstX0 = _params.dstX0;
		s32 depthDstY0 = _params.dstY0;
		s32 depthDstX1 = _params.dstX1;
		s32 depthDstY1 = _params.dstY1;
		if (srcAttachment == nullptr || dstAttachment == nullptr) {
			depthReason = DepthBlitFailureReason::MissingAttachment;
		} else if (!srcAttachment->textureHandle.isNotNull() || !dstAttachment->textureHandle.isNotNull()) {
			depthReason = DepthBlitFailureReason::MissingTextureHandle;
			srcDepthTextureHandle = srcAttachment->textureHandle;
			dstDepthTextureHandle = dstAttachment->textureHandle;
		} else {
			srcDepthTextureHandle = srcAttachment->textureHandle;
			dstDepthTextureHandle = dstAttachment->textureHandle;
			const TextureStore::TextureResource * srcTexture = m_vk->textureStore.getTexture(srcDepthTextureHandle);
			const TextureStore::TextureResource * dstTexture = m_vk->textureStore.getTexture(dstDepthTextureHandle);
			if (srcTexture == nullptr
				|| dstTexture == nullptr
				|| srcTexture->image == VK_NULL_HANDLE
				|| dstTexture->image == VK_NULL_HANDLE
				|| srcTexture->width == 0U
				|| srcTexture->height == 0U
				|| dstTexture->width == 0U
				|| dstTexture->height == 0U
				|| srcTexture->vkFormat == VK_FORMAT_UNDEFINED
				|| dstTexture->vkFormat == VK_FORMAT_UNDEFINED) {
				depthReason = DepthBlitFailureReason::InvalidTextureResource;
			} else if (!hasDepthComponent(srcTexture->vkFormat) || !hasDepthComponent(dstTexture->vkFormat)) {
				depthReason = DepthBlitFailureReason::FormatMismatch;
			} else {
				auto clampToDimension = [](s32 _coord, u32 _dimension) -> s32 {
					const s32 maxCoord = static_cast<s32>(_dimension);
					if (_coord < 0)
						return 0;
					if (_coord > maxCoord)
						return maxCoord;
					return _coord;
				};
				const s32 srcX0 = clampToDimension(_params.srcX0, srcTexture->width);
				const s32 srcY0 = clampToDimension(_params.srcY0, srcTexture->height);
				const s32 srcX1 = clampToDimension(_params.srcX1, srcTexture->width);
				const s32 srcY1 = clampToDimension(_params.srcY1, srcTexture->height);
				const s32 dstX0 = clampToDimension(_params.dstX0, dstTexture->width);
				const s32 dstY0 = clampToDimension(_params.dstY0, dstTexture->height);
				const s32 dstX1 = clampToDimension(_params.dstX1, dstTexture->width);
				const s32 dstY1 = clampToDimension(_params.dstY1, dstTexture->height);
				const s32 srcMinX = std::min(srcX0, srcX1);
				const s32 srcMinY = std::min(srcY0, srcY1);
				const s32 srcWidth = std::max(srcX0, srcX1) - srcMinX;
				const s32 srcHeight = std::max(srcY0, srcY1) - srcMinY;
				const s32 dstMinX = std::min(dstX0, dstX1);
				const s32 dstMinY = std::min(dstY0, dstY1);
				const s32 dstWidth = std::max(dstX0, dstX1) - dstMinX;
				const s32 dstHeight = std::max(dstY0, dstY1) - dstMinY;
				const s32 copyWidth = std::min(srcWidth, dstWidth);
				const s32 copyHeight = std::min(srcHeight, dstHeight);
				if (copyWidth <= 0 || copyHeight <= 0) {
					depthReason = DepthBlitFailureReason::EmptyRegion;
				} else {
					depthSrcX0 = srcMinX;
					depthSrcY0 = srcMinY;
					depthSrcX1 = srcMinX + copyWidth;
					depthSrcY1 = srcMinY + copyHeight;
					depthDstX0 = dstMinX;
					depthDstY0 = dstMinY;
					depthDstX1 = dstMinX + copyWidth;
					depthDstY1 = dstMinY + copyHeight;
					depthOk = m_vk->textureStore.blitTexture(
						srcDepthTextureHandle,
						dstDepthTextureHandle,
						depthSrcX0,
						depthSrcY0,
						depthSrcX1,
						depthSrcY1,
						depthDstX0,
						depthDstY0,
						depthDstX1,
						depthDstY1,
						graphics::TextureParam(static_cast<u32>(graphics::textureParameters::FILTER_NEAREST)),
						true);
					if (!depthOk)
						depthReason = DepthBlitFailureReason::BackendFailure;
				}
			}
		}
		if (depthOk) {
			++stats.successes;
		} else {
			if (depthReason == DepthBlitFailureReason::kNone)
				depthReason = DepthBlitFailureReason::BackendFailure;
			recordDepthBlitFailure(depthReason);
			vkFboTrace(
				"op=blit_depth_fail readFbo=%u drawFbo=%u srcDepthTexture=%u dstDepthTexture=%u reason=%s src=[%d,%d,%d,%d] dst=[%d,%d,%d,%d]",
				static_cast<u32>(_params.readBuffer),
				static_cast<u32>(_params.drawBuffer),
				static_cast<u32>(srcDepthTextureHandle),
				static_cast<u32>(dstDepthTextureHandle),
				depthBlitFailureReasonName(depthReason),
				depthSrcX0,
				depthSrcY0,
				depthSrcX1,
				depthSrcY1,
				depthDstX0,
				depthDstY0,
				depthDstX1,
				depthDstY1);
			ok = false;
		}
		ok = ok && depthOk;
	}

	const DepthBlitStats & depthStats = depthBlitStats();
	vkFboTrace(
		"op=blit_end readFbo=%u drawFbo=%u srcColorAttachment=%s dstColorAttachment=%s colorOp=%u colorOk=%u depthOp=%u depthOk=%u depthReason=%s result=%u depthStats=[attempts=%llu success=%llu fail=%llu]",
		static_cast<u32>(_params.readBuffer),
		static_cast<u32>(_params.drawBuffer),
		bufferAttachmentName(srcColorAttachment),
		bufferAttachmentName(dstColorAttachment),
		colorOp ? 1U : 0U,
		colorOk ? 1U : 0U,
		depthOp ? 1U : 0U,
		depthOk ? 1U : 0U,
		depthBlitFailureReasonName(depthReason),
		ok ? 1U : 0U,
		static_cast<unsigned long long>(depthStats.attempts),
		static_cast<unsigned long long>(depthStats.successes),
		static_cast<unsigned long long>(depthStats.failures));
	return ok;
#endif
}

void ContextImpl::setDrawBuffers(u32 _num)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	const graphics::ObjectHandle drawFramebuffer = m_vk->drawRecorder.drawFramebufferBinding();
	m_vk->framebufferStore.setDrawBuffers(drawFramebuffer, _num);
	vkFboTrace(
		"op=set_draw_buffers drawFbo=%u count=%u",
		static_cast<u32>(drawFramebuffer),
		std::max<u32>(1U, _num));
#else
	(void)_num;
#endif
}

bool ContextImpl::readScreen2(void * _dest, int _width, int _height, int _front)
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	(void)_dest;
	(void)_width;
	(void)_height;
	(void)_front;
	return false;
#else
	(void)_front;
	static const bool debugReadScreen = std::getenv("REALITYVK_VK_DEBUG_READSCREEN") != nullptr;
	if (_dest == nullptr || _width <= 0 || _height <= 0)
		return false;
	if (!m_coreReady || !m_vk)
		return false;
	if (m_vk->device == VK_NULL_HANDLE
		|| m_vk->commandPool == VK_NULL_HANDLE
		|| m_vk->graphicsQueue == VK_NULL_HANDLE
		|| m_vk->swapchain == VK_NULL_HANDLE
		|| m_vk->swapchainImages.empty()) {
		return false;
	}
	if (m_vk->swapchainExtent.width == 0 || m_vk->swapchainExtent.height == 0)
		return false;

	const u32 requestedWidth = static_cast<u32>(_width);
	const u32 requestedHeight = static_cast<u32>(_height);
	const u32 copyWidth = std::min<u32>(requestedWidth, m_vk->swapchainExtent.width);
	const u32 copyHeight = std::min<u32>(requestedHeight, m_vk->swapchainExtent.height);
	if (copyWidth == 0 || copyHeight == 0)
		return false;

	const size_t dstRowBytes = static_cast<size_t>(requestedWidth) * 3U;
	const size_t dstBytes = dstRowBytes * static_cast<size_t>(requestedHeight);
	std::memset(_dest, 0, dstBytes);

	u32 imageIndex = m_vk->lastPresentedImageIndex;
	if (imageIndex >= m_vk->swapchainImages.size())
		imageIndex = 0U;
	const VkImage sourceImage = m_vk->swapchainImages[imageIndex];
	if (sourceImage == VK_NULL_HANDLE)
		return false;

	if (vkDeviceWaitIdle(m_vk->device) != VK_SUCCESS)
		return false;

	const VkDeviceSize stagingSize = static_cast<VkDeviceSize>(copyWidth) * static_cast<VkDeviceSize>(copyHeight) * 4U;
	VkBuffer stagingBuffer = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkFence submitFence = VK_NULL_HANDLE;
	void * mapped = nullptr;
	bool success = false;

	do {
		VkBufferCreateInfo bufferCreateInfo{};
		bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferCreateInfo.size = stagingSize;
		bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(m_vk->device, &bufferCreateInfo, nullptr, &stagingBuffer) != VK_SUCCESS) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkCreateBuffer failed.");
			break;
		}

		VkMemoryRequirements memoryRequirements{};
		vkGetBufferMemoryRequirements(m_vk->device, stagingBuffer, &memoryRequirements);
		const u32 memoryTypeIndex = findMemoryTypeIndex(
			m_vk->physicalDevice,
			memoryRequirements.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (memoryTypeIndex == UINT32_MAX) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: no host-visible memory type for staging buffer.");
			break;
		}

		VkMemoryAllocateInfo memoryAllocateInfo{};
		memoryAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		memoryAllocateInfo.allocationSize = memoryRequirements.size;
		memoryAllocateInfo.memoryTypeIndex = memoryTypeIndex;
		if (vkAllocateMemory(m_vk->device, &memoryAllocateInfo, nullptr, &stagingMemory) != VK_SUCCESS) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkAllocateMemory failed.");
			break;
		}
		if (vkBindBufferMemory(m_vk->device, stagingBuffer, stagingMemory, 0) != VK_SUCCESS) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkBindBufferMemory failed.");
			break;
		}

		VkCommandBufferAllocateInfo commandBufferAllocateInfo{};
		commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		commandBufferAllocateInfo.commandPool = m_vk->commandPool;
		commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commandBufferAllocateInfo.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(m_vk->device, &commandBufferAllocateInfo, &commandBuffer) != VK_SUCCESS) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkAllocateCommandBuffers failed.");
			break;
		}

		VkCommandBufferBeginInfo commandBufferBeginInfo{};
		commandBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		commandBufferBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (vkBeginCommandBuffer(commandBuffer, &commandBufferBeginInfo) != VK_SUCCESS) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkBeginCommandBuffer failed.");
			break;
		}

		const VkImageSubresourceRange imageSubresourceRange = {
			VK_IMAGE_ASPECT_COLOR_BIT,
			0, 1,
			0, 1
		};

		VkImageMemoryBarrier toTransferBarrier{};
		toTransferBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		toTransferBarrier.srcAccessMask = 0;
		toTransferBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		toTransferBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		toTransferBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		toTransferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toTransferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toTransferBarrier.image = sourceImage;
		toTransferBarrier.subresourceRange = imageSubresourceRange;
		vkCmdPipelineBarrier(
			commandBuffer,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0,
			0, nullptr,
			0, nullptr,
			1, &toTransferBarrier);

		VkBufferImageCopy copyRegion{};
		copyRegion.bufferOffset = 0;
		copyRegion.bufferRowLength = 0;
		copyRegion.bufferImageHeight = 0;
		copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.imageSubresource.mipLevel = 0;
		copyRegion.imageSubresource.baseArrayLayer = 0;
		copyRegion.imageSubresource.layerCount = 1;
		copyRegion.imageOffset = { 0, 0, 0 };
		copyRegion.imageExtent = { copyWidth, copyHeight, 1 };
		vkCmdCopyImageToBuffer(
			commandBuffer,
			sourceImage,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			stagingBuffer,
			1,
			&copyRegion);

		VkImageMemoryBarrier toPresentBarrier{};
		toPresentBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		toPresentBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		toPresentBarrier.dstAccessMask = 0;
		toPresentBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		toPresentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		toPresentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toPresentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toPresentBarrier.image = sourceImage;
		toPresentBarrier.subresourceRange = imageSubresourceRange;
		vkCmdPipelineBarrier(
			commandBuffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			0,
			0, nullptr,
			0, nullptr,
			1, &toPresentBarrier);

		if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkEndCommandBuffer failed.");
			break;
		}

		VkFenceCreateInfo fenceCreateInfo{};
		fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		if (vkCreateFence(m_vk->device, &fenceCreateInfo, nullptr, &submitFence) != VK_SUCCESS) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkCreateFence failed.");
			break;
		}

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;
		if (vkQueueSubmit(m_vk->graphicsQueue, 1, &submitInfo, submitFence) != VK_SUCCESS) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkQueueSubmit failed.");
			break;
		}
		if (vkWaitForFences(m_vk->device, 1, &submitFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkWaitForFences failed.");
			break;
		}

		if (vkMapMemory(m_vk->device, stagingMemory, 0, stagingSize, 0, &mapped) != VK_SUCCESS || mapped == nullptr) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkMapMemory failed.");
			break;
		}

		const bool bgraSource = m_vk->swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM
			|| m_vk->swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB;
		const u8 * srcPixels = static_cast<const u8 *>(mapped);
		u8 * dstPixels = static_cast<u8 *>(_dest);
		const size_t srcRowBytes = static_cast<size_t>(copyWidth) * 4U;
		for (u32 y = 0; y < copyHeight; ++y) {
			const u8 * srcRow = srcPixels + static_cast<size_t>(copyHeight - 1U - y) * srcRowBytes;
			u8 * dstRow = dstPixels + static_cast<size_t>(y) * dstRowBytes;
			for (u32 x = 0; x < copyWidth; ++x) {
				const u8 c0 = srcRow[x * 4U + 0U];
				const u8 c1 = srcRow[x * 4U + 1U];
				const u8 c2 = srcRow[x * 4U + 2U];
				if (bgraSource) {
					dstRow[x * 3U + 0U] = c2;
					dstRow[x * 3U + 1U] = c1;
					dstRow[x * 3U + 2U] = c0;
				} else {
					dstRow[x * 3U + 0U] = c0;
					dstRow[x * 3U + 1U] = c1;
					dstRow[x * 3U + 2U] = c2;
				}
			}
		}

		success = true;
	} while (false);

	if (debugReadScreen) {
		LOG(
			LOG_WARNING,
			"VK readScreen2: success=%u copy=%ux%u requested=%dx%d imageIndex=%u format=%d",
			success ? 1U : 0U,
			copyWidth,
			copyHeight,
			_width,
			_height,
			imageIndex,
			static_cast<int>(m_vk->swapchainFormat));
	}

	if (mapped != nullptr)
		vkUnmapMemory(m_vk->device, stagingMemory);
	if (submitFence != VK_NULL_HANDLE)
		vkDestroyFence(m_vk->device, submitFence, nullptr);
	if (commandBuffer != VK_NULL_HANDLE)
		vkFreeCommandBuffers(m_vk->device, m_vk->commandPool, 1, &commandBuffer);
	if (stagingMemory != VK_NULL_HANDLE)
		vkFreeMemory(m_vk->device, stagingMemory, nullptr);
	if (stagingBuffer != VK_NULL_HANDLE)
		vkDestroyBuffer(m_vk->device, stagingBuffer, nullptr);

	return success;
#endif
}

graphics::PixelReadBuffer * ContextImpl::createPixelReadBuffer(size_t _sizeInBytes)
{
	return new VulkanPixelReadBuffer(
		_sizeInBytes,
		[this](
			s32 _x,
			s32 _y,
			u32 _width,
			u32 _height,
			graphics::Parameter _format,
			graphics::Parameter _type,
			std::vector<u8> & _outData) -> bool {
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
			(void)_x;
			(void)_y;
			(void)_width;
			(void)_height;
			(void)_format;
			(void)_type;
			(void)_outData;
			return false;
#else
			(void)_type;
			if (!m_vk || _width == 0U || _height == 0U)
				return false;

			const s32 clampedX = std::max<s32>(0, _x);
			const s32 clampedY = std::max<s32>(0, _y);
			if (clampedX >= std::numeric_limits<s32>::max() || clampedY >= std::numeric_limits<s32>::max())
				return false;
			const u32 readX = static_cast<u32>(clampedX);
			const u32 readY = static_cast<u32>(clampedY);

			graphics::ObjectHandle readFramebuffer = m_vk->drawRecorder.readFramebufferBinding();
			if (!readFramebuffer.isNotNull())
				readFramebuffer = m_vk->lastNonDefaultReadFramebuffer;
			if (!readFramebuffer.isNotNull())
				readFramebuffer = m_vk->lastColorBlitDrawFramebuffer;
			if (!readFramebuffer.isNotNull())
				readFramebuffer = m_vk->lastNonDefaultDrawFramebuffer;
			if (!readFramebuffer.isNotNull())
				return false;

			const bool depthRead = _format == graphics::colorFormat::DEPTH;
			const u32 readColorAttachmentLimit = m_vk->framebufferStore.getDrawBufferCount(readFramebuffer);
			graphics::BufferAttachmentParam sourceAttachmentType = depthRead
				? graphics::bufferAttachment::DEPTH_ATTACHMENT
				: graphics::bufferAttachment::COLOR_ATTACHMENT0;
			const FramebufferStore::FramebufferAttachment * sourceAttachment = depthRead
				? m_vk->framebufferStore.getAttachment(readFramebuffer, graphics::bufferAttachment::DEPTH_ATTACHMENT)
				: resolveColorAttachment(
					m_vk->framebufferStore,
					readFramebuffer,
					graphics::bufferAttachment::COLOR_ATTACHMENT0,
					&sourceAttachmentType,
					readColorAttachmentLimit);
			if (sourceAttachment == nullptr || !sourceAttachment->textureHandle.isNotNull())
				return false;

			u32 rowBytes = 0U;
			u32 bytesPerPixel = 0U;
			if (!m_vk->textureStore.readTexture(
				sourceAttachment->textureHandle,
				readX,
				readY,
				_width,
				_height,
				_outData,
				rowBytes,
				bytesPerPixel)) {
				return false;
			}
			if (rowBytes == 0U || bytesPerPixel == 0U || (rowBytes % bytesPerPixel) != 0U)
				return false;
			static const bool debugReadback = std::getenv("REALITYVK_VK_DEBUG_READBACK") != nullptr;
			if (debugReadback) {
				static u32 readbackLogCount = 0U;
				static const u32 readbackLogLimit = []() -> u32 {
					const char * env = std::getenv("REALITYVK_VK_DEBUG_READBACK_LIMIT");
					if (env == nullptr || env[0] == '\0')
						return 64U;
					return static_cast<u32>(std::strtoul(env, nullptr, 10));
				}();
				if (readbackLogCount < readbackLogLimit) {
					LOG(
						LOG_WARNING,
						"VK readback debug: kind=pixel fbo=%u attachment=%s texture=%u x=%u y=%u w=%u h=%u rowBytes=%u bpp=%u bytes=%u",
						static_cast<u32>(readFramebuffer),
						bufferAttachmentName(sourceAttachmentType),
						static_cast<u32>(sourceAttachment->textureHandle),
						readX,
						readY,
						_width,
						_height,
						rowBytes,
						bytesPerPixel,
						static_cast<u32>(_outData.size()));
					++readbackLogCount;
				}
			}
			return true;
#endif
		});
}

graphics::ColorBufferReader * ContextImpl::createColorBufferReader(CachedTexture * _pTexture)
{
	if (_pTexture == nullptr)
		return nullptr;
	return new VulkanColorBufferReader(
		_pTexture,
		[this](
			graphics::ObjectHandle _textureHandle,
			s32 _x,
			s32 _y,
			u32 _width,
			u32 _height,
			graphics::Parameter _format,
			graphics::Parameter _type,
			std::vector<u8> & _outData,
			u32 & _outStridePixels) -> bool {
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
			(void)_textureHandle;
			(void)_x;
			(void)_y;
			(void)_width;
			(void)_height;
			(void)_format;
			(void)_type;
			(void)_outData;
			_outStridePixels = 0U;
			return false;
#else
			(void)_format;
			(void)_type;
			_outStridePixels = 0U;
			if (!m_vk || !_textureHandle.isNotNull() || _width == 0U || _height == 0U)
				return false;
			const s32 clampedX = std::max<s32>(0, _x);
			const s32 clampedY = std::max<s32>(0, _y);
			const u32 readX = static_cast<u32>(clampedX);
			const u32 readY = static_cast<u32>(clampedY);

			u32 rowBytes = 0U;
			u32 bytesPerPixel = 0U;
			if (!m_vk->textureStore.readTexture(
				_textureHandle,
				readX,
				readY,
				_width,
				_height,
				_outData,
				rowBytes,
				bytesPerPixel)) {
				return false;
			}
			if (rowBytes == 0U || bytesPerPixel == 0U || (rowBytes % bytesPerPixel) != 0U)
				return false;
			static const bool debugReadback = std::getenv("REALITYVK_VK_DEBUG_READBACK") != nullptr;
			if (debugReadback) {
				static u32 readbackLogCount = 0U;
				static const u32 readbackLogLimit = []() -> u32 {
					const char * env = std::getenv("REALITYVK_VK_DEBUG_READBACK_LIMIT");
					if (env == nullptr || env[0] == '\0')
						return 64U;
					return static_cast<u32>(std::strtoul(env, nullptr, 10));
				}();
				if (readbackLogCount < readbackLogLimit) {
					LOG(
						LOG_WARNING,
						"VK readback debug: kind=color texture=%u x=%u y=%u w=%u h=%u rowBytes=%u bpp=%u bytes=%u",
						static_cast<u32>(_textureHandle),
						readX,
						readY,
						_width,
						_height,
						rowBytes,
						bytesPerPixel,
						static_cast<u32>(_outData.size()));
					++readbackLogCount;
				}
			}
			_outStridePixels = rowBytes / bytesPerPixel;
			return _outStridePixels != 0U;
#endif
		});
}

graphics::CombinerProgram * ContextImpl::createCombinerProgram(Combiner & _color, Combiner & _alpha, const CombinerKey & _key)
{
	(void)_color;
	(void)_alpha;
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (m_vk) {
		return m_vk->programLibrary.createCombinerProgram(
			_key,
			[](const CombinerKey & _programKey) -> graphics::CombinerProgram * {
				return new VulkanInferredCombinerProgram(_programKey);
			});
	}
#endif
	return new VulkanInferredCombinerProgram(_key);
}

bool ContextImpl::saveShadersStorage(const graphics::Combiners & _combiners)
{
	return ShaderKeyStorage::save(_combiners);
}

bool ContextImpl::loadShadersStorage(graphics::Combiners & _combiners)
{
	return ShaderKeyStorage::load(_combiners, [](const CombinerKey & _key) -> graphics::CombinerProgram * {
		return new VulkanInferredCombinerProgram(_key);
	});
}

graphics::ShaderProgram * ContextImpl::createDepthFogShader()
{
	return new VulkanDepthFogShaderProgram;
}

graphics::TexrectDrawerShaderProgram * ContextImpl::createTexrectDrawerDrawShader()
{
	return new VulkanTexrectDrawerShaderProgram;
}

graphics::ShaderProgram * ContextImpl::createTexrectDrawerClearShader()
{
	return new VulkanTexrectDrawerClearShaderProgram;
}

graphics::ShaderProgram * ContextImpl::createTexrectUpscaleCopyShader()
{
	return new VulkanSpecialShaderProgram(true, true, false, false);
}

graphics::ShaderProgram * ContextImpl::createTexrectColorAndDepthUpscaleCopyShader()
{
	return new VulkanTexrectColorDepthCopyShaderProgram;
}

graphics::ShaderProgram * ContextImpl::createTexrectDownscaleCopyShader()
{
	return new VulkanSpecialShaderProgram(true, true, false, false);
}

graphics::ShaderProgram * ContextImpl::createTexrectColorAndDepthDownscaleCopyShader()
{
	return new VulkanTexrectColorDepthCopyShaderProgram;
}

graphics::ShaderProgram * ContextImpl::createGammaCorrectionShader()
{
	return new VulkanGammaCorrectionShaderProgram;
}

graphics::ShaderProgram * ContextImpl::createFXAAShader()
{
	return new VulkanFXAAShaderProgram;
}

graphics::TextDrawerShaderProgram * ContextImpl::createTextDrawerShader()
{
	return new VulkanTextDrawerShaderProgram;
}

void ContextImpl::resetShaderProgram()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.markAllStateDirty();
#endif
}

void ContextImpl::drawTriangles(const graphics::Context::DrawTriangleParameters & _params)
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	(void)_params;
	return;
#else
	if (!m_vk)
		return;
	if (_params.vertices == nullptr || _params.verticesCount == 0)
		return;

	PrimitiveType primitive = PrimitiveType::Triangles;
	if (!resolvePrimitiveType(_params.mode, primitive))
		return;

	DrawPacket packet{};
	packet.primitive = primitive;
	packet.transformMode = VertexTransformMode::Triangle;
	packet.textureUnit0 = 0U;
	packet.textureUnit1 = 1U;
	packet.shaderFlags = resolveShaderFlags(_params.combiner, true, true);
	packet.fogColorR = gDP.fogColor.r;
	packet.fogColorG = gDP.fogColor.g;
	packet.fogColorB = gDP.fogColor.b;
	packet.fogColorA = gDP.fogColor.a;
	applyStrictBlendMuxPacketState(false, packet);
	assignPacketDebugMetadata(packet, kPacketSourceTriangles, _params.combiner, false);
		if (_params.elements != nullptr && _params.elementsCount > 0) {
			packet.vertices.reserve(_params.elementsCount);
			auto appendIndexedVertex = [&](u32 _vertexIndex) {
				if (_vertexIndex >= _params.verticesCount)
					return;
				appendTriangleVertex(_params.vertices[_vertexIndex], _params.flatColors, packet);
			};

			if (_params.elementsType == graphics::datatype::UNSIGNED_SHORT) {
				const u16 * elements = reinterpret_cast<const u16 *>(_params.elements);
				for (u32 i = 0; i < _params.elementsCount; ++i)
					appendIndexedVertex(static_cast<u32>(elements[i]));
			} else if (_params.elementsType == graphics::datatype::UNSIGNED_BYTE) {
				const u8 * elements = reinterpret_cast<const u8 *>(_params.elements);
				for (u32 i = 0; i < _params.elementsCount; ++i)
					appendIndexedVertex(static_cast<u32>(elements[i]));
			} else if (_params.elementsType == graphics::datatype::UNSIGNED_INT) {
				const u32 * elements = reinterpret_cast<const u32 *>(_params.elements);
				for (u32 i = 0; i < _params.elementsCount; ++i)
					appendIndexedVertex(elements[i]);
			} else {
				static bool warnedUnsupportedIndexType = false;
				if (!warnedUnsupportedIndexType) {
					LOG(
						LOG_WARNING,
						"Vulkan draw path does not support index type 0x%x; draw call skipped.",
						static_cast<u32>(_params.elementsType));
					warnedUnsupportedIndexType = true;
				}
				return;
			}
		} else {
			packet.vertices.reserve(_params.verticesCount);
			for (u32 i = 0; i < _params.verticesCount; ++i)
				appendTriangleVertex(_params.vertices[i], _params.flatColors, packet);
		}

	if (packet.vertices.empty())
		return;
	applyCombinerSolidColorOverride(_params.combiner, packet);
	attachPacketBindings(m_vk->bindingState, packet);
	normalizePacketTextureCoordinates(m_vk->textureStore, packet);
	if (!isDefaultDrawFramebufferBound()) {
		m_vk->drawRecorder.applyStateToPacket(packet);
		if (!executeOffscreenDrawPacket(packet, m_vk->drawRecorder.drawFramebufferBinding())) {
			offscreenSkippedDrawCalls() += 1U;
			offscreenSkippedVertices() += static_cast<u64>(packet.vertices.size());
			vkFboTrace(
				"op=draw_skip primitive=triangles drawFbo=%u readFbo=%u vertices=%u reason=offscreen_execute_failed",
				static_cast<u32>(m_vk->drawRecorder.drawFramebufferBinding()),
				static_cast<u32>(m_vk->drawRecorder.readFramebufferBinding()),
				static_cast<u32>(packet.vertices.size()));
		}
		return;
	}
	if (isVkFboTraceVerboseEnabled()) {
		vkFboTrace(
			"op=draw_enqueue primitive=triangles drawFbo=%u vertices=%u textureMask=0x%x shaderFlags=0x%x",
			static_cast<u32>(m_vk->drawRecorder.drawFramebufferBinding()),
			static_cast<u32>(packet.vertices.size()),
			packet.textureSlotMask,
			packet.shaderFlags);
	}
	normalizePacketPositions(packet);
	m_vk->drawRecorder.pushPacket(std::move(packet));
#endif
}

void ContextImpl::drawRects(const graphics::Context::DrawRectParameters & _params)
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	(void)_params;
	return;
#else
	if (!m_vk)
		return;
	if (_params.vertices == nullptr || _params.verticesCount == 0)
		return;

	PrimitiveType primitive = PrimitiveType::Triangles;
	if (!resolvePrimitiveType(_params.mode, primitive))
		return;

	DrawPacket packet{};
	packet.primitive = primitive;
	packet.transformMode = VertexTransformMode::Rect;
	packet.textureUnit0 = 0U;
	packet.textureUnit1 = 1U;
	packet.shaderFlags = resolveShaderFlags(_params.combiner, false, _params.texrect);
	packet.texrectAlphaTest = 0U;
	packet.texrectFilterMode = 0U;
	packet.blendMux1Packed = 0U;
	packet.blendMux2Packed = 0U;
	packet.blendParamsPacked = 0U;
	packet.texrectTextureWidth = 1.0f;
	packet.texrectTextureHeight = 1.0f;
	packet.gammaLevel = 2.0f;
	packet.textColorR = 1.0f;
	packet.textColorG = 1.0f;
	packet.textColorB = 1.0f;
	packet.textColorA = 1.0f;
	packet.fogColorR = gDP.fogColor.r;
	packet.fogColorG = gDP.fogColor.g;
	packet.fogColorB = gDP.fogColor.b;
	packet.fogColorA = gDP.fogColor.a;
	applyStrictBlendMuxPacketState(_params.texrect, packet);
	assignPacketDebugMetadata(packet, kPacketSourceRects, _params.combiner, _params.texrect);
	const bool isClearRectPass = isVulkanTexrectDrawerClearProgram(_params.combiner);
	const auto * texrectDrawProgram = dynamic_cast<const VulkanTexrectDrawerShaderProgram *>(_params.combiner);
	static const bool disableSpecialTexrect = std::getenv("REALITYVK_VK_DISABLE_SPECIAL_TEXRECT") != nullptr;
	if (texrectDrawProgram != nullptr && !disableSpecialTexrect) {
		packet.shaderFlags |= vulkan::draw_shader_flags::kSpecialTexrectDraw;
		packet.texrectAlphaTest = texrectDrawProgram->alphaTestEnabled();
		packet.texrectFilterMode = texrectDrawProgram->filterMode();
		packet.texrectTextureWidth = static_cast<f32>(std::max<u32>(1U, texrectDrawProgram->textureWidth()));
		packet.texrectTextureHeight = static_cast<f32>(std::max<u32>(1U, texrectDrawProgram->textureHeight()));
	}
	if (dynamic_cast<const VulkanTexrectColorDepthCopyShaderProgram *>(_params.combiner) != nullptr)
		packet.shaderFlags |= vulkan::draw_shader_flags::kSpecialDepthFromTexture1;
	static const bool disableSpecialGamma = std::getenv("REALITYVK_VK_DISABLE_SPECIAL_GAMMA") != nullptr;
	static const bool disableSpecialFXAA = std::getenv("REALITYVK_VK_DISABLE_SPECIAL_FXAA") != nullptr;
	const auto * gammaProgram = dynamic_cast<const VulkanGammaCorrectionShaderProgram *>(_params.combiner);
	if (gammaProgram != nullptr && !disableSpecialGamma) {
		packet.shaderFlags |= vulkan::draw_shader_flags::kSpecialGammaCorrection;
		packet.gammaLevel = std::max(0.001f, gammaProgram->gammaLevel());
	}
	if (!disableSpecialFXAA && dynamic_cast<const VulkanFXAAShaderProgram *>(_params.combiner) != nullptr)
		packet.shaderFlags |= vulkan::draw_shader_flags::kSpecialFXAA;
	const auto * depthFogProgram = dynamic_cast<const VulkanDepthFogShaderProgram *>(_params.combiner);
	if (depthFogProgram != nullptr) {
		packet.shaderFlags |= vulkan::draw_shader_flags::kSpecialDepthFog;
		packet.fogColorR = gDP.fogColor.r;
		packet.fogColorG = gDP.fogColor.g;
		packet.fogColorB = gDP.fogColor.b;
		packet.fogColorA = gDP.fogColor.a;
	}
	const auto * textProgram = dynamic_cast<const VulkanTextDrawerShaderProgram *>(_params.combiner);
	if (textProgram != nullptr) {
		packet.shaderFlags |= vulkan::draw_shader_flags::kSpecialTextDraw;
		const f32 * color = textProgram->textColor();
		packet.textColorR = color[0];
		packet.textColorG = color[1];
		packet.textColorB = color[2];
		packet.textColorA = color[3];
	}
	if ((packet.shaderFlags & (vulkan::draw_shader_flags::kSpecialTexrectDraw
		| vulkan::draw_shader_flags::kSpecialDepthFromTexture1
		| vulkan::draw_shader_flags::kSpecialGammaCorrection
		| vulkan::draw_shader_flags::kSpecialFXAA
		| vulkan::draw_shader_flags::kSpecialDepthFog
		| vulkan::draw_shader_flags::kSpecialTextDraw)) != 0U) {
		packet.shaderFlags &= ~vulkan::draw_shader_flags::kSpecialStrictBlendMux;
		packet.blendMux1Packed = 0U;
		packet.blendMux2Packed = 0U;
		packet.blendParamsPacked = 0U;
	}
	packet.vertices.reserve(_params.verticesCount);
	for (u32 i = 0; i < _params.verticesCount; ++i)
		appendRectVertex(_params.vertices[i], packet);
	if (isClearRectPass) {
		for (DrawVertex & vertex : packet.vertices) {
			vertex.r = 0.0f;
			vertex.g = 0.0f;
			vertex.b = 0.0f;
			vertex.a = 0.0f;
		}
	}

	if (packet.vertices.empty())
		return;
	applyCombinerSolidColorOverride(_params.combiner, packet);
	attachPacketBindings(m_vk->bindingState, packet);
	normalizePacketTextureCoordinates(m_vk->textureStore, packet);
	if (!isDefaultDrawFramebufferBound()) {
		m_vk->drawRecorder.applyStateToPacket(packet);
		if (!executeOffscreenDrawPacket(packet, m_vk->drawRecorder.drawFramebufferBinding())) {
			offscreenSkippedDrawCalls() += 1U;
			offscreenSkippedVertices() += static_cast<u64>(packet.vertices.size());
			vkFboTrace(
				"op=draw_skip primitive=rects drawFbo=%u readFbo=%u vertices=%u reason=offscreen_execute_failed",
				static_cast<u32>(m_vk->drawRecorder.drawFramebufferBinding()),
				static_cast<u32>(m_vk->drawRecorder.readFramebufferBinding()),
				static_cast<u32>(packet.vertices.size()));
		}
		return;
	}
	if (isVkFboTraceVerboseEnabled()) {
		vkFboTrace(
			"op=draw_enqueue primitive=rects drawFbo=%u vertices=%u textureMask=0x%x shaderFlags=0x%x",
			static_cast<u32>(m_vk->drawRecorder.drawFramebufferBinding()),
			static_cast<u32>(packet.vertices.size()),
			packet.textureSlotMask,
			packet.shaderFlags);
	}
	normalizePacketPositions(packet);
	m_vk->drawRecorder.pushPacket(std::move(packet));
#endif
}

void ContextImpl::drawLine(f32 _width, SPVertex * _vertices)
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	(void)_width;
	(void)_vertices;
	return;
#else
	if (!m_vk)
		return;
	if (_vertices == nullptr)
		return;

	DrawPacket packet{};
	packet.primitive = PrimitiveType::Lines;
	packet.transformMode = VertexTransformMode::Triangle;
	packet.lineWidth = std::max(1.0f, _width);
	packet.shaderFlags = vulkan::draw_shader_flags::kShade;
	assignPacketDebugMetadata(packet, kPacketSourceLines, nullptr, false);
	packet.vertices.reserve(2);
	for (u32 i = 0; i < 2; ++i)
		appendLineVertex(_vertices[i], packet);

	attachPacketBindings(m_vk->bindingState, packet);
	normalizePacketTextureCoordinates(m_vk->textureStore, packet);
	if (!isDefaultDrawFramebufferBound()) {
		m_vk->drawRecorder.applyStateToPacket(packet, draw_dirty::kLineWidth);
		if (!executeOffscreenDrawPacket(packet, m_vk->drawRecorder.drawFramebufferBinding())) {
			offscreenSkippedDrawCalls() += 1U;
			offscreenSkippedVertices() += static_cast<u64>(packet.vertices.size());
			vkFboTrace(
				"op=draw_skip primitive=lines drawFbo=%u readFbo=%u vertices=%u reason=offscreen_execute_failed",
				static_cast<u32>(m_vk->drawRecorder.drawFramebufferBinding()),
				static_cast<u32>(m_vk->drawRecorder.readFramebufferBinding()),
				static_cast<u32>(packet.vertices.size()));
		}
		return;
	}
	if (isVkFboTraceVerboseEnabled()) {
		vkFboTrace(
			"op=draw_enqueue primitive=lines drawFbo=%u vertices=%u textureMask=0x%x shaderFlags=0x%x",
			static_cast<u32>(m_vk->drawRecorder.drawFramebufferBinding()),
			static_cast<u32>(packet.vertices.size()),
			packet.textureSlotMask,
			packet.shaderFlags);
	}
	normalizePacketPositions(packet);
	m_vk->drawRecorder.pushPacket(std::move(packet), draw_dirty::kLineWidth);
#endif
}

bool ContextImpl::executeOffscreenDrawPacket(const DrawPacket & _packet, graphics::ObjectHandle _drawFramebuffer)
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	(void)_packet;
	(void)_drawFramebuffer;
	return false;
#else
	if (!m_vk || m_vk->device == VK_NULL_HANDLE || m_vk->commandPool == VK_NULL_HANDLE || m_vk->graphicsQueue == VK_NULL_HANDLE)
		return false;
	if (!_drawFramebuffer.isNotNull())
		return false;
	if (_packet.vertices.empty())
		return true;

	const u32 drawColorAttachmentLimit = m_vk->framebufferStore.getDrawBufferCount(_drawFramebuffer);
	graphics::BufferAttachmentParam colorAttachmentType = graphics::bufferAttachment::COLOR_ATTACHMENT0;
	const FramebufferStore::FramebufferAttachment * colorAttachment = resolveColorAttachment(
		m_vk->framebufferStore,
		_drawFramebuffer,
		graphics::bufferAttachment::COLOR_ATTACHMENT0,
		&colorAttachmentType,
		drawColorAttachmentLimit);
	if (colorAttachment == nullptr || !colorAttachment->textureHandle.isNotNull()) {
		vkFboTrace(
			"op=draw_offscreen_fail drawFbo=%u reason=no_color_attachment",
			static_cast<u32>(_drawFramebuffer));
		return false;
	}

	TextureStore::TextureResource * colorTexture = m_vk->textureStore.getTextureMutable(colorAttachment->textureHandle);
	if (colorTexture == nullptr
		|| colorTexture->image == VK_NULL_HANDLE
		|| colorTexture->imageView == VK_NULL_HANDLE
		|| colorTexture->width == 0
		|| colorTexture->height == 0
		|| colorTexture->vkFormat == VK_FORMAT_UNDEFINED) {
		vkFboTrace(
			"op=draw_offscreen_fail drawFbo=%u reason=invalid_color_texture attachment=%s texture=%u",
			static_cast<u32>(_drawFramebuffer),
			bufferAttachmentName(colorAttachmentType),
			static_cast<u32>(colorAttachment->textureHandle));
		return false;
	}
	if (colorTexture->msaaLevel != 0U) {
		static bool warnedColorMsaaMetadata = false;
		if (!warnedColorMsaaMetadata) {
			LOG(
				LOG_WARNING,
				"VK offscreen draw: color texture msaaLevel metadata=%u on texture=%u; continuing with single-sample path.",
				colorTexture->msaaLevel,
				static_cast<u32>(colorAttachment->textureHandle));
			warnedColorMsaaMetadata = true;
		}
	}

	TextureStore::TextureResource * depthTexture = nullptr;
	const FramebufferStore::FramebufferAttachment * depthAttachment = m_vk->framebufferStore.getAttachment(
		_drawFramebuffer,
		graphics::bufferAttachment::DEPTH_ATTACHMENT);
	if (depthAttachment != nullptr && depthAttachment->textureHandle.isNotNull()) {
		depthTexture = m_vk->textureStore.getTextureMutable(depthAttachment->textureHandle);
		if (depthTexture != nullptr
			&& (depthTexture->image == VK_NULL_HANDLE
				|| depthTexture->imageView == VK_NULL_HANDLE
				|| depthTexture->width == 0U
				|| depthTexture->height == 0U)) {
			vkFboTrace(
				"op=draw_offscreen depth_detach drawFbo=%u depthTexture=%u reason=invalid_depth_texture colorSize=%ux%u",
				static_cast<u32>(_drawFramebuffer),
				static_cast<u32>(depthAttachment->textureHandle),
				colorTexture->width,
				colorTexture->height);
			depthTexture = nullptr;
		} else if (depthTexture != nullptr && depthTexture->msaaLevel != 0U) {
			static bool warnedDepthMsaaMetadata = false;
			if (!warnedDepthMsaaMetadata) {
				LOG(
					LOG_WARNING,
					"VK offscreen draw: depth texture msaaLevel metadata=%u on texture=%u; continuing with single-sample path.",
					depthTexture->msaaLevel,
					static_cast<u32>(depthAttachment->textureHandle));
				warnedDepthMsaaMetadata = true;
			}
		}
	}

	u32 renderWidth = colorTexture->width;
	u32 renderHeight = colorTexture->height;
	if (depthTexture != nullptr) {
		const bool depthSizeMismatch = depthTexture->width != colorTexture->width
			|| depthTexture->height != colorTexture->height;
		if (depthSizeMismatch) {
			const u32 clampedWidth = std::min(colorTexture->width, depthTexture->width);
			const u32 clampedHeight = std::min(colorTexture->height, depthTexture->height);
			const bool needsDepth = _packet.state.depth.testEnabled || _packet.state.depth.writeEnabled;
			if (!needsDepth || clampedWidth == 0U || clampedHeight == 0U) {
				vkFboTrace(
					"op=draw_offscreen depth_detach drawFbo=%u depthTexture=%u reason=size_mismatch_depth_unused_or_empty colorSize=%ux%u depthSize=%ux%u",
					static_cast<u32>(_drawFramebuffer),
					static_cast<u32>(depthAttachment->textureHandle),
					colorTexture->width,
					colorTexture->height,
					depthTexture->width,
					depthTexture->height);
				depthTexture = nullptr;
			} else {
				vkFboTrace(
					"op=draw_offscreen extent_clamp drawFbo=%u colorSize=%ux%u depthSize=%ux%u clamped=%ux%u",
					static_cast<u32>(_drawFramebuffer),
					colorTexture->width,
					colorTexture->height,
					depthTexture->width,
					depthTexture->height,
					clampedWidth,
					clampedHeight);
				renderWidth = clampedWidth;
				renderHeight = clampedHeight;
			}
		}
	}
	if (renderWidth == 0U || renderHeight == 0U) {
		vkFboTrace(
			"op=draw_offscreen_fail drawFbo=%u reason=zero_render_extent colorSize=%ux%u",
			static_cast<u32>(_drawFramebuffer),
			colorTexture->width,
			colorTexture->height);
		return false;
	}
	const VkExtent2D renderExtent = { renderWidth, renderHeight };
	VkRenderPass renderPass = VK_NULL_HANDLE;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	VkBuffer vertexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	PipelineCache offscreenPipelineCache;
	bool success = false;
	bool commandBufferAllocated = false;

	auto cleanup = [&]() {
		offscreenPipelineCache.destroy();
		if (commandBufferAllocated && commandBuffer != VK_NULL_HANDLE)
			vkFreeCommandBuffers(m_vk->device, m_vk->commandPool, 1, &commandBuffer);
		if (vertexBuffer != VK_NULL_HANDLE)
			vkDestroyBuffer(m_vk->device, vertexBuffer, nullptr);
		if (vertexMemory != VK_NULL_HANDLE)
			vkFreeMemory(m_vk->device, vertexMemory, nullptr);
		if (framebuffer != VK_NULL_HANDLE)
			vkDestroyFramebuffer(m_vk->device, framebuffer, nullptr);
		if (renderPass != VK_NULL_HANDLE)
			vkDestroyRenderPass(m_vk->device, renderPass, nullptr);
	};

	do {
		VkAttachmentDescription attachments[2]{};
		u32 attachmentCount = 0U;
		VkClearValue clearValues[2]{};

		const bool colorWasUndefined = colorTexture->imageLayout == VK_IMAGE_LAYOUT_UNDEFINED;
		attachments[attachmentCount].flags = 0;
		attachments[attachmentCount].format = colorTexture->vkFormat;
		attachments[attachmentCount].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[attachmentCount].loadOp = colorWasUndefined ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[attachmentCount].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[attachmentCount].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[attachmentCount].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[attachmentCount].initialLayout = colorWasUndefined
			? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
			: colorTexture->imageLayout;
		attachments[attachmentCount].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		clearValues[attachmentCount].color.float32[0] = 0.0f;
		clearValues[attachmentCount].color.float32[1] = 0.0f;
		clearValues[attachmentCount].color.float32[2] = 0.0f;
		clearValues[attachmentCount].color.float32[3] = 0.0f;
		VkAttachmentReference colorAttachmentRef{};
		colorAttachmentRef.attachment = attachmentCount;
		colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		++attachmentCount;

		VkAttachmentReference depthAttachmentRef{};
		const bool hasDepth = depthTexture != nullptr;
		if (hasDepth) {
			const bool depthWasUndefined = depthTexture->imageLayout == VK_IMAGE_LAYOUT_UNDEFINED;
			attachments[attachmentCount].flags = 0;
			attachments[attachmentCount].format = depthTexture->vkFormat;
			attachments[attachmentCount].samples = VK_SAMPLE_COUNT_1_BIT;
			attachments[attachmentCount].loadOp = depthWasUndefined ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
			attachments[attachmentCount].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachments[attachmentCount].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachments[attachmentCount].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachments[attachmentCount].initialLayout = depthWasUndefined
				? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
				: depthTexture->imageLayout;
			attachments[attachmentCount].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			clearValues[attachmentCount].depthStencil.depth = 1.0f;
			clearValues[attachmentCount].depthStencil.stencil = 0U;
			depthAttachmentRef.attachment = attachmentCount;
			depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			++attachmentCount;
		}

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorAttachmentRef;
		subpass.pDepthStencilAttachment = hasDepth ? &depthAttachmentRef : nullptr;

		VkSubpassDependency dependency{};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
			| VK_PIPELINE_STAGE_TRANSFER_BIT
			| VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
			| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
			| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
			| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
			| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT
			| VK_ACCESS_TRANSFER_READ_BIT
			| VK_ACCESS_TRANSFER_WRITE_BIT
			| VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
			| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
			| VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
			| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
			| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		VkRenderPassCreateInfo renderPassCreateInfo{};
		renderPassCreateInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassCreateInfo.attachmentCount = attachmentCount;
		renderPassCreateInfo.pAttachments = attachments;
		renderPassCreateInfo.subpassCount = 1;
		renderPassCreateInfo.pSubpasses = &subpass;
		renderPassCreateInfo.dependencyCount = 1;
		renderPassCreateInfo.pDependencies = &dependency;
		if (vkCreateRenderPass(m_vk->device, &renderPassCreateInfo, nullptr, &renderPass) != VK_SUCCESS)
			break;

		VkImageView framebufferAttachments[2] = {
			colorTexture->imageView,
			hasDepth ? depthTexture->imageView : VK_NULL_HANDLE
		};
		VkFramebufferCreateInfo framebufferCreateInfo{};
		framebufferCreateInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebufferCreateInfo.renderPass = renderPass;
		framebufferCreateInfo.attachmentCount = hasDepth ? 2U : 1U;
		framebufferCreateInfo.pAttachments = framebufferAttachments;
		framebufferCreateInfo.width = renderExtent.width;
		framebufferCreateInfo.height = renderExtent.height;
		framebufferCreateInfo.layers = 1;
		if (vkCreateFramebuffer(m_vk->device, &framebufferCreateInfo, nullptr, &framebuffer) != VK_SUCCESS)
			break;

		if (!offscreenPipelineCache.init(
			m_vk->device,
			renderPass,
			m_vk->programLibrary.getBasicColorPipelineLayout(),
			m_vk->programLibrary.getBasicColorVertexShader(),
			m_vk->programLibrary.getBasicColorFragmentShader(),
			m_vk->programLibrary.getBasicTexturedVertexShader(),
			m_vk->programLibrary.getBasicTexturedFragmentShader())) {
			break;
		}

			DrawPacket normalizePacket = _packet;
			normalizePacket.state.raster.viewportX = 0;
				normalizePacket.state.raster.viewportY = 0;
				normalizePacket.state.raster.viewportWidth = static_cast<s32>(renderExtent.width);
				normalizePacket.state.raster.viewportHeight = static_cast<s32>(renderExtent.height);
				normalizePacket.state.raster.viewportValid = true;
			static const bool forceOffscreenRasterRectTransform = std::getenv("REALITYVK_VK_FORCE_OFFSCREEN_RASTER_RECT_TRANSFORM") != nullptr;
				normalizePacket.forceRasterRectTransform = forceOffscreenRasterRectTransform
					&& normalizePacket.transformMode == vulkan::VertexTransformMode::Rect;
		std::vector<DrawVertex> normalizedVertices;
		normalizedVertices.reserve(_packet.vertices.size());
		for (const DrawVertex & vertex : _packet.vertices)
			normalizedVertices.push_back(normalizeFallbackVertex(normalizePacket, vertex));
		const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(normalizedVertices.size() * sizeof(DrawVertex));
		if (vertexBytes == 0)
			break;

		VkBufferCreateInfo vertexBufferCreateInfo{};
		vertexBufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		vertexBufferCreateInfo.size = vertexBytes;
		vertexBufferCreateInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		vertexBufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(m_vk->device, &vertexBufferCreateInfo, nullptr, &vertexBuffer) != VK_SUCCESS)
			break;

		VkMemoryRequirements vertexMemoryRequirements{};
		vkGetBufferMemoryRequirements(m_vk->device, vertexBuffer, &vertexMemoryRequirements);
		const u32 memoryTypeIndex = findMemoryTypeIndex(
			m_vk->physicalDevice,
			vertexMemoryRequirements.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (memoryTypeIndex == UINT32_MAX)
			break;

		VkMemoryAllocateInfo memoryAllocateInfo{};
		memoryAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		memoryAllocateInfo.allocationSize = vertexMemoryRequirements.size;
		memoryAllocateInfo.memoryTypeIndex = memoryTypeIndex;
		if (vkAllocateMemory(m_vk->device, &memoryAllocateInfo, nullptr, &vertexMemory) != VK_SUCCESS)
			break;
		if (vkBindBufferMemory(m_vk->device, vertexBuffer, vertexMemory, 0) != VK_SUCCESS)
			break;

		void * mapped = nullptr;
		if (vkMapMemory(m_vk->device, vertexMemory, 0, vertexBytes, 0, &mapped) != VK_SUCCESS || mapped == nullptr)
			break;
		std::memcpy(mapped, normalizedVertices.data(), static_cast<size_t>(vertexBytes));
		vkUnmapMemory(m_vk->device, vertexMemory);

		VkCommandBufferAllocateInfo commandAllocateInfo{};
		commandAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		commandAllocateInfo.commandPool = m_vk->commandPool;
		commandAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commandAllocateInfo.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(m_vk->device, &commandAllocateInfo, &commandBuffer) != VK_SUCCESS)
			break;
		commandBufferAllocated = true;

		VkCommandBufferBeginInfo commandBeginInfo{};
		commandBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		commandBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (vkBeginCommandBuffer(commandBuffer, &commandBeginInfo) != VK_SUCCESS)
			break;

		VkRenderPassBeginInfo renderPassBeginInfo{};
		renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		renderPassBeginInfo.renderPass = renderPass;
		renderPassBeginInfo.framebuffer = framebuffer;
		renderPassBeginInfo.renderArea.offset = { 0, 0 };
		renderPassBeginInfo.renderArea.extent = renderExtent;
		renderPassBeginInfo.clearValueCount = attachmentCount;
		renderPassBeginInfo.pClearValues = clearValues;
		vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

		m_vk->descriptorBinder.beginFrame(m_vk->frameSyncIndex);
		DrawPacket packetCopy = _packet;
		static const bool disableStrictOffscreen = std::getenv("REALITYVK_VK_STRICT_OFFSCREEN_DISABLE") != nullptr;
		if (disableStrictOffscreen) {
			packetCopy.shaderFlags &= ~vulkan::draw_shader_flags::kSpecialStrictBlendMux;
			packetCopy.blendMux1Packed = 0U;
			packetCopy.blendMux2Packed = 0U;
			packetCopy.blendParamsPacked = 0U;
		}
		packetCopy.vertices = std::move(normalizedVertices);
		static const bool debugFlipRtTexrectY = std::getenv("REALITYVK_VK_DEBUG_FLIP_RT_TEXRECT_Y") != nullptr;
		if (debugFlipRtTexrectY
			&& packetCopy.debugTexrect
			&& !packetCopy.vertices.empty()) {
			bool sampleRtTexture = false;
			for (u32 slot = 0U; slot < 2U; ++slot) {
				if ((packetCopy.textureSlotMask & (1U << slot)) == 0U)
					continue;
				const TextureStore::TextureResource * texture = m_vk->textureStore.getTexture(packetCopy.textureSlots[slot].texture);
				if (texture != nullptr && texture->renderTargetWritten) {
					sampleRtTexture = true;
					break;
				}
			}
			if (sampleRtTexture) {
				for (DrawVertex & vertex : packetCopy.vertices) {
					vertex.t0 = 1.0f - vertex.t0;
					vertex.t1 = 1.0f - vertex.t1;
				}
			}
		}
		static const bool debugFlipOffscreenTexrectY = std::getenv("REALITYVK_VK_DEBUG_FLIP_OFFSCREEN_TEXRECT_Y") != nullptr;
		static const u32 debugFlipOffscreenTexrectHandle = []() -> u32 {
			const char * env = std::getenv("REALITYVK_VK_DEBUG_FLIP_OFFSCREEN_TEXRECT_HANDLE");
			if (env == nullptr || env[0] == '\0')
				return 0U;
			return static_cast<u32>(std::strtoul(env, nullptr, 10));
		}();
		if (debugFlipOffscreenTexrectY
			&& packetCopy.debugSource == kPacketSourceRects
			&& packetCopy.debugTexrect
			&& (packetCopy.textureSlotMask & (1U << 0)) != 0U
			&& !packetCopy.vertices.empty()) {
			bool handleMatch = debugFlipOffscreenTexrectHandle == 0U;
			if (!handleMatch) {
				for (u32 slot = 0U; slot < 2U; ++slot) {
					if ((packetCopy.textureSlotMask & (1U << slot)) == 0U)
						continue;
					if (static_cast<u32>(packetCopy.textureSlots[slot].texture) == debugFlipOffscreenTexrectHandle) {
						handleMatch = true;
						break;
					}
				}
			}
			if (handleMatch) {
				for (DrawVertex & vertex : packetCopy.vertices) {
					vertex.t0 = 1.0f - vertex.t0;
					vertex.t1 = 1.0f - vertex.t1;
				}
			}
		}
		static const bool debugFlipOffscreenTexrectPosY = std::getenv("REALITYVK_VK_DEBUG_FLIP_OFFSCREEN_TEXRECT_POS_Y") != nullptr;
		if (debugFlipOffscreenTexrectPosY
			&& packetCopy.debugSource == kPacketSourceRects
			&& packetCopy.debugTexrect
			&& !packetCopy.vertices.empty()) {
			for (DrawVertex & vertex : packetCopy.vertices)
				vertex.y = -vertex.y;
		}
		static const bool debugFlipOffscreenFillRectPosY = std::getenv("REALITYVK_VK_DEBUG_FLIP_OFFSCREEN_FILLRECT_POS_Y") != nullptr;
		if (debugFlipOffscreenFillRectPosY
			&& packetCopy.debugSource == kPacketSourceRects
			&& !packetCopy.debugTexrect
			&& !packetCopy.vertices.empty()) {
			for (DrawVertex & vertex : packetCopy.vertices)
				vertex.y = -vertex.y;
		}
		static const bool debugFlipOffscreenTriangleY = std::getenv("REALITYVK_VK_DEBUG_FLIP_OFFSCREEN_TRIANGLE_Y") != nullptr;
		if (debugFlipOffscreenTriangleY
			&& packetCopy.transformMode == vulkan::VertexTransformMode::Triangle
			&& !packetCopy.vertices.empty()) {
			for (DrawVertex & vertex : packetCopy.vertices)
				vertex.y = -vertex.y;
		}
		static const bool debugPaintOffscreenTriangles = std::getenv("REALITYVK_VK_DEBUG_PAINT_OFFSCREEN_TRIANGLES") != nullptr;
		if (debugPaintOffscreenTriangles
			&& packetCopy.transformMode == vulkan::VertexTransformMode::Triangle
			&& !packetCopy.vertices.empty()) {
			packetCopy.textureSlotMask = 0U;
			packetCopy.shaderFlags = vulkan::draw_shader_flags::kShade;
			packetCopy.blendMux1Packed = 0U;
			packetCopy.blendMux2Packed = 0U;
			packetCopy.blendParamsPacked = 0U;
			for (DrawVertex & vertex : packetCopy.vertices) {
				vertex.r = 0.0f;
				vertex.g = 1.0f;
				vertex.b = 0.0f;
				vertex.a = 1.0f;
			}
		}
		static const bool debugPaintOffscreenTexrects = std::getenv("REALITYVK_VK_DEBUG_PAINT_OFFSCREEN_TEXRECTS") != nullptr;
		if (debugPaintOffscreenTexrects
			&& packetCopy.transformMode == vulkan::VertexTransformMode::Rect
			&& packetCopy.debugTexrect
			&& !packetCopy.vertices.empty()) {
			packetCopy.textureSlotMask = 0U;
			packetCopy.shaderFlags = vulkan::draw_shader_flags::kShade;
			packetCopy.blendMux1Packed = 0U;
			packetCopy.blendMux2Packed = 0U;
			packetCopy.blendParamsPacked = 0U;
			for (DrawVertex & vertex : packetCopy.vertices) {
				vertex.r = 1.0f;
				vertex.g = 0.0f;
				vertex.b = 1.0f;
				vertex.a = 1.0f;
			}
		}
			packetCopy.state.raster.viewportX = 0;
			packetCopy.state.raster.viewportY = 0;
			packetCopy.state.raster.viewportWidth = static_cast<s32>(renderExtent.width);
			packetCopy.state.raster.viewportHeight = static_cast<s32>(renderExtent.height);
			packetCopy.state.raster.viewportValid = true;
			packetCopy.dirtyMask |= draw_dirty::kViewport;
		static const bool debugOffscreen = std::getenv("REALITYVK_VK_DEBUG_OFFSCREEN") != nullptr;
		static const u32 debugTraceRtHandle = []() -> u32 {
			const char * env = std::getenv("REALITYVK_VK_DEBUG_TRACE_RT_HANDLE");
			if (env == nullptr || env[0] == '\0')
				return 0U;
			return static_cast<u32>(std::strtoul(env, nullptr, 10));
		}();
		static u32 debugTraceRtLogCount = 0U;
		if (debugOffscreen && !packetCopy.vertices.empty()) {
			static const u32 offscreenLogLimit = []() -> u32 {
				const char * env = std::getenv("REALITYVK_VK_DEBUG_OFFSCREEN_LIMIT");
				if (env == nullptr || env[0] == '\0')
					return 96U;
				return static_cast<u32>(std::strtoul(env, nullptr, 10));
			}();
			static u32 offscreenLogCount = 0U;
			if (offscreenLogCount < offscreenLogLimit) {
				f32 preMinX = _packet.vertices[0].x;
				f32 preMinY = _packet.vertices[0].y;
				f32 preMaxX = preMinX;
				f32 preMaxY = preMinY;
				f32 minX = packetCopy.vertices[0].x;
				f32 minY = packetCopy.vertices[0].y;
				f32 maxX = minX;
				f32 maxY = minY;
				f32 minS0 = packetCopy.vertices[0].s0;
				f32 minT0 = packetCopy.vertices[0].t0;
				f32 maxS0 = minS0;
				f32 maxT0 = minT0;
				f32 minZ = packetCopy.vertices[0].z;
				f32 maxZ = minZ;
				f32 minW = packetCopy.vertices[0].w;
				f32 maxW = minW;
				f32 minR = packetCopy.vertices[0].r;
				f32 minG = packetCopy.vertices[0].g;
				f32 minB = packetCopy.vertices[0].b;
				f32 minA = packetCopy.vertices[0].a;
				f32 maxR = minR;
				f32 maxG = minG;
				f32 maxB = minB;
				f32 maxA = minA;
				for (const DrawVertex & vertex : _packet.vertices) {
					preMinX = std::min(preMinX, vertex.x);
					preMinY = std::min(preMinY, vertex.y);
					preMaxX = std::max(preMaxX, vertex.x);
					preMaxY = std::max(preMaxY, vertex.y);
				}
				for (const DrawVertex & vertex : packetCopy.vertices) {
					minX = std::min(minX, vertex.x);
					minY = std::min(minY, vertex.y);
					maxX = std::max(maxX, vertex.x);
					maxY = std::max(maxY, vertex.y);
					minS0 = std::min(minS0, vertex.s0);
					minT0 = std::min(minT0, vertex.t0);
					maxS0 = std::max(maxS0, vertex.s0);
					maxT0 = std::max(maxT0, vertex.t0);
					minZ = std::min(minZ, vertex.z);
					maxZ = std::max(maxZ, vertex.z);
					minW = std::min(minW, vertex.w);
					maxW = std::max(maxW, vertex.w);
					minR = std::min(minR, vertex.r);
					minG = std::min(minG, vertex.g);
					minB = std::min(minB, vertex.b);
					minA = std::min(minA, vertex.a);
					maxR = std::max(maxR, vertex.r);
					maxG = std::max(maxG, vertex.g);
					maxB = std::max(maxB, vertex.b);
					maxA = std::max(maxA, vertex.a);
				}
					LOG(
						LOG_WARNING,
						"VK offscreen debug: id=%llu src=%s texrect=%u mux=0x%016llx cycle=%u fbo=%u extent=%ux%u mode=%s vertices=%u shaderFlags=0x%08x texMask=0x%08x blend=[en=%u src=%u,%u dst=%u,%u] depth=[test=%u write=%u cmp=%u] prePos=[%.3f,%.3f]-[%.3f,%.3f] pos=[%.3f,%.3f]-[%.3f,%.3f] z=[%.3f,%.3f] w=[%.3f,%.3f] color=[%.3f,%.3f,%.3f,%.3f]-[%.3f,%.3f,%.3f,%.3f] tex0=[%.3f,%.3f]-[%.3f,%.3f] srcViewport=%d,%d,%d,%d srcViewportValid=%u",
						static_cast<unsigned long long>(packetCopy.debugPacketId),
						packetSourceName(packetCopy.debugSource),
						packetCopy.debugTexrect ? 1U : 0U,
						static_cast<unsigned long long>(packetCopy.debugCombinerMux),
						packetCopy.debugCombinerCycleType,
						static_cast<u32>(_drawFramebuffer),
						renderExtent.width,
						renderExtent.height,
					transformModeName(packetCopy.transformMode),
						static_cast<u32>(packetCopy.vertices.size()),
						packetCopy.shaderFlags,
						packetCopy.textureSlotMask,
						packetCopy.state.blend.enabled ? 1U : 0U,
						static_cast<u32>(packetCopy.state.blend.srcColor),
						static_cast<u32>(packetCopy.state.blend.srcAlpha),
						static_cast<u32>(packetCopy.state.blend.dstColor),
						static_cast<u32>(packetCopy.state.blend.dstAlpha),
						packetCopy.state.depth.testEnabled ? 1U : 0U,
						packetCopy.state.depth.writeEnabled ? 1U : 0U,
						static_cast<u32>(packetCopy.state.depth.compare),
					preMinX,
					preMinY,
					preMaxX,
					preMaxY,
					minX,
					minY,
					maxX,
					maxY,
					minZ,
						maxZ,
						minW,
						maxW,
						minR,
						minG,
						minB,
						minA,
						maxR,
						maxG,
						maxB,
						maxA,
						minS0,
						minT0,
						maxS0,
						maxT0,
					_packet.state.raster.viewportX,
					_packet.state.raster.viewportY,
					_packet.state.raster.viewportWidth,
					_packet.state.raster.viewportHeight,
					_packet.state.raster.viewportValid ? 1U : 0U);
				++offscreenLogCount;
			}
		}
		if (debugTraceRtHandle != 0U
			&& debugTraceRtLogCount < 256U
			&& static_cast<u32>(colorAttachment->textureHandle) == debugTraceRtHandle) {
			f32 minX = 0.0f;
			f32 minY = 0.0f;
			f32 maxX = 0.0f;
			f32 maxY = 0.0f;
			f32 minS0 = 0.0f;
			f32 maxS0 = 0.0f;
			f32 minT0 = 0.0f;
			f32 maxT0 = 0.0f;
			f32 minS1 = 0.0f;
			f32 maxS1 = 0.0f;
			f32 minT1 = 0.0f;
			f32 maxT1 = 0.0f;
			if (!packetCopy.vertices.empty()) {
				minX = maxX = packetCopy.vertices[0].x;
				minY = maxY = packetCopy.vertices[0].y;
				minS0 = maxS0 = packetCopy.vertices[0].s0;
				minT0 = maxT0 = packetCopy.vertices[0].t0;
				minS1 = maxS1 = packetCopy.vertices[0].s1;
				minT1 = maxT1 = packetCopy.vertices[0].t1;
				for (const DrawVertex & vertex : packetCopy.vertices) {
					minX = std::min(minX, vertex.x);
					maxX = std::max(maxX, vertex.x);
					minY = std::min(minY, vertex.y);
					maxY = std::max(maxY, vertex.y);
					minS0 = std::min(minS0, vertex.s0);
					maxS0 = std::max(maxS0, vertex.s0);
					minT0 = std::min(minT0, vertex.t0);
					maxT0 = std::max(maxT0, vertex.t0);
					minS1 = std::min(minS1, vertex.s1);
					maxS1 = std::max(maxS1, vertex.s1);
					minT1 = std::min(minT1, vertex.t1);
					maxT1 = std::max(maxT1, vertex.t1);
				}
			}
			const DrawVertex * v0 = packetCopy.vertices.size() > 0 ? &packetCopy.vertices[0] : nullptr;
			const DrawVertex * v1 = packetCopy.vertices.size() > 1 ? &packetCopy.vertices[1] : nullptr;
			const DrawVertex * v2 = packetCopy.vertices.size() > 2 ? &packetCopy.vertices[2] : nullptr;
			const DrawVertex * v3 = packetCopy.vertices.size() > 3 ? &packetCopy.vertices[3] : nullptr;
			u32 sampledHandle0 = 0U;
			u32 sampledHandle1 = 0U;
			u32 sampledHandle0Rt = 0U;
			u32 sampledHandle1Rt = 0U;
			if ((packetCopy.textureSlotMask & (1U << 0U)) != 0U) {
				sampledHandle0 = static_cast<u32>(packetCopy.textureSlots[0U].texture);
				const TextureStore::TextureResource * sampled0 = m_vk->textureStore.getTexture(packetCopy.textureSlots[0U].texture);
				sampledHandle0Rt = sampled0 != nullptr && sampled0->renderTargetWritten ? 1U : 0U;
			}
			if ((packetCopy.textureSlotMask & (1U << 1U)) != 0U) {
				sampledHandle1 = static_cast<u32>(packetCopy.textureSlots[1U].texture);
				const TextureStore::TextureResource * sampled1 = m_vk->textureStore.getTexture(packetCopy.textureSlots[1U].texture);
				sampledHandle1Rt = sampled1 != nullptr && sampled1->renderTargetWritten ? 1U : 0U;
			}
			LOG(
				LOG_WARNING,
				"VK RT trace(write): handle=%u fbo=%u packetId=%llu src=%s texrect=%u mode=%s mux=0x%016llx cycle=%u vertices=%u sample0=%u(rt=%u) sample1=%u(rt=%u) pos=[%.6f,%.6f]-[%.6f,%.6f] tex0=[%.6f,%.6f]-[%.6f,%.6f] tex1=[%.6f,%.6f]-[%.6f,%.6f] v0=[%.6f,%.6f,%.6f,%.6f] v1=[%.6f,%.6f,%.6f,%.6f] v2=[%.6f,%.6f,%.6f,%.6f] v3=[%.6f,%.6f,%.6f,%.6f]",
				debugTraceRtHandle,
				static_cast<u32>(_drawFramebuffer),
				static_cast<unsigned long long>(packetCopy.debugPacketId),
				packetSourceName(packetCopy.debugSource),
				packetCopy.debugTexrect ? 1U : 0U,
				transformModeName(packetCopy.transformMode),
				static_cast<unsigned long long>(packetCopy.debugCombinerMux),
				packetCopy.debugCombinerCycleType,
				static_cast<u32>(packetCopy.vertices.size()),
				sampledHandle0,
				sampledHandle0Rt,
				sampledHandle1,
				sampledHandle1Rt,
				minX,
				minY,
				maxX,
				maxY,
				minS0,
				minT0,
				maxS0,
				maxT0,
				minS1,
				minT1,
				maxS1,
				maxT1,
				v0 != nullptr ? v0->x : 0.0f,
				v0 != nullptr ? v0->y : 0.0f,
				v0 != nullptr ? v0->s0 : 0.0f,
				v0 != nullptr ? v0->t0 : 0.0f,
				v1 != nullptr ? v1->x : 0.0f,
				v1 != nullptr ? v1->y : 0.0f,
				v1 != nullptr ? v1->s0 : 0.0f,
				v1 != nullptr ? v1->t0 : 0.0f,
				v2 != nullptr ? v2->x : 0.0f,
				v2 != nullptr ? v2->y : 0.0f,
				v2 != nullptr ? v2->s0 : 0.0f,
				v2 != nullptr ? v2->t0 : 0.0f,
				v3 != nullptr ? v3->x : 0.0f,
				v3 != nullptr ? v3->y : 0.0f,
				v3 != nullptr ? v3->s0 : 0.0f,
				v3 != nullptr ? v3->t0 : 0.0f);
			++debugTraceRtLogCount;
		}
		std::vector<DrawPacket> packets;
		packets.emplace_back(std::move(packetCopy));

		DrawCommandEncoder::EncodeInfo drawInfo{};
		drawInfo.commandBuffer = commandBuffer;
		drawInfo.vertexBuffer = vertexBuffer;
		drawInfo.vertexBufferOffset = 0;
		drawInfo.packets = &packets;
		drawInfo.pipelineCache = &offscreenPipelineCache;
		drawInfo.descriptorBinder = &m_vk->descriptorBinder;
		drawInfo.pipelineLayout = m_vk->programLibrary.getBasicColorPipelineLayout();
		drawInfo.renderExtent = renderExtent;
		drawInfo.wideLinesEnabled = m_vk->wideLinesEnabled;
		drawInfo.maxLineWidth = m_maxLineWidth;
		DrawCommandEncoder::encode(drawInfo);

		vkCmdEndRenderPass(commandBuffer);
		if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
			break;

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;
		if (vkQueueSubmit(m_vk->graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS)
			break;
		if (vkQueueWaitIdle(m_vk->graphicsQueue) != VK_SUCCESS)
			break;

		colorTexture->imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		colorTexture->hasContent = true;
		colorTexture->renderTargetWritten = true;
		colorTexture->contentWidth = std::max(colorTexture->contentWidth, renderExtent.width);
		colorTexture->contentHeight = std::max(colorTexture->contentHeight, renderExtent.height);
		if (depthTexture != nullptr) {
			depthTexture->imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			depthTexture->hasContent = true;
			depthTexture->renderTargetWritten = true;
			depthTexture->contentWidth = std::max(depthTexture->contentWidth, renderExtent.width);
			depthTexture->contentHeight = std::max(depthTexture->contentHeight, renderExtent.height);
		}
		success = true;
	} while (false);

	if (!success) {
		vkFboTrace(
			"op=draw_offscreen result=0 drawFbo=%u vertices=%u textureMask=0x%x",
			static_cast<u32>(_drawFramebuffer),
			static_cast<u32>(_packet.vertices.size()),
			_packet.textureSlotMask);
	} else if (isVkFboTraceVerboseEnabled()) {
		vkFboTrace(
			"op=draw_offscreen result=1 drawFbo=%u vertices=%u textureMask=0x%x",
			static_cast<u32>(_drawFramebuffer),
			static_cast<u32>(_packet.vertices.size()),
			_packet.textureSlotMask);
	}
	cleanup();
	return success;
#endif
}

f32 ContextImpl::getMaxLineWidth()
{
	return m_maxLineWidth;
}

bool ContextImpl::present()
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_coreReady || !m_vk || m_vk->device == VK_NULL_HANDLE || m_vk->swapchain == VK_NULL_HANDLE)
		return false;
	if (m_vk->graphicsQueue == VK_NULL_HANDLE || m_vk->presentQueue == VK_NULL_HANDLE)
		return false;
	if (m_vk->frameSync.empty() || m_vk->frameCommandBuffers.empty())
		return false;
	if (m_vk->frameSyncIndex >= m_vk->frameCommandBuffers.size())
		return false;

	VulkanState::FrameSync & currentFrame = m_vk->frameSync[m_vk->frameSyncIndex];
	VkCommandBuffer commandBuffer = m_vk->frameCommandBuffers[m_vk->frameSyncIndex];
	if (currentFrame.imageAvailable == VK_NULL_HANDLE
		|| currentFrame.renderFinished == VK_NULL_HANDLE
		|| currentFrame.inFlight == VK_NULL_HANDLE
		|| commandBuffer == VK_NULL_HANDLE) {
		return false;
	}

	bool success = false;
	do {
		if (vkWaitForFences(m_vk->device, 1, &currentFrame.inFlight, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkWaitForFences failed.");
			break;
		}

		u32 currentImageIndex = 0;
		const VkResult acquireResult = vkAcquireNextImageKHR(
			m_vk->device,
			m_vk->swapchain,
			UINT64_MAX,
			currentFrame.imageAvailable,
			VK_NULL_HANDLE,
			&currentImageIndex);

		if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR || acquireResult == VK_SUBOPTIMAL_KHR) {
			vkDeviceWaitIdle(m_vk->device);
			destroySwapchain();
			createSwapchain();
			break;
		}

		if (acquireResult != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkAcquireNextImageKHR failed: %d", static_cast<int>(acquireResult));
			break;
		}

		if (currentImageIndex >= m_vk->swapchainImageFences.size()) {
			LOG(LOG_WARNING, "Acquired Vulkan swapchain image index is out of range: %u", currentImageIndex);
			break;
		}
		if (currentImageIndex >= m_vk->swapchainFramebuffers.size()) {
			LOG(LOG_WARNING, "Acquired Vulkan framebuffer index is out of range: %u", currentImageIndex);
			break;
		}
		if (m_vk->renderPass == VK_NULL_HANDLE) {
			LOG(LOG_WARNING, "Vulkan render pass is not initialized.");
			break;
		}

		VkFence & imageFence = m_vk->swapchainImageFences[currentImageIndex];
		if (imageFence != VK_NULL_HANDLE && imageFence != currentFrame.inFlight) {
			if (vkWaitForFences(m_vk->device, 1, &imageFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
				LOG(LOG_WARNING, "vkWaitForFences failed for swapchain image.");
				break;
			}
		}

		if (vkResetCommandBuffer(commandBuffer, 0) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkResetCommandBuffer failed.");
			break;
		}

			const std::vector<DrawPacket> & queuedPackets = m_vk->drawRecorder.packets();
			std::vector<DrawPacket> presentPackets;
			presentPackets.reserve(queuedPackets.size() + 1U);
			size_t totalVertexCount = 0;
			u32 skippedSuspiciousPresentPackets = 0U;
			bool hasLargeTexturedPacket = false;
			static const bool enablePresentFallback = std::getenv("REALITYVK_VK_ENABLE_PRESENT_FALLBACK") != nullptr;

			auto packetTextureDimensions = [&](const DrawPacket & _packet, u32 & _width, u32 & _height) -> bool {
				_width = 0U;
				_height = 0U;
				if ((_packet.textureSlotMask & (1U << 0)) == 0U)
					return false;
				const TextureStore::TextureResource * texture = m_vk->textureStore.getTexture(_packet.textureSlots[0].texture);
				if (texture == nullptr)
					return false;
				_width = texture->contentWidth != 0U ? texture->contentWidth : texture->width;
				_height = texture->contentHeight != 0U ? texture->contentHeight : texture->height;
				return _width != 0U && _height != 0U;
			};
			static const u32 debugTraceRtHandle = []() -> u32 {
				const char * env = std::getenv("REALITYVK_VK_DEBUG_TRACE_RT_HANDLE");
				if (env == nullptr || env[0] == '\0')
					return 0U;
				return static_cast<u32>(std::strtoul(env, nullptr, 10));
			}();
			static u32 debugTraceRtReadLogCount = 0U;
			static const std::vector<u32> debugTraceFinalLayerHandles = []() -> std::vector<u32> {
				const char * env = std::getenv("REALITYVK_VK_DEBUG_TRACE_FINAL_LAYER_HANDLES");
				std::vector<u32> handles;
				if (env == nullptr || env[0] == '\0')
					return handles;
				const char * cursor = env;
				while (*cursor != '\0') {
					while (*cursor == ',' || *cursor == ' ' || *cursor == '\t')
						++cursor;
					if (*cursor == '\0')
						break;
					char * end = nullptr;
					const unsigned long value = std::strtoul(cursor, &end, 0);
					if (end == cursor) {
						while (*cursor != '\0' && *cursor != ',')
							++cursor;
						continue;
					}
					handles.push_back(static_cast<u32>(value));
					cursor = end;
				}
				std::sort(handles.begin(), handles.end());
				handles.erase(std::unique(handles.begin(), handles.end()), handles.end());
				return handles;
			}();
			static const bool debugTraceFinalLayers = !debugTraceFinalLayerHandles.empty();
			static const u32 debugTraceFinalLayerLimit = []() -> u32 {
				const char * env = std::getenv("REALITYVK_VK_DEBUG_TRACE_FINAL_LAYER_LIMIT");
				if (env == nullptr || env[0] == '\0')
					return 96U;
				return static_cast<u32>(std::strtoul(env, nullptr, 10));
			}();
			static u32 debugTraceFinalLayerLogCount = 0U;

			auto packetMatchesFinalLayerHandle = [&](const DrawPacket & _packet, u32 & _matchedHandle, u32 & _matchedSlot, bool & _matchedHandleIsRt) -> bool {
				_matchedHandle = 0U;
				_matchedSlot = 0U;
				_matchedHandleIsRt = false;
				if (!debugTraceFinalLayers)
					return false;
				for (u32 slot = 0U; slot < 2U; ++slot) {
					if ((_packet.textureSlotMask & (1U << slot)) == 0U)
						continue;
					const u32 handle = static_cast<u32>(_packet.textureSlots[slot].texture);
					if (!std::binary_search(debugTraceFinalLayerHandles.begin(), debugTraceFinalLayerHandles.end(), handle))
						continue;
					_matchedHandle = handle;
					_matchedSlot = slot;
					const TextureStore::TextureResource * texture = m_vk->textureStore.getTexture(_packet.textureSlots[slot].texture);
					_matchedHandleIsRt = texture != nullptr && texture->renderTargetWritten;
					return true;
				}
				return false;
			};

			auto packetIsFullscreen = [](const DrawPacket & _packet) -> bool {
				if (_packet.vertices.empty())
					return false;
				f32 minX = _packet.vertices[0].x;
				f32 minY = _packet.vertices[0].y;
				f32 maxX = minX;
				f32 maxY = minY;
				for (const DrawVertex & vertex : _packet.vertices) {
					minX = std::min(minX, vertex.x);
					minY = std::min(minY, vertex.y);
					maxX = std::max(maxX, vertex.x);
					maxY = std::max(maxY, vertex.y);
				}
				const f32 width = maxX - minX;
				const f32 height = maxY - minY;
				return width >= 1.70f && height >= 1.70f;
			};

			const u64 swapchainArea = static_cast<u64>(std::max<u32>(1U, m_vk->swapchainExtent.width))
				* static_cast<u64>(std::max<u32>(1U, m_vk->swapchainExtent.height));
			auto isSuspiciousFullscreenSmallTexturePacket = [&](const DrawPacket & _packet) -> bool {
				if (_packet.debugSource != kPacketSourceRects || !_packet.debugTexrect)
					return false;
				if ((_packet.shaderFlags & vulkan::draw_shader_flags::kTexture0) == 0U)
					return false;
				if (!packetIsFullscreen(_packet))
					return false;
				u32 textureWidth = 0U;
				u32 textureHeight = 0U;
				if (!packetTextureDimensions(_packet, textureWidth, textureHeight))
					return false;
				const u64 textureArea = static_cast<u64>(textureWidth) * static_cast<u64>(textureHeight);
				// Reject atlas-sized fullscreen packets that frequently override the real scene.
				return textureArea * 64ULL < swapchainArea;
			};

				for (const DrawPacket & packet : queuedPackets) {
					DrawPacket normalizePacket = packet;
					normalizePacket.state.raster.viewportX = 0;
				normalizePacket.state.raster.viewportY = 0;
				normalizePacket.state.raster.viewportWidth = static_cast<s32>(m_vk->swapchainExtent.width);
				normalizePacket.state.raster.viewportHeight = static_cast<s32>(m_vk->swapchainExtent.height);
				normalizePacket.state.raster.viewportValid = true;
				normalizePacket.forceRasterRectTransform = normalizePacket.transformMode == vulkan::VertexTransformMode::Rect;
					DrawPacket packetCopy = packet;
					packetCopy.vertices.clear();
					packetCopy.vertices.reserve(packet.vertices.size());
					for (const DrawVertex & vertex : packet.vertices)
						packetCopy.vertices.push_back(normalizeFallbackVertex(normalizePacket, vertex));
					bool appliedRtTexrectFlip = false;
					u32 appliedRtTexrectFlipSlotMask = 0U;

					static const bool autoFlipRtTexrectY = std::getenv("REALITYVK_VK_DISABLE_RT_TEXRECT_Y_FLIP") == nullptr;
					static const bool debugFlipRtTexrectY = std::getenv("REALITYVK_VK_DEBUG_FLIP_RT_TEXRECT_Y") != nullptr;
					static const u32 debugFlipRtTexrectHandle = []() -> u32 {
						const char * env = std::getenv("REALITYVK_VK_DEBUG_FLIP_RT_TEXRECT_HANDLE");
						if (env == nullptr || env[0] == '\0')
							return 0U;
						return static_cast<u32>(std::strtoul(env, nullptr, 10));
					}();
					if ((autoFlipRtTexrectY || debugFlipRtTexrectY)
						&& packetCopy.debugTexrect
						&& !packetCopy.vertices.empty()) {
						bool sampleRtTexture = false;
						u32 sampledRtSlotMask = 0U;
						const bool filterByDebugHandle = debugFlipRtTexrectY && debugFlipRtTexrectHandle != 0U;
						bool hasTargetHandle = !filterByDebugHandle;
						for (u32 slot = 0U; slot < 2U; ++slot) {
							if ((packetCopy.textureSlotMask & (1U << slot)) == 0U)
								continue;
							const graphics::ObjectHandle handle = packetCopy.textureSlots[slot].texture;
							if (filterByDebugHandle && static_cast<u32>(handle) == debugFlipRtTexrectHandle)
								hasTargetHandle = true;
							const TextureStore::TextureResource * texture = m_vk->textureStore.getTexture(handle);
							if (texture != nullptr && texture->renderTargetWritten) {
								sampleRtTexture = true;
								if (!filterByDebugHandle || static_cast<u32>(handle) == debugFlipRtTexrectHandle)
									sampledRtSlotMask |= (1U << slot);
								if (hasTargetHandle)
									break;
							}
						}
						if (sampleRtTexture && hasTargetHandle && sampledRtSlotMask != 0U) {
							for (DrawVertex & vertex : packetCopy.vertices) {
								if ((sampledRtSlotMask & (1U << 0U)) != 0U)
									vertex.t0 = 1.0f - vertex.t0;
								if ((sampledRtSlotMask & (1U << 1U)) != 0U)
									vertex.t1 = 1.0f - vertex.t1;
							}
							appliedRtTexrectFlip = true;
							appliedRtTexrectFlipSlotMask = sampledRtSlotMask;
						}
					}

					static const bool debugFlipPresentTexrectY = std::getenv("REALITYVK_VK_DEBUG_FLIP_PRESENT_TEXRECT_Y") != nullptr;
					if (debugFlipPresentTexrectY
						&& packetCopy.debugSource == kPacketSourceRects
						&& packetCopy.debugTexrect
						&& (packetCopy.textureSlotMask & (1U << 0)) != 0U
						&& !packetCopy.vertices.empty()) {
						f32 minT0 = packetCopy.vertices[0].t0;
						f32 maxT0 = minT0;
						for (const DrawVertex & vertex : packetCopy.vertices) {
							minT0 = std::min(minT0, vertex.t0);
							maxT0 = std::max(maxT0, vertex.t0);
						}
						// Only flip when texcoords are normalized-like.
						if (minT0 >= -0.25f && maxT0 <= 1.25f) {
							for (DrawVertex & vertex : packetCopy.vertices) {
								vertex.t0 = 1.0f - vertex.t0;
								vertex.t1 = 1.0f - vertex.t1;
							}
						}
					}
					static const bool canonicalizeFinalRtTexrectPos = std::getenv("REALITYVK_VK_DISABLE_CANONICAL_FINAL_RT_TEXRECT_POS") == nullptr;
					static const bool canonicalizeFinalRtTexrectUv = std::getenv("REALITYVK_VK_DISABLE_CANONICAL_FINAL_RT_TEXRECT_UV") == nullptr;
					if (canonicalizeFinalRtTexrectPos
						&& packetCopy.debugTexrect
						&& packetIsFullscreen(packetCopy)
						&& packetCopy.vertices.size() == 4U) {
						bool sampledRtTexture = false;
						for (u32 slot = 0U; slot < 2U; ++slot) {
							if ((packetCopy.textureSlotMask & (1U << slot)) == 0U)
								continue;
							const TextureStore::TextureResource * texture = m_vk->textureStore.getTexture(packetCopy.textureSlots[slot].texture);
							if (texture != nullptr && texture->renderTargetWritten) {
								sampledRtTexture = true;
								break;
							}
						}
						if (sampledRtTexture) {
							packetCopy.vertices[0].x = -1.0f;
							packetCopy.vertices[0].y = 1.0f;
							packetCopy.vertices[1].x = 1.0f;
							packetCopy.vertices[1].y = 1.0f;
							packetCopy.vertices[2].x = -1.0f;
							packetCopy.vertices[2].y = -1.0f;
							packetCopy.vertices[3].x = 1.0f;
							packetCopy.vertices[3].y = -1.0f;
						}
					}
					if (canonicalizeFinalRtTexrectUv
						&& packetCopy.debugTexrect
						&& packetIsFullscreen(packetCopy)
						&& packetCopy.vertices.size() == 4U) {
						bool sampledRtTexture0 = false;
						bool sampledRtTexture1 = false;
						if ((packetCopy.textureSlotMask & (1U << 0U)) != 0U) {
							const TextureStore::TextureResource * texture0 = m_vk->textureStore.getTexture(packetCopy.textureSlots[0].texture);
							sampledRtTexture0 = texture0 != nullptr && texture0->renderTargetWritten;
						}
						if ((packetCopy.textureSlotMask & (1U << 1U)) != 0U) {
							const TextureStore::TextureResource * texture1 = m_vk->textureStore.getTexture(packetCopy.textureSlots[1].texture);
							sampledRtTexture1 = texture1 != nullptr && texture1->renderTargetWritten;
						}
						if (sampledRtTexture0) {
							packetCopy.vertices[0].s0 = 0.0f;
							packetCopy.vertices[1].s0 = 1.0f;
							packetCopy.vertices[2].s0 = 0.0f;
							packetCopy.vertices[3].s0 = 1.0f;
							const bool flipT0 = (appliedRtTexrectFlipSlotMask & (1U << 0U)) != 0U;
							packetCopy.vertices[0].t0 = flipT0 ? 1.0f : 0.0f;
							packetCopy.vertices[1].t0 = flipT0 ? 1.0f : 0.0f;
							packetCopy.vertices[2].t0 = flipT0 ? 0.0f : 1.0f;
							packetCopy.vertices[3].t0 = flipT0 ? 0.0f : 1.0f;
						}
						if (sampledRtTexture1) {
							packetCopy.vertices[0].s1 = 0.0f;
							packetCopy.vertices[1].s1 = 1.0f;
							packetCopy.vertices[2].s1 = 0.0f;
							packetCopy.vertices[3].s1 = 1.0f;
							const bool flipT1 = (appliedRtTexrectFlipSlotMask & (1U << 1U)) != 0U;
							packetCopy.vertices[0].t1 = flipT1 ? 1.0f : 0.0f;
							packetCopy.vertices[1].t1 = flipT1 ? 1.0f : 0.0f;
							packetCopy.vertices[2].t1 = flipT1 ? 0.0f : 1.0f;
							packetCopy.vertices[3].t1 = flipT1 ? 0.0f : 1.0f;
						}
					}

					// Packets can carry upscaled viewports (for example 2880x2880 on a 1440x1080 swapchain).
					// The present path already uploads clip-space vertices, so use swapchain viewport extents.
				packetCopy.state.raster.viewportX = 0;
				packetCopy.state.raster.viewportY = 0;
				packetCopy.state.raster.viewportWidth = static_cast<s32>(m_vk->swapchainExtent.width);
				packetCopy.state.raster.viewportHeight = static_cast<s32>(m_vk->swapchainExtent.height);
				packetCopy.state.raster.viewportValid = true;
				packetCopy.dirtyMask |= draw_dirty::kViewport;
				static const bool debugDisablePresentScissor = std::getenv("REALITYVK_VK_DEBUG_DISABLE_PRESENT_SCISSOR") != nullptr;
				if (debugDisablePresentScissor) {
					packetCopy.state.raster.scissorEnabled = false;
					packetCopy.dirtyMask |= draw_dirty::kScissor;
				}

				if (enablePresentFallback && isSuspiciousFullscreenSmallTexturePacket(packetCopy)) {
					++skippedSuspiciousPresentPackets;
					continue;
				}

				u32 textureWidth = 0U;
				u32 textureHeight = 0U;
				if (packetTextureDimensions(packetCopy, textureWidth, textureHeight)) {
					const u64 textureArea = static_cast<u64>(textureWidth) * static_cast<u64>(textureHeight);
					if (textureArea * 20ULL >= swapchainArea)
						hasLargeTexturedPacket = true;
				}
				if (debugTraceRtHandle != 0U && debugTraceRtReadLogCount < 256U) {
					for (u32 slot = 0U; slot < 2U; ++slot) {
						if ((packetCopy.textureSlotMask & (1U << slot)) == 0U)
							continue;
						if (static_cast<u32>(packetCopy.textureSlots[slot].texture) != debugTraceRtHandle)
							continue;
						f32 minX = 0.0f;
						f32 minY = 0.0f;
						f32 maxX = 0.0f;
						f32 maxY = 0.0f;
						f32 minS0 = 0.0f;
						f32 maxS0 = 0.0f;
						f32 minT0 = 0.0f;
						f32 maxT0 = 0.0f;
						f32 minS1 = 0.0f;
						f32 maxS1 = 0.0f;
						f32 minT1 = 0.0f;
						f32 maxT1 = 0.0f;
						if (!packetCopy.vertices.empty()) {
							minX = maxX = packetCopy.vertices[0].x;
							minY = maxY = packetCopy.vertices[0].y;
							minS0 = maxS0 = packetCopy.vertices[0].s0;
							minT0 = maxT0 = packetCopy.vertices[0].t0;
							minS1 = maxS1 = packetCopy.vertices[0].s1;
							minT1 = maxT1 = packetCopy.vertices[0].t1;
							for (const DrawVertex & vertex : packetCopy.vertices) {
								minX = std::min(minX, vertex.x);
								maxX = std::max(maxX, vertex.x);
								minY = std::min(minY, vertex.y);
								maxY = std::max(maxY, vertex.y);
								minS0 = std::min(minS0, vertex.s0);
								maxS0 = std::max(maxS0, vertex.s0);
								minT0 = std::min(minT0, vertex.t0);
								maxT0 = std::max(maxT0, vertex.t0);
								minS1 = std::min(minS1, vertex.s1);
								maxS1 = std::max(maxS1, vertex.s1);
								minT1 = std::min(minT1, vertex.t1);
								maxT1 = std::max(maxT1, vertex.t1);
							}
						}
						const DrawVertex * v0 = packetCopy.vertices.size() > 0 ? &packetCopy.vertices[0] : nullptr;
						const DrawVertex * v1 = packetCopy.vertices.size() > 1 ? &packetCopy.vertices[1] : nullptr;
						const DrawVertex * v2 = packetCopy.vertices.size() > 2 ? &packetCopy.vertices[2] : nullptr;
						const DrawVertex * v3 = packetCopy.vertices.size() > 3 ? &packetCopy.vertices[3] : nullptr;
						LOG(
							LOG_WARNING,
							"VK RT trace(read): handle=%u slot=%u packetId=%llu src=%s texrect=%u mode=%s mux=0x%016llx cycle=%u vertices=%u pos=[%.3f,%.3f]-[%.3f,%.3f] tex0=[%.3f,%.3f]-[%.3f,%.3f] tex1=[%.3f,%.3f]-[%.3f,%.3f] v0=[%.3f,%.3f,%.3f,%.3f] v1=[%.3f,%.3f,%.3f,%.3f] v2=[%.3f,%.3f,%.3f,%.3f] v3=[%.3f,%.3f,%.3f,%.3f]",
							debugTraceRtHandle,
							slot,
							static_cast<unsigned long long>(packetCopy.debugPacketId),
							packetSourceName(packetCopy.debugSource),
							packetCopy.debugTexrect ? 1U : 0U,
							transformModeName(packetCopy.transformMode),
							static_cast<unsigned long long>(packetCopy.debugCombinerMux),
							packetCopy.debugCombinerCycleType,
							static_cast<u32>(packetCopy.vertices.size()),
							minX,
							minY,
							maxX,
							maxY,
							minS0,
							minT0,
							maxS0,
							maxT0,
							minS1,
							minT1,
							maxS1,
							maxT1,
							v0 != nullptr ? v0->x : 0.0f,
							v0 != nullptr ? v0->y : 0.0f,
							v0 != nullptr ? v0->s0 : 0.0f,
							v0 != nullptr ? v0->t0 : 0.0f,
							v1 != nullptr ? v1->x : 0.0f,
							v1 != nullptr ? v1->y : 0.0f,
							v1 != nullptr ? v1->s0 : 0.0f,
							v1 != nullptr ? v1->t0 : 0.0f,
							v2 != nullptr ? v2->x : 0.0f,
							v2 != nullptr ? v2->y : 0.0f,
							v2 != nullptr ? v2->s0 : 0.0f,
							v2 != nullptr ? v2->t0 : 0.0f,
							v3 != nullptr ? v3->x : 0.0f,
							v3 != nullptr ? v3->y : 0.0f,
							v3 != nullptr ? v3->s0 : 0.0f,
							v3 != nullptr ? v3->t0 : 0.0f);
						++debugTraceRtReadLogCount;
						break;
					}
				}
				if (debugTraceFinalLayers && debugTraceFinalLayerLogCount < debugTraceFinalLayerLimit) {
					u32 matchedHandle = 0U;
					u32 matchedSlot = 0U;
					bool matchedHandleIsRt = false;
					if (packetMatchesFinalLayerHandle(packetCopy, matchedHandle, matchedSlot, matchedHandleIsRt)) {
						f32 minX = 0.0f;
						f32 minY = 0.0f;
						f32 maxX = 0.0f;
						f32 maxY = 0.0f;
						f32 minS0 = 0.0f;
						f32 maxS0 = 0.0f;
						f32 minT0 = 0.0f;
						f32 maxT0 = 0.0f;
						f32 minS1 = 0.0f;
						f32 maxS1 = 0.0f;
						f32 minT1 = 0.0f;
						f32 maxT1 = 0.0f;
						if (!packetCopy.vertices.empty()) {
							minX = maxX = packetCopy.vertices[0].x;
							minY = maxY = packetCopy.vertices[0].y;
							minS0 = maxS0 = packetCopy.vertices[0].s0;
							minT0 = maxT0 = packetCopy.vertices[0].t0;
							minS1 = maxS1 = packetCopy.vertices[0].s1;
							minT1 = maxT1 = packetCopy.vertices[0].t1;
							for (const DrawVertex & vertex : packetCopy.vertices) {
								minX = std::min(minX, vertex.x);
								maxX = std::max(maxX, vertex.x);
								minY = std::min(minY, vertex.y);
								maxY = std::max(maxY, vertex.y);
								minS0 = std::min(minS0, vertex.s0);
								maxS0 = std::max(maxS0, vertex.s0);
								minT0 = std::min(minT0, vertex.t0);
								maxT0 = std::max(maxT0, vertex.t0);
								minS1 = std::min(minS1, vertex.s1);
								maxS1 = std::max(maxS1, vertex.s1);
								minT1 = std::min(minT1, vertex.t1);
								maxT1 = std::max(maxT1, vertex.t1);
							}
						}
						const bool fullscreenPacket = packetIsFullscreen(packetCopy);
						const char * layerLabel = packetCopy.debugTexrect
							? (fullscreenPacket ? "final_fullscreen_texrect" : "final_partial_texrect")
							: "final_non_texrect";
						LOG(
							LOG_WARNING,
							"VK composite trace(final): label=%s handle=%u slot=%u packetId=%llu src=%s texrect=%u mode=%s fullscreen=%u rtSample=%u flipApplied=%u flipSlots=0x%x strictBlend=%u blendEnabled=%u shaderFlags=0x%08x mux=0x%016llx cycle=%u vertices=%u pos=[%.3f,%.3f]-[%.3f,%.3f] tex0=[%.3f,%.3f]-[%.3f,%.3f] tex1=[%.3f,%.3f]-[%.3f,%.3f] viewport=%d,%d,%d,%d valid=%u scissor=%d,%d,%d,%d enabled=%u",
							layerLabel,
							matchedHandle,
							matchedSlot,
							static_cast<unsigned long long>(packetCopy.debugPacketId),
							packetSourceName(packetCopy.debugSource),
							packetCopy.debugTexrect ? 1U : 0U,
							transformModeName(packetCopy.transformMode),
							fullscreenPacket ? 1U : 0U,
							matchedHandleIsRt ? 1U : 0U,
							appliedRtTexrectFlip ? 1U : 0U,
							appliedRtTexrectFlipSlotMask,
							(packetCopy.shaderFlags & vulkan::draw_shader_flags::kSpecialStrictBlendMux) != 0U ? 1U : 0U,
							packetCopy.state.blend.enabled ? 1U : 0U,
							packetCopy.shaderFlags,
							static_cast<unsigned long long>(packetCopy.debugCombinerMux),
							packetCopy.debugCombinerCycleType,
							static_cast<u32>(packetCopy.vertices.size()),
							minX,
							minY,
							maxX,
							maxY,
							minS0,
							minT0,
							maxS0,
							maxT0,
							minS1,
							minT1,
							maxS1,
							maxT1,
							packetCopy.state.raster.viewportX,
							packetCopy.state.raster.viewportY,
							packetCopy.state.raster.viewportWidth,
							packetCopy.state.raster.viewportHeight,
							packetCopy.state.raster.viewportValid ? 1U : 0U,
							packetCopy.state.raster.scissorX,
							packetCopy.state.raster.scissorY,
							packetCopy.state.raster.scissorWidth,
							packetCopy.state.raster.scissorHeight,
							packetCopy.state.raster.scissorEnabled ? 1U : 0U);
						++debugTraceFinalLayerLogCount;
					}
				}

				totalVertexCount += packetCopy.vertices.size();
				presentPackets.emplace_back(std::move(packetCopy));
			}

			struct PresentFallbackCandidate {
				graphics::ObjectHandle framebuffer = graphics::ObjectHandle::null;
				graphics::ObjectHandle texture = graphics::ObjectHandle::null;
				graphics::TextureTargetParam textureTarget = graphics::textureTarget::TEXTURE_2D;
				u32 width = 0U;
				u32 height = 0U;
				u64 serial = 0U;
			};

			PresentFallbackCandidate fallbackCandidate{};
			auto considerFallbackFramebuffer = [&](graphics::ObjectHandle _framebuffer, u64 _serial) {
				if (!_framebuffer.isNotNull())
					return;
				const u32 colorAttachmentLimit = m_vk->framebufferStore.getDrawBufferCount(_framebuffer);
				const FramebufferStore::FramebufferAttachment * attachment = resolveColorAttachment(
					m_vk->framebufferStore,
					_framebuffer,
					graphics::bufferAttachment::COLOR_ATTACHMENT0,
					nullptr,
					colorAttachmentLimit);
				if (attachment == nullptr || !attachment->textureHandle.isNotNull())
					return;
				const TextureStore::TextureResource * texture = m_vk->textureStore.getTexture(attachment->textureHandle);
				if (texture == nullptr || texture->imageView == VK_NULL_HANDLE || !texture->hasContent)
					return;
				const u32 width = texture->contentWidth != 0U ? texture->contentWidth : texture->width;
				const u32 height = texture->contentHeight != 0U ? texture->contentHeight : texture->height;
				if (width == 0U || height == 0U)
					return;

				const u64 currentArea = static_cast<u64>(width) * static_cast<u64>(height);
				const u64 bestArea = static_cast<u64>(fallbackCandidate.width) * static_cast<u64>(fallbackCandidate.height);
				if (!fallbackCandidate.texture.isNotNull()
					|| _serial > fallbackCandidate.serial
					|| (_serial == fallbackCandidate.serial && currentArea > bestArea)) {
					fallbackCandidate.framebuffer = _framebuffer;
					fallbackCandidate.texture = attachment->textureHandle;
					fallbackCandidate.textureTarget = graphics::TextureTargetParam(static_cast<u32>(attachment->textureTarget));
					fallbackCandidate.width = width;
					fallbackCandidate.height = height;
					fallbackCandidate.serial = _serial;
				}
			};

			considerFallbackFramebuffer(
				m_vk->lastColorBlitDrawFramebuffer,
				m_vk->lastColorBlitDrawFramebufferSerial);
			if (!fallbackCandidate.texture.isNotNull()) {
				considerFallbackFramebuffer(
					m_vk->lastNonDefaultReadFramebuffer,
					m_vk->lastNonDefaultReadFramebufferSerial);
			}
			if (!fallbackCandidate.texture.isNotNull()) {
				considerFallbackFramebuffer(
					m_vk->lastNonDefaultDrawFramebuffer,
					m_vk->lastNonDefaultDrawFramebufferSerial);
			}

			bool fallbackInjected = false;
			const bool shouldInjectFallback = enablePresentFallback
				&& fallbackCandidate.texture.isNotNull()
				&& (presentPackets.empty() || skippedSuspiciousPresentPackets > 0U || !hasLargeTexturedPacket);
			if (shouldInjectFallback) {
				DrawPacket fallbackPacket{};
				fallbackPacket.primitive = PrimitiveType::TriangleStrip;
				fallbackPacket.transformMode = VertexTransformMode::Rect;
				fallbackPacket.textureUnit0 = 0U;
				fallbackPacket.textureUnit1 = 1U;
				fallbackPacket.shaderFlags = vulkan::draw_shader_flags::kTexture0;
				fallbackPacket.positionsNormalized = true;
				fallbackPacket.forceRasterRectTransform = false;
				assignPacketDebugMetadata(fallbackPacket, kPacketSourceFallbackFbo, nullptr, true);
				fallbackPacket.textureSlotMask = (1U << 0);
				fallbackPacket.textureSlots[0].unit = 0U;
				fallbackPacket.textureSlots[0].texture = fallbackCandidate.texture;
				fallbackPacket.textureSlots[0].target = fallbackCandidate.textureTarget;
				fallbackPacket.state.raster.viewportX = 0;
				fallbackPacket.state.raster.viewportY = 0;
				fallbackPacket.state.raster.viewportWidth = static_cast<s32>(m_vk->swapchainExtent.width);
				fallbackPacket.state.raster.viewportHeight = static_cast<s32>(m_vk->swapchainExtent.height);
				fallbackPacket.state.raster.viewportValid = true;
				fallbackPacket.state.raster.scissorEnabled = false;
				fallbackPacket.state.depth.testEnabled = false;
				fallbackPacket.state.depth.writeEnabled = false;
				fallbackPacket.state.depth.compare = CompareMode::kAlways;
				fallbackPacket.state.blend.enabled = false;
				fallbackPacket.state.cullMode = CullMode::kNone;
				fallbackPacket.dirtyMask = draw_dirty::kAll;
				fallbackPacket.vertices.reserve(4U);
				auto addFallbackVertex = [&fallbackPacket](f32 _x, f32 _y, f32 _s, f32 _t) {
					DrawVertex vertex{};
					vertex.x = _x;
					vertex.y = _y;
					vertex.z = 0.0f;
					vertex.w = 1.0f;
					vertex.r = 1.0f;
					vertex.g = 1.0f;
					vertex.b = 1.0f;
					vertex.a = 1.0f;
					vertex.s0 = _s;
					vertex.t0 = _t;
					vertex.s1 = _s;
					vertex.t1 = _t;
					fallbackPacket.vertices.push_back(vertex);
				};
				// Offscreen color attachments are stored upside-down relative to clip-space Y.
				addFallbackVertex(-1.0f, -1.0f, 0.0f, 1.0f);
				addFallbackVertex(1.0f, -1.0f, 1.0f, 1.0f);
				addFallbackVertex(-1.0f, 1.0f, 0.0f, 0.0f);
				addFallbackVertex(1.0f, 1.0f, 1.0f, 0.0f);

				const bool singleFullscreenPacket = presentPackets.size() == 1U && packetIsFullscreen(presentPackets[0]);
				if (singleFullscreenPacket)
					presentPackets.emplace_back(std::move(fallbackPacket));
				else
					presentPackets.insert(presentPackets.begin(), std::move(fallbackPacket));
				totalVertexCount += 4U;
				fallbackInjected = true;
			}

			const bool hasDrawPackets = totalVertexCount > 0;
			if (isVkFboTraceEnabled()) {
				vkFboTrace(
					"op=present packets=%u vertices=%u skippedOffscreenDraws=%llu skippedOffscreenVertices=%llu skippedSuspicious=%u fallbackInjected=%u fallbackFbo=%u fallbackTex=%u fallbackSize=%ux%u",
					static_cast<u32>(presentPackets.size()),
					static_cast<u32>(totalVertexCount),
					static_cast<unsigned long long>(offscreenSkippedDrawCalls()),
					static_cast<unsigned long long>(offscreenSkippedVertices()),
					skippedSuspiciousPresentPackets,
					fallbackInjected ? 1U : 0U,
					static_cast<u32>(fallbackCandidate.framebuffer),
					static_cast<u32>(fallbackCandidate.texture),
					fallbackCandidate.width,
					fallbackCandidate.height);
			}
				static const bool debugPresent = std::getenv("REALITYVK_VK_DEBUG_PRESENT") != nullptr;
				static const bool debugPresentPackets = std::getenv("REALITYVK_VK_DEBUG_PRESENT_PACKETS") != nullptr;
				if (debugPresent) {
				f32 minX = 0.0f;
				f32 minY = 0.0f;
				f32 maxX = 0.0f;
				f32 maxY = 0.0f;
				f32 minR = 0.0f;
				f32 minG = 0.0f;
				f32 minB = 0.0f;
				f32 minA = 0.0f;
				f32 maxR = 0.0f;
				f32 maxG = 0.0f;
				f32 maxB = 0.0f;
				f32 maxA = 0.0f;
				f32 minS0 = 0.0f;
				f32 minT0 = 0.0f;
				f32 maxS0 = 0.0f;
				f32 maxT0 = 0.0f;
				f32 minS1 = 0.0f;
				f32 minT1 = 0.0f;
				f32 maxS1 = 0.0f;
				f32 maxT1 = 0.0f;
				f32 minW = 0.0f;
				f32 maxW = 0.0f;
				f32 sampleW = 0.0f;
				f32 sampleZ = 0.0f;
				f32 minXOverW = 0.0f;
				f32 maxXOverW = 0.0f;
				f32 minYOverW = 0.0f;
				f32 maxYOverW = 0.0f;
				bool hasAnyVertex = false;

					for (const DrawPacket & packet : presentPackets) {
						for (const DrawVertex & vertex : packet.vertices) {
						const f32 invW = std::abs(vertex.w) > 1.0e-6f ? (1.0f / vertex.w) : 0.0f;
						const f32 xOverW = vertex.x * invW;
						const f32 yOverW = vertex.y * invW;
						if (!hasAnyVertex) {
							minX = maxX = vertex.x;
							minY = maxY = vertex.y;
							minR = maxR = vertex.r;
							minG = maxG = vertex.g;
							minB = maxB = vertex.b;
							minA = maxA = vertex.a;
							minS0 = maxS0 = vertex.s0;
							minT0 = maxT0 = vertex.t0;
							minS1 = maxS1 = vertex.s1;
							minT1 = maxT1 = vertex.t1;
							minW = maxW = vertex.w;
							sampleW = vertex.w;
							sampleZ = vertex.z;
							minXOverW = maxXOverW = xOverW;
							minYOverW = maxYOverW = yOverW;
							hasAnyVertex = true;
							continue;
						}
						minX = std::min(minX, vertex.x);
						minY = std::min(minY, vertex.y);
						maxX = std::max(maxX, vertex.x);
						maxY = std::max(maxY, vertex.y);
						minR = std::min(minR, vertex.r);
						minG = std::min(minG, vertex.g);
						minB = std::min(minB, vertex.b);
						minA = std::min(minA, vertex.a);
						maxR = std::max(maxR, vertex.r);
						maxG = std::max(maxG, vertex.g);
						maxB = std::max(maxB, vertex.b);
						maxA = std::max(maxA, vertex.a);
						minS0 = std::min(minS0, vertex.s0);
						minT0 = std::min(minT0, vertex.t0);
						maxS0 = std::max(maxS0, vertex.s0);
						maxT0 = std::max(maxT0, vertex.t0);
						minS1 = std::min(minS1, vertex.s1);
						minT1 = std::min(minT1, vertex.t1);
						maxS1 = std::max(maxS1, vertex.s1);
						maxT1 = std::max(maxT1, vertex.t1);
						minW = std::min(minW, vertex.w);
						maxW = std::max(maxW, vertex.w);
						minXOverW = std::min(minXOverW, xOverW);
						maxXOverW = std::max(maxXOverW, xOverW);
						minYOverW = std::min(minYOverW, yOverW);
						maxYOverW = std::max(maxYOverW, yOverW);
						}
					}
					if (debugPresentPackets) {
						static const u32 presentPacketLogLimit = []() -> u32 {
							const char * env = std::getenv("REALITYVK_VK_DEBUG_PRESENT_PACKET_LIMIT");
							if (env == nullptr || env[0] == '\0')
								return 128U;
							return static_cast<u32>(std::strtoul(env, nullptr, 10));
						}();
						static u32 presentPacketLogCount = 0U;
						for (const DrawPacket & packet : presentPackets) {
							if (presentPacketLogCount >= presentPacketLogLimit)
								break;
							if (packet.vertices.empty())
								continue;
							f32 pMinX = packet.vertices[0].x;
							f32 pMinY = packet.vertices[0].y;
							f32 pMaxX = pMinX;
							f32 pMaxY = pMinY;
							f32 pMinS0 = packet.vertices[0].s0;
							f32 pMinT0 = packet.vertices[0].t0;
							f32 pMaxS0 = pMinS0;
							f32 pMaxT0 = pMinT0;
							f32 pMinR = packet.vertices[0].r;
							f32 pMinG = packet.vertices[0].g;
							f32 pMinB = packet.vertices[0].b;
							f32 pMinA = packet.vertices[0].a;
							f32 pMaxR = pMinR;
							f32 pMaxG = pMinG;
							f32 pMaxB = pMinB;
							f32 pMaxA = pMinA;
							for (const DrawVertex & vertex : packet.vertices) {
								pMinX = std::min(pMinX, vertex.x);
								pMinY = std::min(pMinY, vertex.y);
								pMaxX = std::max(pMaxX, vertex.x);
								pMaxY = std::max(pMaxY, vertex.y);
								pMinS0 = std::min(pMinS0, vertex.s0);
								pMinT0 = std::min(pMinT0, vertex.t0);
								pMaxS0 = std::max(pMaxS0, vertex.s0);
								pMaxT0 = std::max(pMaxT0, vertex.t0);
								pMinR = std::min(pMinR, vertex.r);
								pMinG = std::min(pMinG, vertex.g);
								pMinB = std::min(pMinB, vertex.b);
								pMinA = std::min(pMinA, vertex.a);
								pMaxR = std::max(pMaxR, vertex.r);
								pMaxG = std::max(pMaxG, vertex.g);
								pMaxB = std::max(pMaxB, vertex.b);
								pMaxA = std::max(pMaxA, vertex.a);
							}
							LOG(
								LOG_WARNING,
								"VK present packet debug: id=%llu src=%s texrect=%u mux=0x%016llx cycle=%u vertices=%u shaderFlags=0x%08x texMask=0x%08x handle0=%u handle1=%u pos=[%.3f,%.3f]-[%.3f,%.3f] color=[%.3f,%.3f,%.3f,%.3f]-[%.3f,%.3f,%.3f,%.3f] tex0=[%.3f,%.3f]-[%.3f,%.3f]",
								static_cast<unsigned long long>(packet.debugPacketId),
								packetSourceName(packet.debugSource),
								packet.debugTexrect ? 1U : 0U,
								static_cast<unsigned long long>(packet.debugCombinerMux),
								packet.debugCombinerCycleType,
								static_cast<u32>(packet.vertices.size()),
								packet.shaderFlags,
								packet.textureSlotMask,
								(packet.textureSlotMask & (1U << 0)) != 0U ? static_cast<u32>(packet.textureSlots[0].texture) : 0U,
								(packet.textureSlotMask & (1U << 1)) != 0U ? static_cast<u32>(packet.textureSlots[1].texture) : 0U,
								pMinX,
								pMinY,
								pMaxX,
								pMaxY,
								pMinR,
								pMinG,
								pMinB,
								pMinA,
								pMaxR,
								pMaxG,
								pMaxB,
								pMaxA,
								pMinS0,
								pMinT0,
								pMaxS0,
								pMaxT0);
							++presentPacketLogCount;
						}
					}

					LOG(
						LOG_WARNING,
					"VK present debug: packets=%u vertices=%zu hasDraw=%u bounds=[%.2f,%.2f]-[%.2f,%.2f] tex0=[%.3f,%.3f]-[%.3f,%.3f] tex1=[%.3f,%.3f]-[%.3f,%.3f] color=[%.3f,%.3f,%.3f,%.3f]-[%.3f,%.3f,%.3f,%.3f] w=[%.4f,%.4f] ndc=[%.3f,%.3f]-[%.3f,%.3f] sampleW=%.4f sampleZ=%.4f swap=%ux%u",
					static_cast<unsigned>(presentPackets.size()),
					totalVertexCount,
					hasDrawPackets ? 1U : 0U,
					hasAnyVertex ? minX : 0.0f,
					hasAnyVertex ? minY : 0.0f,
					hasAnyVertex ? maxX : 0.0f,
					hasAnyVertex ? maxY : 0.0f,
					hasAnyVertex ? minS0 : 0.0f,
					hasAnyVertex ? minT0 : 0.0f,
					hasAnyVertex ? maxS0 : 0.0f,
					hasAnyVertex ? maxT0 : 0.0f,
					hasAnyVertex ? minS1 : 0.0f,
					hasAnyVertex ? minT1 : 0.0f,
					hasAnyVertex ? maxS1 : 0.0f,
					hasAnyVertex ? maxT1 : 0.0f,
					hasAnyVertex ? minR : 0.0f,
					hasAnyVertex ? minG : 0.0f,
					hasAnyVertex ? minB : 0.0f,
					hasAnyVertex ? minA : 0.0f,
					hasAnyVertex ? maxR : 0.0f,
					hasAnyVertex ? maxG : 0.0f,
					hasAnyVertex ? maxB : 0.0f,
					hasAnyVertex ? maxA : 0.0f,
					hasAnyVertex ? minW : 0.0f,
					hasAnyVertex ? maxW : 0.0f,
					hasAnyVertex ? minXOverW : 0.0f,
					hasAnyVertex ? minYOverW : 0.0f,
					hasAnyVertex ? maxXOverW : 0.0f,
					hasAnyVertex ? maxYOverW : 0.0f,
					hasAnyVertex ? sampleW : 0.0f,
					hasAnyVertex ? sampleZ : 0.0f,
					m_vk->swapchainExtent.width,
					m_vk->swapchainExtent.height);
			}

			UploadArena::Allocation uploadAllocation{};
			if (hasDrawPackets) {
				const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(totalVertexCount * sizeof(DrawVertex));
			m_vk->uploadArena.beginFrame(m_vk->frameSyncIndex);
			if (!m_vk->uploadArena.allocate(
				m_vk->frameSyncIndex,
				vertexBytes,
				static_cast<VkDeviceSize>(alignof(DrawVertex)),
				uploadAllocation)) {
				LOG(LOG_WARNING, "Failed to allocate Vulkan upload arena range.");
				break;
			}
			if (uploadAllocation.buffer == VK_NULL_HANDLE || uploadAllocation.mapped == nullptr) {
				LOG(LOG_WARNING, "Vulkan upload arena returned an invalid allocation.");
				break;
			}

				DrawVertex * dst = static_cast<DrawVertex *>(uploadAllocation.mapped);
				for (const DrawPacket & packet : presentPackets) {
					if (packet.vertices.empty())
						continue;
					for (const DrawVertex & vertex : packet.vertices) {
						*dst = vertex;
						++dst;
					}
				}
		}

		VkCommandBufferBeginInfo commandBufferBeginInfo{};
		commandBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		commandBufferBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (vkBeginCommandBuffer(commandBuffer, &commandBufferBeginInfo) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkBeginCommandBuffer failed.");
			break;
		}

		const std::array<f32, 4> & clearColor = m_vk->drawRecorder.clearColor();
		VkClearValue clearValues[2]{};
		clearValues[0].color.float32[0] = clearColor[0];
		clearValues[0].color.float32[1] = clearColor[1];
		clearValues[0].color.float32[2] = clearColor[2];
		clearValues[0].color.float32[3] = clearColor[3];
		clearValues[1].depthStencil.depth = m_vk->drawRecorder.clearDepth();
		clearValues[1].depthStencil.stencil = 0;
		VkRenderPassBeginInfo renderPassBeginInfo{};
		renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		renderPassBeginInfo.renderPass = m_vk->renderPass;
		renderPassBeginInfo.framebuffer = m_vk->swapchainFramebuffers[currentImageIndex];
		renderPassBeginInfo.renderArea.offset = { 0, 0 };
		renderPassBeginInfo.renderArea.extent = m_vk->swapchainExtent;
		renderPassBeginInfo.clearValueCount = 2;
		renderPassBeginInfo.pClearValues = clearValues;
		vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

			if (hasDrawPackets) {
				m_vk->descriptorBinder.beginFrame(m_vk->frameSyncIndex);
				DrawCommandEncoder::EncodeInfo drawInfo{};
			drawInfo.commandBuffer = commandBuffer;
			drawInfo.vertexBuffer = uploadAllocation.buffer;
			drawInfo.vertexBufferOffset = uploadAllocation.offset;
				drawInfo.packets = &presentPackets;
			drawInfo.pipelineCache = &m_vk->pipelineCache;
			drawInfo.descriptorBinder = &m_vk->descriptorBinder;
			drawInfo.pipelineLayout = m_vk->programLibrary.getBasicColorPipelineLayout();
			drawInfo.renderExtent = m_vk->swapchainExtent;
			drawInfo.wideLinesEnabled = m_vk->wideLinesEnabled;
			drawInfo.maxLineWidth = m_maxLineWidth;
			DrawCommandEncoder::encode(drawInfo);
		}

		vkCmdEndRenderPass(commandBuffer);
		if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkEndCommandBuffer failed.");
			break;
		}

		if (vkResetFences(m_vk->device, 1, &currentFrame.inFlight) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkResetFences failed.");
			break;
		}
		imageFence = currentFrame.inFlight;

		VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = &currentFrame.imageAvailable;
		submitInfo.pWaitDstStageMask = &waitStage;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = &currentFrame.renderFinished;

		const VkResult submitResult = vkQueueSubmit(m_vk->graphicsQueue, 1, &submitInfo, currentFrame.inFlight);
		if (submitResult != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkQueueSubmit failed: %d", static_cast<int>(submitResult));
			break;
		}

		VkPresentInfoKHR presentInfo{};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = &currentFrame.renderFinished;
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = &m_vk->swapchain;
		presentInfo.pImageIndices = &currentImageIndex;

		const VkResult presentResult = vkQueuePresentKHR(m_vk->presentQueue, &presentInfo);
		if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
			vkDeviceWaitIdle(m_vk->device);
			destroySwapchain();
			createSwapchain();
			break;
		}
		if (presentResult != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkQueuePresentKHR failed: %d", static_cast<int>(presentResult));
			break;
		}

		m_vk->lastPresentedImageIndex = currentImageIndex;
		m_vk->frameSyncIndex = (m_vk->frameSyncIndex + 1U) % static_cast<u32>(m_vk->frameSync.size());
		success = true;
	} while (false);

	resetFrameRenderData();
	return success;
#endif
}

bool ContextImpl::isSupported(graphics::SpecialFeatures _feature) const
{
	static const bool experimentalFetchBlendAll = std::getenv("REALITYVK_VK_EXPERIMENTAL_FETCH_BLEND") != nullptr;
	static const bool experimentalFetchDepth = experimentalFetchBlendAll
		|| std::getenv("REALITYVK_VK_EXPERIMENTAL_FB_FETCH_DEPTH") != nullptr;
	static const bool experimentalFetchColor = experimentalFetchBlendAll
		|| std::getenv("REALITYVK_VK_EXPERIMENTAL_FB_FETCH_COLOR") != nullptr;
	static const bool experimentalDualSource = experimentalFetchBlendAll
		|| std::getenv("REALITYVK_VK_EXPERIMENTAL_DUAL_SOURCE") != nullptr;
	switch (_feature) {
	case graphics::SpecialFeatures::Multisampling:
		return m_maxMsaaLevel > 1;
	case graphics::SpecialFeatures::TextureBarrier:
	case graphics::SpecialFeatures::BlitFramebuffer:
	case graphics::SpecialFeatures::ShaderProgramBinary:
	case graphics::SpecialFeatures::WeakBlitFramebuffer:
	case graphics::SpecialFeatures::ImageTextures:
		return m_coreReady;
	case graphics::SpecialFeatures::N64DepthWithFbFetchDepth:
		return m_coreReady && experimentalFetchDepth;
	case graphics::SpecialFeatures::FramebufferFetchColor:
		return m_coreReady && experimentalFetchColor;
	case graphics::SpecialFeatures::DualSourceBlending:
		// Dual-source factors require backend support and device feature enablement.
		return m_coreReady && experimentalDualSource && m_vk != nullptr && m_vk->dualSrcBlendEnabled;
	case graphics::SpecialFeatures::EglImage:
	case graphics::SpecialFeatures::EglImageFramebuffer:
		return false;
	case graphics::SpecialFeatures::DepthFramebufferTextures:
	case graphics::SpecialFeatures::IntegerTextures:
		return m_coreReady;
	}
	return false;
}

s32 ContextImpl::getMaxMSAALevel()
{
	return m_maxMsaaLevel;
}

bool ContextImpl::isError() const
{
	if (!m_coreReady || !m_vk)
		return true;
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	return m_vk->instance == VK_NULL_HANDLE
		|| m_vk->physicalDevice == VK_NULL_HANDLE
		|| m_vk->device == VK_NULL_HANDLE
		|| m_vk->graphicsQueue == VK_NULL_HANDLE;
#else
	return true;
#endif
}

bool ContextImpl::isFramebufferError() const
{
	if (!m_coreReady || !m_vk)
		return true;
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (m_vk->renderPass == VK_NULL_HANDLE)
		return true;
	const graphics::ObjectHandle drawFramebuffer = m_vk->drawRecorder.drawFramebufferBinding();
	if (drawFramebuffer == graphics::ObjectHandle::defaultFramebuffer) {
		return m_vk->swapchain == VK_NULL_HANDLE
			|| m_vk->swapchainFramebuffers.empty()
			|| m_vk->swapchainExtent.width == 0U
			|| m_vk->swapchainExtent.height == 0U;
	}

	const FramebufferStore::FramebufferResource * framebuffer = m_vk->framebufferStore.getFramebuffer(drawFramebuffer);
	if (framebuffer == nullptr)
		return true;

	const u32 colorAttachmentLimit = m_vk->framebufferStore.getDrawBufferCount(drawFramebuffer);
	const FramebufferStore::FramebufferAttachment * colorAttachment = resolveColorAttachment(
		m_vk->framebufferStore,
		drawFramebuffer,
		graphics::bufferAttachment::COLOR_ATTACHMENT0,
		nullptr,
		colorAttachmentLimit);
	if (colorAttachment != nullptr && colorAttachment->textureHandle.isNotNull()) {
		const TextureStore::TextureResource * colorTexture = m_vk->textureStore.getTexture(colorAttachment->textureHandle);
		if (colorTexture == nullptr || colorTexture->imageView == VK_NULL_HANDLE)
			return true;
		return false;
	}

	const FramebufferStore::FramebufferAttachment * depthAttachment = m_vk->framebufferStore.getAttachment(
		drawFramebuffer,
		graphics::bufferAttachment::DEPTH_ATTACHMENT);
	if (depthAttachment != nullptr && depthAttachment->textureHandle.isNotNull()) {
		const TextureStore::TextureResource * depthTexture = m_vk->textureStore.getTexture(depthAttachment->textureHandle);
		return depthTexture == nullptr || depthTexture->imageView == VK_NULL_HANDLE;
	}
	return true;
#else
	return true;
#endif
}

void ContextImpl::initFramebufferFormats()
{
	m_fbTexFormats.reset(new graphics::FramebufferTextureFormats);
	m_fbTexFormats->colorInternalFormat = graphics::internalcolorFormat::RGBA8;
	m_fbTexFormats->colorFormat = graphics::colorFormat::RGBA;
	m_fbTexFormats->colorType = graphics::datatype::UNSIGNED_BYTE;
	m_fbTexFormats->colorFormatBytes = 4;

	m_fbTexFormats->monochromeInternalFormat = graphics::internalcolorFormat::LUMINANCE;
	m_fbTexFormats->monochromeFormat = graphics::colorFormat::RED;
	m_fbTexFormats->monochromeType = graphics::datatype::UNSIGNED_BYTE;
	m_fbTexFormats->monochromeFormatBytes = 1;

	m_fbTexFormats->depthInternalFormat = graphics::internalcolorFormat::DEPTH;
	m_fbTexFormats->depthFormat = graphics::colorFormat::DEPTH;
	m_fbTexFormats->depthType = graphics::datatype::FLOAT;
	m_fbTexFormats->depthFormatBytes = 4;

	m_fbTexFormats->depthImageInternalFormat = graphics::internalcolorFormat::R16F;
	m_fbTexFormats->depthImageFormat = graphics::colorFormat::RED;
	m_fbTexFormats->depthImageType = graphics::datatype::FLOAT;
	m_fbTexFormats->depthImageFormatBytes = 4;

	m_fbTexFormats->lutInternalFormat = graphics::internalcolorFormat::COLOR_INDEX8;
	m_fbTexFormats->lutFormat = graphics::colorFormat::RED;
	m_fbTexFormats->lutType = graphics::datatype::UNSIGNED_INT;
	m_fbTexFormats->lutFormatBytes = 4;

	m_fbTexFormats->fontInternalFormat = graphics::internalcolorFormat::LUMINANCE;
	m_fbTexFormats->fontFormat = graphics::colorFormat::RED;
	m_fbTexFormats->fontType = graphics::datatype::UNSIGNED_BYTE;
	m_fbTexFormats->fontFormatBytes = 1;
}

bool ContextImpl::initializeVulkanCore()
{
	if (!hasVulkanSupport())
		return false;
	if (!createInstance())
		return false;
	if (!createSurface()) {
		const bool hasPresentationTarget = m_presentationWindowInfo.system != graphics::Context::PresentationWindowInfo::WindowSystem::Unknown
			&& m_presentationWindowInfo.window != 0;
		if (hasPresentationTarget) {
			LOG(LOG_WARNING, "Vulkan presentation target was provided, but surface creation failed.");
		}
	}
	if (!selectPhysicalDevice())
		return false;
	if (!createDeviceAndQueue())
		return false;
	createSwapchain();
	m_coreReady = true;
	return true;
}

bool ContextImpl::createInstance()
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk)
		m_vk.reset(new VulkanState);

	m_vk->surfaceExtensionEnabled = false;
	m_vk->xlibSurfaceExtensionEnabled = false;
	m_vk->win32SurfaceExtensionEnabled = false;
	m_vk->swapchainExtensionEnabled = false;

	u32 instanceExtensionCount = 0;
	std::vector<VkExtensionProperties> instanceExtensions;
	if (vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr) == VK_SUCCESS && instanceExtensionCount > 0U) {
		instanceExtensions.resize(instanceExtensionCount);
		if (vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, instanceExtensions.data()) != VK_SUCCESS) {
			instanceExtensions.clear();
		}
	}

	auto hasInstanceExtension = [&instanceExtensions](const char * _extensionName) {
		for (const auto & extension : instanceExtensions) {
			if (strcmp(extension.extensionName, _extensionName) == 0)
				return true;
		}
		return false;
	};

	std::vector<const char *> enabledInstanceExtensions;
	auto addEnabledInstanceExtension = [&enabledInstanceExtensions](const char * _extensionName) {
		for (const char * current : enabledInstanceExtensions) {
			if (strcmp(current, _extensionName) == 0)
				return;
		}
		enabledInstanceExtensions.push_back(_extensionName);
	};

	bool gotVidExtInstanceExtensions = false;
#ifdef MUPENPLUSAPI
	if (CoreVideo_VK_GetInstanceExtensions != nullptr) {
		const char ** vidExtExtensions = nullptr;
		uint32_t vidExtExtensionCount = 0;
		const m64p_error status = CoreVideo_VK_GetInstanceExtensions(&vidExtExtensions, &vidExtExtensionCount);
		if (status == M64ERR_SUCCESS && vidExtExtensions != nullptr && vidExtExtensionCount > 0U) {
			gotVidExtInstanceExtensions = true;
			for (uint32_t i = 0; i < vidExtExtensionCount; ++i) {
				const char * extensionName = vidExtExtensions[i];
				if (extensionName == nullptr || extensionName[0] == '\0')
					continue;
				if (!hasInstanceExtension(extensionName)) {
					LOG(LOG_WARNING, "Vulkan extension %s requested by core vidext is unavailable.", extensionName);
					return false;
				}
				addEnabledInstanceExtension(extensionName);
				if (strcmp(extensionName, VK_KHR_SURFACE_EXTENSION_NAME) == 0)
					m_vk->surfaceExtensionEnabled = true;
#if defined(OS_LINUX)
				if (strcmp(extensionName, VK_KHR_XLIB_SURFACE_EXTENSION_NAME) == 0)
					m_vk->xlibSurfaceExtensionEnabled = true;
#endif
#if defined(OS_WINDOWS)
				if (strcmp(extensionName, VK_KHR_WIN32_SURFACE_EXTENSION_NAME) == 0)
					m_vk->win32SurfaceExtensionEnabled = true;
#endif
			}
		} else if (status != M64ERR_SUCCESS) {
			LOG(LOG_WARNING, "VidExt_VK_GetInstanceExtensions failed with error code: %d", status);
		}
	}
#endif

	if (!gotVidExtInstanceExtensions) {
		const bool hasPresentationTarget = m_presentationWindowInfo.system != graphics::Context::PresentationWindowInfo::WindowSystem::Unknown
			&& m_presentationWindowInfo.window != 0;
		if (hasPresentationTarget) {
			bool platformExtensionReady = false;

			switch (m_presentationWindowInfo.system) {
#if defined(OS_LINUX)
			case graphics::Context::PresentationWindowInfo::WindowSystem::Xlib:
				if (hasInstanceExtension(VK_KHR_XLIB_SURFACE_EXTENSION_NAME)) {
					addEnabledInstanceExtension(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
					m_vk->xlibSurfaceExtensionEnabled = true;
					platformExtensionReady = true;
				} else {
					LOG(LOG_WARNING, "Vulkan extension %s is unavailable; Xlib surface creation is disabled.", VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
				}
				break;
#endif
#if defined(OS_WINDOWS)
			case graphics::Context::PresentationWindowInfo::WindowSystem::Win32:
				if (hasInstanceExtension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) {
					addEnabledInstanceExtension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
					m_vk->win32SurfaceExtensionEnabled = true;
					platformExtensionReady = true;
				} else {
					LOG(LOG_WARNING, "Vulkan extension %s is unavailable; Win32 surface creation is disabled.", VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
				}
				break;
#endif
			case graphics::Context::PresentationWindowInfo::WindowSystem::Unknown:
			default:
				break;
			}

			if (platformExtensionReady) {
				if (hasInstanceExtension(VK_KHR_SURFACE_EXTENSION_NAME)) {
					addEnabledInstanceExtension(VK_KHR_SURFACE_EXTENSION_NAME);
					m_vk->surfaceExtensionEnabled = true;
				} else {
					LOG(LOG_WARNING, "Vulkan extension %s is unavailable; surface creation is disabled.", VK_KHR_SURFACE_EXTENSION_NAME);
					m_vk->xlibSurfaceExtensionEnabled = false;
					m_vk->win32SurfaceExtensionEnabled = false;
					enabledInstanceExtensions.clear();
				}
			}
		}
	}

	VkApplicationInfo appInfo{};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "RealityVK Vulkan";
	appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
	appInfo.pEngineName = "RealityVK";
	appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
	appInfo.apiVersion = VK_API_VERSION_1_1;

	VkInstanceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;
	createInfo.enabledExtensionCount = static_cast<u32>(enabledInstanceExtensions.size());
	createInfo.ppEnabledExtensionNames = enabledInstanceExtensions.empty() ? nullptr : enabledInstanceExtensions.data();

	const VkResult result = vkCreateInstance(&createInfo, nullptr, &m_vk->instance);
	if (result != VK_SUCCESS) {
		LOG(LOG_WARNING, "vkCreateInstance failed: %d", static_cast<int>(result));
		return false;
	}
	return true;
#endif
}

bool ContextImpl::createSurface()
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk || m_vk->instance == VK_NULL_HANDLE)
		return false;
	if (m_vk->surface != VK_NULL_HANDLE)
		return true;

#ifdef MUPENPLUSAPI
	if (CoreVideo_VK_GetSurface != nullptr) {
		void * surfaceHandle = nullptr;
		void * instanceHandle = reinterpret_cast<void *>(m_vk->instance);
		const m64p_error status = CoreVideo_VK_GetSurface(&surfaceHandle, instanceHandle);
		if (status == M64ERR_SUCCESS && surfaceHandle != nullptr) {
			m_vk->surface = reinterpret_cast<VkSurfaceKHR>(surfaceHandle);
			return true;
		}
		if (status != M64ERR_SUCCESS) {
			LOG(LOG_WARNING, "VidExt_VK_GetSurface failed with error code: %d", status);
		} else {
			LOG(LOG_WARNING, "VidExt_VK_GetSurface returned a null Vulkan surface.");
		}
	}
#endif

	switch (m_presentationWindowInfo.system) {
#if defined(OS_LINUX)
	case graphics::Context::PresentationWindowInfo::WindowSystem::Xlib:
		if (!m_vk->surfaceExtensionEnabled || !m_vk->xlibSurfaceExtensionEnabled)
			return false;
		if (m_presentationWindowInfo.display == nullptr || m_presentationWindowInfo.window == 0)
			return false;
	{
		VkXlibSurfaceCreateInfoKHR createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
		createInfo.dpy = reinterpret_cast<Display *>(m_presentationWindowInfo.display);
		createInfo.window = static_cast<Window>(m_presentationWindowInfo.window);

		const VkResult result = vkCreateXlibSurfaceKHR(m_vk->instance, &createInfo, nullptr, &m_vk->surface);
		if (result != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkCreateXlibSurfaceKHR failed: %d", static_cast<int>(result));
			return false;
		}
		return true;
	}
#endif
#if defined(OS_WINDOWS)
	case graphics::Context::PresentationWindowInfo::WindowSystem::Win32:
		if (!m_vk->surfaceExtensionEnabled || !m_vk->win32SurfaceExtensionEnabled)
			return false;
		if (m_presentationWindowInfo.display == nullptr || m_presentationWindowInfo.window == 0)
			return false;
	{
		VkWin32SurfaceCreateInfoKHR createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
		createInfo.hinstance = reinterpret_cast<HINSTANCE>(m_presentationWindowInfo.display);
		createInfo.hwnd = reinterpret_cast<HWND>(m_presentationWindowInfo.window);

		const VkResult result = vkCreateWin32SurfaceKHR(m_vk->instance, &createInfo, nullptr, &m_vk->surface);
		if (result != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkCreateWin32SurfaceKHR failed: %d", static_cast<int>(result));
			return false;
		}
		return true;
	}
#else
	case graphics::Context::PresentationWindowInfo::WindowSystem::Win32:
		return false;
#endif
	case graphics::Context::PresentationWindowInfo::WindowSystem::Unknown:
	default:
		return false;
	}
#endif
}

bool ContextImpl::selectPhysicalDevice()
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk || m_vk->instance == VK_NULL_HANDLE)
		return false;

	m_vk->physicalDevice = VK_NULL_HANDLE;
	m_vk->graphicsQueueFamily = UINT32_MAX;
	m_vk->presentQueueFamily = UINT32_MAX;

	u32 deviceCount = 0;
	if (vkEnumeratePhysicalDevices(m_vk->instance, &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0U) {
		LOG(LOG_WARNING, "Vulkan physical device enumeration failed or no device found.");
		return false;
	}

	std::vector<VkPhysicalDevice> devices(deviceCount, VK_NULL_HANDLE);
	if (vkEnumeratePhysicalDevices(m_vk->instance, &deviceCount, devices.data()) != VK_SUCCESS) {
		LOG(LOG_WARNING, "Vulkan physical device enumeration failed.");
		return false;
	}

	for (VkPhysicalDevice device : devices) {
		u32 queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
		if (queueFamilyCount == 0U)
			continue;

		std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());
		s32 graphicsQueueFamily = -1;
		s32 presentQueueFamily = -1;

		for (u32 i = 0; i < queueFamilyCount; ++i) {
			if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && graphicsQueueFamily < 0) {
				graphicsQueueFamily = static_cast<s32>(i);
			}

			if (m_vk->surface != VK_NULL_HANDLE && presentQueueFamily < 0) {
				VkBool32 presentSupported = VK_FALSE;
				if (vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_vk->surface, &presentSupported) == VK_SUCCESS && presentSupported == VK_TRUE) {
					presentQueueFamily = static_cast<s32>(i);
				}
			}
		}

		if (graphicsQueueFamily < 0)
			continue;
		if (m_vk->surface != VK_NULL_HANDLE && presentQueueFamily < 0)
			continue;

		if (m_vk->surface == VK_NULL_HANDLE)
			presentQueueFamily = graphicsQueueFamily;

		m_vk->physicalDevice = device;
		m_vk->graphicsQueueFamily = static_cast<u32>(graphicsQueueFamily);
		m_vk->presentQueueFamily = static_cast<u32>(presentQueueFamily);

		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(device, &properties);
		VkPhysicalDeviceFeatures features{};
		vkGetPhysicalDeviceFeatures(device, &features);
		m_maxTextureSize = static_cast<s32>(std::max<u32>(1U, properties.limits.maxImageDimension2D));
		m_maxLineWidth = features.wideLines == VK_TRUE
			? std::max(1.0f, properties.limits.lineWidthRange[1])
			: 1.0f;
		m_maxAnisotropy = std::max(1.0f, properties.limits.maxSamplerAnisotropy);

		const VkSampleCountFlags sampleCounts = properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts;
		if (sampleCounts & VK_SAMPLE_COUNT_16_BIT) {
			m_maxMsaaLevel = 16;
		} else if (sampleCounts & VK_SAMPLE_COUNT_8_BIT) {
			m_maxMsaaLevel = 8;
		} else if (sampleCounts & VK_SAMPLE_COUNT_4_BIT) {
			m_maxMsaaLevel = 4;
		} else if (sampleCounts & VK_SAMPLE_COUNT_2_BIT) {
			m_maxMsaaLevel = 2;
		} else {
			m_maxMsaaLevel = 1;
		}
		return true;
	}

	if (m_vk->surface != VK_NULL_HANDLE) {
		LOG(LOG_WARNING, "No Vulkan queue family with presentation support was found.");
	} else {
		LOG(LOG_WARNING, "No Vulkan queue family with graphics capability was found.");
	}
	return false;
#endif
}

bool ContextImpl::createDeviceAndQueue()
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk || m_vk->physicalDevice == VK_NULL_HANDLE || m_vk->graphicsQueueFamily == UINT32_MAX)
		return false;
	if (m_vk->presentQueueFamily == UINT32_MAX)
		m_vk->presentQueueFamily = m_vk->graphicsQueueFamily;

	m_vk->swapchainExtensionEnabled = false;
	m_vk->graphicsQueue = VK_NULL_HANDLE;
	m_vk->presentQueue = VK_NULL_HANDLE;

	const float queuePriority = 1.0f;
	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	queueCreateInfos.reserve(2);

	VkDeviceQueueCreateInfo graphicsQueueCreateInfo{};
	graphicsQueueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	graphicsQueueCreateInfo.queueFamilyIndex = m_vk->graphicsQueueFamily;
	graphicsQueueCreateInfo.queueCount = 1;
	graphicsQueueCreateInfo.pQueuePriorities = &queuePriority;
	queueCreateInfos.push_back(graphicsQueueCreateInfo);

	if (m_vk->presentQueueFamily != m_vk->graphicsQueueFamily) {
		VkDeviceQueueCreateInfo presentQueueCreateInfo{};
		presentQueueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		presentQueueCreateInfo.queueFamilyIndex = m_vk->presentQueueFamily;
		presentQueueCreateInfo.queueCount = 1;
		presentQueueCreateInfo.pQueuePriorities = &queuePriority;
		queueCreateInfos.push_back(presentQueueCreateInfo);
	}

	VkPhysicalDeviceFeatures availableFeatures{};
	vkGetPhysicalDeviceFeatures(m_vk->physicalDevice, &availableFeatures);
	VkPhysicalDeviceFeatures requestedFeatures{};
	if (availableFeatures.samplerAnisotropy == VK_TRUE)
		requestedFeatures.samplerAnisotropy = VK_TRUE;
	if (availableFeatures.wideLines == VK_TRUE)
		requestedFeatures.wideLines = VK_TRUE;
	if (availableFeatures.dualSrcBlend == VK_TRUE)
		requestedFeatures.dualSrcBlend = VK_TRUE;

	u32 extensionCount = 0;
	std::vector<const char *> extensions;
	if (vkEnumerateDeviceExtensionProperties(m_vk->physicalDevice, nullptr, &extensionCount, nullptr) == VK_SUCCESS && extensionCount > 0U) {
		std::vector<VkExtensionProperties> extensionProperties(extensionCount);
		if (vkEnumerateDeviceExtensionProperties(m_vk->physicalDevice, nullptr, &extensionCount, extensionProperties.data()) == VK_SUCCESS) {
			for (const auto & extension : extensionProperties) {
				if (strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
					if (m_vk->surface != VK_NULL_HANDLE) {
						extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
						m_vk->swapchainExtensionEnabled = true;
					}
					break;
				}
			}
		}
	}

	if (m_vk->surface != VK_NULL_HANDLE && !m_vk->swapchainExtensionEnabled)
		LOG(LOG_WARNING, "Vulkan device extension %s is unavailable; swapchain creation is disabled.", VK_KHR_SWAPCHAIN_EXTENSION_NAME);

	VkDeviceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	createInfo.queueCreateInfoCount = static_cast<u32>(queueCreateInfos.size());
	createInfo.pQueueCreateInfos = queueCreateInfos.data();
	createInfo.pEnabledFeatures = &requestedFeatures;
	createInfo.enabledExtensionCount = static_cast<u32>(extensions.size());
	createInfo.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

	const VkResult result = vkCreateDevice(m_vk->physicalDevice, &createInfo, nullptr, &m_vk->device);
	if (result != VK_SUCCESS) {
		LOG(LOG_WARNING, "vkCreateDevice failed: %d", static_cast<int>(result));
		return false;
	}

	vkGetDeviceQueue(m_vk->device, m_vk->graphicsQueueFamily, 0, &m_vk->graphicsQueue);
	vkGetDeviceQueue(m_vk->device, m_vk->presentQueueFamily, 0, &m_vk->presentQueue);
	m_vk->wideLinesEnabled = requestedFeatures.wideLines == VK_TRUE;
	m_vk->dualSrcBlendEnabled = requestedFeatures.dualSrcBlend == VK_TRUE;
	if (!m_vk->wideLinesEnabled)
		m_maxLineWidth = 1.0f;
	if (!m_vk->dualSrcBlendEnabled)
		LOG(LOG_WARNING, "Vulkan dual-source blending unavailable; dual-source path stays disabled.");
	if (!createPresentSyncObjects()) {
		LOG(LOG_WARNING, "Failed to create Vulkan presentation sync primitives.");
		vkDestroyDevice(m_vk->device, nullptr);
		m_vk->device = VK_NULL_HANDLE;
		m_vk->graphicsQueue = VK_NULL_HANDLE;
		m_vk->presentQueue = VK_NULL_HANDLE;
		return false;
	}
	return true;
#endif
}

bool ContextImpl::createPresentSyncObjects()
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk || m_vk->device == VK_NULL_HANDLE)
		return false;

	destroyPresentSyncObjects();

	constexpr size_t kFramesInFlight = 2;
	VkSemaphoreCreateInfo semaphoreCreateInfo{};
	semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	VkFenceCreateInfo fenceCreateInfo{};
	fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	VkCommandPoolCreateInfo commandPoolCreateInfo{};
	commandPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	commandPoolCreateInfo.queueFamilyIndex = m_vk->graphicsQueueFamily;

	m_vk->frameSync.clear();
	m_vk->frameSync.resize(kFramesInFlight);
	for (VulkanState::FrameSync & frame : m_vk->frameSync) {
		if (vkCreateSemaphore(m_vk->device, &semaphoreCreateInfo, nullptr, &frame.imageAvailable) != VK_SUCCESS) {
			destroyPresentSyncObjects();
			return false;
		}
		if (vkCreateSemaphore(m_vk->device, &semaphoreCreateInfo, nullptr, &frame.renderFinished) != VK_SUCCESS) {
			destroyPresentSyncObjects();
			return false;
		}
		if (vkCreateFence(m_vk->device, &fenceCreateInfo, nullptr, &frame.inFlight) != VK_SUCCESS) {
			destroyPresentSyncObjects();
			return false;
		}
	}

	if (vkCreateCommandPool(m_vk->device, &commandPoolCreateInfo, nullptr, &m_vk->commandPool) != VK_SUCCESS) {
		destroyPresentSyncObjects();
		return false;
	}

	m_vk->frameCommandBuffers.clear();
	m_vk->frameCommandBuffers.resize(kFramesInFlight, VK_NULL_HANDLE);
	VkCommandBufferAllocateInfo commandBufferAllocateInfo{};
	commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferAllocateInfo.commandPool = m_vk->commandPool;
	commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferAllocateInfo.commandBufferCount = static_cast<u32>(m_vk->frameCommandBuffers.size());
	if (vkAllocateCommandBuffers(m_vk->device, &commandBufferAllocateInfo, m_vk->frameCommandBuffers.data()) != VK_SUCCESS) {
		destroyPresentSyncObjects();
		return false;
	}
	if (!m_vk->uploadArena.init(
		m_vk->physicalDevice,
		m_vk->device,
		static_cast<u32>(kFramesInFlight),
		kInitialFrameVertexBufferSize)) {
		destroyPresentSyncObjects();
		return false;
	}
	if (!m_vk->textureStore.init(
		m_vk->physicalDevice,
		m_vk->device,
		m_vk->commandPool,
		m_vk->graphicsQueue,
		m_maxAnisotropy)) {
		destroyPresentSyncObjects();
		return false;
	}

	m_vk->frameSyncIndex = 0;
	m_vk->drawRecorder.markAllStateDirty();
	return true;
#endif
}

bool ContextImpl::createDrawResources()
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk || m_vk->device == VK_NULL_HANDLE || m_vk->renderPass == VK_NULL_HANDLE)
		return false;

	destroyDrawResources();
	if (!createShaderModules())
		return false;
	if (!createPipelines()) {
		destroyDrawResources();
		return false;
	}
	m_vk->drawRecorder.markAllStateDirty();
	return true;
#endif
}

bool ContextImpl::createShaderModules()
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk || m_vk->device == VK_NULL_HANDLE)
		return false;
	if (!m_vk->descriptorLayoutRegistry.init(m_vk->device))
		return false;
	if (!m_vk->programLibrary.init(m_vk->device, &m_vk->descriptorLayoutRegistry))
		return false;
	if (!m_vk->programLibrary.loadBasicColorProgram(
		kBasicColorVertSpv,
		kBasicColorVertSpv_len,
		kBasicColorFragSpv,
		kBasicColorFragSpv_len)) {
		return false;
	}
	if (!m_vk->programLibrary.loadBasicTexturedProgram(
		kBasicTexturedVertSpv,
		kBasicTexturedVertSpv_len,
		kBasicTexturedFragSpv,
		kBasicTexturedFragSpv_len)) {
		return false;
	}
	return true;
#endif
}

bool ContextImpl::createPipelines()
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk || m_vk->device == VK_NULL_HANDLE || m_vk->renderPass == VK_NULL_HANDLE)
		return false;
	const VkShaderModule colorVertexShader = m_vk->programLibrary.getBasicColorVertexShader();
	const VkShaderModule colorFragmentShader = m_vk->programLibrary.getBasicColorFragmentShader();
	const VkShaderModule texturedVertexShader = m_vk->programLibrary.getBasicTexturedVertexShader();
	const VkShaderModule texturedFragmentShader = m_vk->programLibrary.getBasicTexturedFragmentShader();
	const VkPipelineLayout colorPipelineLayout = m_vk->programLibrary.getBasicColorPipelineLayout();
	if (colorVertexShader == VK_NULL_HANDLE || colorFragmentShader == VK_NULL_HANDLE || colorPipelineLayout == VK_NULL_HANDLE)
		return false;
	if (!m_vk->pipelineCache.init(
		m_vk->device,
		m_vk->renderPass,
		colorPipelineLayout,
		colorVertexShader,
		colorFragmentShader,
		texturedVertexShader,
		texturedFragmentShader)) {
		return false;
	}
	if (!m_vk->descriptorBinder.init(
		m_vk->device,
		&m_vk->descriptorLayoutRegistry,
		&m_vk->textureStore,
		static_cast<u32>(std::max<size_t>(1, m_vk->frameSync.size())))) {
		m_vk->pipelineCache.destroy();
		return false;
	}
	return true;
#endif
}

void ContextImpl::destroyDrawResources()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk || m_vk->device == VK_NULL_HANDLE)
		return;

	m_vk->pipelineCache.destroy();
	m_vk->descriptorBinder.destroy();
	m_vk->programLibrary.destroy();
	m_vk->descriptorLayoutRegistry.destroy();
#endif
}

void ContextImpl::resetFrameRenderData()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.resetFramePackets();
#endif
}

bool ContextImpl::isDefaultDrawFramebufferBound() const
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk)
		return false;
	return m_vk->drawRecorder.isDefaultDrawFramebufferBound();
#endif
}

void ContextImpl::createSwapchain()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk || m_vk->device == VK_NULL_HANDLE || !m_vk->swapchainExtensionEnabled)
		return;
	if (m_vk->physicalDevice == VK_NULL_HANDLE)
		return;
	if (m_vk->graphicsQueueFamily == UINT32_MAX || m_vk->presentQueueFamily == UINT32_MAX)
		return;
	if (m_vk->surface == VK_NULL_HANDLE) {
		LOG(LOG_WARNING, "Vulkan surface is not initialized yet; swapchain creation is deferred.");
		return;
	}

	if (m_vk->swapchain != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(m_vk->device);
		destroySwapchain();
	}

	VkSurfaceCapabilitiesKHR capabilities{};
	if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_vk->physicalDevice, m_vk->surface, &capabilities) != VK_SUCCESS) {
		LOG(LOG_WARNING, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed.");
		return;
	}

	if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
		LOG(LOG_WARNING, "Vulkan surface does not support color-attachment swapchain images.");
		return;
	}

	u32 formatCount = 0;
	if (vkGetPhysicalDeviceSurfaceFormatsKHR(m_vk->physicalDevice, m_vk->surface, &formatCount, nullptr) != VK_SUCCESS || formatCount == 0) {
		LOG(LOG_WARNING, "vkGetPhysicalDeviceSurfaceFormatsKHR failed.");
		return;
	}

	std::vector<VkSurfaceFormatKHR> formats(formatCount);
	if (vkGetPhysicalDeviceSurfaceFormatsKHR(m_vk->physicalDevice, m_vk->surface, &formatCount, formats.data()) != VK_SUCCESS) {
		LOG(LOG_WARNING, "vkGetPhysicalDeviceSurfaceFormatsKHR failed.");
		return;
	}

	VkSurfaceFormatKHR selectedFormat = formats[0];
	for (const auto & format : formats) {
		if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			selectedFormat = format;
			break;
		}
		if (format.format == VK_FORMAT_R8G8B8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			selectedFormat = format;
		}
	}

	u32 presentModeCount = 0;
	std::vector<VkPresentModeKHR> presentModes;
	if (vkGetPhysicalDeviceSurfacePresentModesKHR(m_vk->physicalDevice, m_vk->surface, &presentModeCount, nullptr) == VK_SUCCESS && presentModeCount > 0) {
		presentModes.resize(presentModeCount);
		if (vkGetPhysicalDeviceSurfacePresentModesKHR(m_vk->physicalDevice, m_vk->surface, &presentModeCount, presentModes.data()) != VK_SUCCESS) {
			presentModes.clear();
		}
	}

	VkPresentModeKHR selectedPresentMode = VK_PRESENT_MODE_FIFO_KHR;
	for (VkPresentModeKHR presentMode : presentModes) {
		if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
			selectedPresentMode = presentMode;
			break;
		}
	}

	VkExtent2D extent = capabilities.currentExtent;
	if (extent.width == std::numeric_limits<u32>::max()) {
		const u32 requestedWidth = std::max<u32>(1U, m_presentationWindowInfo.width);
		const u32 requestedHeight = std::max<u32>(1U, m_presentationWindowInfo.height);
		extent.width = std::min(capabilities.maxImageExtent.width, std::max(capabilities.minImageExtent.width, requestedWidth));
		extent.height = std::min(capabilities.maxImageExtent.height, std::max(capabilities.minImageExtent.height, requestedHeight));
	}

	u32 imageCount = capabilities.minImageCount + 1;
	if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount)
		imageCount = capabilities.maxImageCount;

	VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if ((capabilities.supportedCompositeAlpha & compositeAlpha) == 0) {
		if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) {
			compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
		} else if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR) {
			compositeAlpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
		} else if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
			compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
		}
	}

	VkSwapchainCreateInfoKHR createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	createInfo.surface = m_vk->surface;
	createInfo.minImageCount = imageCount;
	createInfo.imageFormat = selectedFormat.format;
	createInfo.imageColorSpace = selectedFormat.colorSpace;
	createInfo.imageExtent = extent;
	createInfo.imageArrayLayers = 1;
	createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	u32 queueFamilyIndices[2] = { m_vk->graphicsQueueFamily, m_vk->presentQueueFamily };
	if (m_vk->graphicsQueueFamily != m_vk->presentQueueFamily) {
		createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		createInfo.queueFamilyIndexCount = 2;
		createInfo.pQueueFamilyIndices = queueFamilyIndices;
	} else {
		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		createInfo.queueFamilyIndexCount = 0;
		createInfo.pQueueFamilyIndices = nullptr;
	}
	createInfo.preTransform = capabilities.currentTransform;
	createInfo.compositeAlpha = compositeAlpha;
	createInfo.presentMode = selectedPresentMode;
	createInfo.clipped = VK_TRUE;
	createInfo.oldSwapchain = VK_NULL_HANDLE;

	const VkResult result = vkCreateSwapchainKHR(m_vk->device, &createInfo, nullptr, &m_vk->swapchain);
	if (result != VK_SUCCESS) {
		LOG(LOG_WARNING, "vkCreateSwapchainKHR failed: %d", static_cast<int>(result));
		return;
	}

	u32 swapchainImageCount = 0;
	if (vkGetSwapchainImagesKHR(m_vk->device, m_vk->swapchain, &swapchainImageCount, nullptr) != VK_SUCCESS || swapchainImageCount == 0U) {
		LOG(LOG_WARNING, "vkGetSwapchainImagesKHR failed.");
		destroySwapchain();
		return;
	}

	m_vk->swapchainImages.resize(swapchainImageCount, VK_NULL_HANDLE);
	if (vkGetSwapchainImagesKHR(m_vk->device, m_vk->swapchain, &swapchainImageCount, m_vk->swapchainImages.data()) != VK_SUCCESS) {
		LOG(LOG_WARNING, "vkGetSwapchainImagesKHR failed.");
		destroySwapchain();
		return;
	}

	m_vk->swapchainImageViews.clear();
	m_vk->swapchainImageViews.reserve(m_vk->swapchainImages.size());
	for (VkImage image : m_vk->swapchainImages) {
		VkImageViewCreateInfo imageViewCreateInfo{};
		imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		imageViewCreateInfo.image = image;
		imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		imageViewCreateInfo.format = selectedFormat.format;
		imageViewCreateInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		imageViewCreateInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		imageViewCreateInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		imageViewCreateInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
		imageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
		imageViewCreateInfo.subresourceRange.levelCount = 1;
		imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
		imageViewCreateInfo.subresourceRange.layerCount = 1;

		VkImageView imageView = VK_NULL_HANDLE;
		if (vkCreateImageView(m_vk->device, &imageViewCreateInfo, nullptr, &imageView) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkCreateImageView failed for swapchain image.");
			destroySwapchain();
			return;
		}
		m_vk->swapchainImageViews.push_back(imageView);
	}

	m_vk->depthFormat = pickDepthFormat(m_vk->physicalDevice);
	if (m_vk->depthFormat == VK_FORMAT_UNDEFINED) {
		LOG(LOG_WARNING, "No supported Vulkan depth format found.");
		destroySwapchain();
		return;
	}

	const bool depthHasStencil = hasStencilComponent(m_vk->depthFormat);
	const VkImageAspectFlags depthAspectMask = depthHasStencil
		? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
		: VK_IMAGE_ASPECT_DEPTH_BIT;
	m_vk->swapchainDepthImages.assign(m_vk->swapchainImages.size(), VK_NULL_HANDLE);
	m_vk->swapchainDepthMemories.assign(m_vk->swapchainImages.size(), VK_NULL_HANDLE);
	m_vk->swapchainDepthImageViews.assign(m_vk->swapchainImages.size(), VK_NULL_HANDLE);

	for (size_t i = 0; i < m_vk->swapchainDepthImages.size(); ++i) {
		VkImageCreateInfo depthImageCreateInfo{};
		depthImageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		depthImageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
		depthImageCreateInfo.extent.width = extent.width;
		depthImageCreateInfo.extent.height = extent.height;
		depthImageCreateInfo.extent.depth = 1;
		depthImageCreateInfo.mipLevels = 1;
		depthImageCreateInfo.arrayLayers = 1;
		depthImageCreateInfo.format = m_vk->depthFormat;
		depthImageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		depthImageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		depthImageCreateInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		depthImageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		depthImageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		if (vkCreateImage(m_vk->device, &depthImageCreateInfo, nullptr, &m_vk->swapchainDepthImages[i]) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkCreateImage failed for swapchain depth image.");
			destroySwapchain();
			return;
		}

		VkMemoryRequirements depthMemoryRequirements{};
		vkGetImageMemoryRequirements(m_vk->device, m_vk->swapchainDepthImages[i], &depthMemoryRequirements);
		const u32 depthMemoryTypeIndex = findMemoryTypeIndex(
			m_vk->physicalDevice,
			depthMemoryRequirements.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (depthMemoryTypeIndex == UINT32_MAX) {
			LOG(LOG_WARNING, "No compatible memory type found for Vulkan depth image.");
			destroySwapchain();
			return;
		}

		VkMemoryAllocateInfo depthMemoryAllocateInfo{};
		depthMemoryAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		depthMemoryAllocateInfo.allocationSize = depthMemoryRequirements.size;
		depthMemoryAllocateInfo.memoryTypeIndex = depthMemoryTypeIndex;
		if (vkAllocateMemory(m_vk->device, &depthMemoryAllocateInfo, nullptr, &m_vk->swapchainDepthMemories[i]) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkAllocateMemory failed for swapchain depth image.");
			destroySwapchain();
			return;
		}

		if (vkBindImageMemory(m_vk->device, m_vk->swapchainDepthImages[i], m_vk->swapchainDepthMemories[i], 0) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkBindImageMemory failed for swapchain depth image.");
			destroySwapchain();
			return;
		}

		VkImageViewCreateInfo depthImageViewCreateInfo{};
		depthImageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		depthImageViewCreateInfo.image = m_vk->swapchainDepthImages[i];
		depthImageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		depthImageViewCreateInfo.format = m_vk->depthFormat;
		depthImageViewCreateInfo.subresourceRange.aspectMask = depthAspectMask;
		depthImageViewCreateInfo.subresourceRange.baseMipLevel = 0;
		depthImageViewCreateInfo.subresourceRange.levelCount = 1;
		depthImageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
		depthImageViewCreateInfo.subresourceRange.layerCount = 1;

		if (vkCreateImageView(m_vk->device, &depthImageViewCreateInfo, nullptr, &m_vk->swapchainDepthImageViews[i]) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkCreateImageView failed for swapchain depth image.");
			destroySwapchain();
			return;
		}
	}

	const VkAttachmentDescription attachments[2] = {
		{
			0,
			selectedFormat.format,
			VK_SAMPLE_COUNT_1_BIT,
			VK_ATTACHMENT_LOAD_OP_CLEAR,
			VK_ATTACHMENT_STORE_OP_STORE,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			VK_ATTACHMENT_STORE_OP_DONT_CARE,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
		},
		{
			0,
			m_vk->depthFormat,
			VK_SAMPLE_COUNT_1_BIT,
			VK_ATTACHMENT_LOAD_OP_CLEAR,
			VK_ATTACHMENT_STORE_OP_DONT_CARE,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			VK_ATTACHMENT_STORE_OP_DONT_CARE,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
		}
	};

	VkAttachmentReference colorAttachmentReference{};
	colorAttachmentReference.attachment = 0;
	colorAttachmentReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference depthAttachmentReference{};
	depthAttachmentReference.attachment = 1;
	depthAttachmentReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpassDescription{};
	subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpassDescription.colorAttachmentCount = 1;
	subpassDescription.pColorAttachments = &colorAttachmentReference;
	subpassDescription.pDepthStencilAttachment = &depthAttachmentReference;

	VkSubpassDependency subpassDependency{};
	subpassDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	subpassDependency.dstSubpass = 0;
	subpassDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
		| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
		| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	subpassDependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
		| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
		| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	subpassDependency.srcAccessMask = 0;
	subpassDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
		| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
		| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo renderPassCreateInfo{};
	renderPassCreateInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassCreateInfo.attachmentCount = 2;
	renderPassCreateInfo.pAttachments = attachments;
	renderPassCreateInfo.subpassCount = 1;
	renderPassCreateInfo.pSubpasses = &subpassDescription;
	renderPassCreateInfo.dependencyCount = 1;
	renderPassCreateInfo.pDependencies = &subpassDependency;
	if (vkCreateRenderPass(m_vk->device, &renderPassCreateInfo, nullptr, &m_vk->renderPass) != VK_SUCCESS) {
		LOG(LOG_WARNING, "vkCreateRenderPass failed for swapchain.");
		destroySwapchain();
		return;
	}
	if (!createDrawResources()) {
		LOG(LOG_WARNING, "Failed to initialize Vulkan draw resources.");
		destroySwapchain();
		return;
	}

	m_vk->swapchainFramebuffers.clear();
	m_vk->swapchainFramebuffers.reserve(m_vk->swapchainImageViews.size());
	for (size_t i = 0; i < m_vk->swapchainImageViews.size(); ++i) {
		const VkImageView attachmentsForFramebuffer[2] = {
			m_vk->swapchainImageViews[i],
			m_vk->swapchainDepthImageViews[i]
		};
		VkFramebufferCreateInfo framebufferCreateInfo{};
		framebufferCreateInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebufferCreateInfo.renderPass = m_vk->renderPass;
		framebufferCreateInfo.attachmentCount = 2;
		framebufferCreateInfo.pAttachments = attachmentsForFramebuffer;
		framebufferCreateInfo.width = extent.width;
		framebufferCreateInfo.height = extent.height;
		framebufferCreateInfo.layers = 1;

		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		if (vkCreateFramebuffer(m_vk->device, &framebufferCreateInfo, nullptr, &framebuffer) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkCreateFramebuffer failed for swapchain image.");
			destroySwapchain();
			return;
		}
		m_vk->swapchainFramebuffers.push_back(framebuffer);
	}

	m_vk->swapchainFormat = selectedFormat.format;
	m_vk->swapchainExtent = extent;
	m_vk->swapchainImageFences.assign(m_vk->swapchainImages.size(), VK_NULL_HANDLE);
	m_vk->frameSyncIndex = 0;
	m_vk->lastPresentedImageIndex = UINT32_MAX;
	resetFrameRenderData();

	LOG(LOG_VERBOSE, "Vulkan swapchain created: %ux%u", extent.width, extent.height);
#endif
}

void ContextImpl::destroySwapchain()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk || m_vk->device == VK_NULL_HANDLE)
		return;

	destroyDrawResources();

	if (!m_vk->swapchainFramebuffers.empty()) {
		for (VkFramebuffer framebuffer : m_vk->swapchainFramebuffers) {
			if (framebuffer != VK_NULL_HANDLE)
				vkDestroyFramebuffer(m_vk->device, framebuffer, nullptr);
		}
		m_vk->swapchainFramebuffers.clear();
	}

	if (!m_vk->swapchainDepthImageViews.empty()) {
		for (VkImageView imageView : m_vk->swapchainDepthImageViews) {
			if (imageView != VK_NULL_HANDLE)
				vkDestroyImageView(m_vk->device, imageView, nullptr);
		}
		m_vk->swapchainDepthImageViews.clear();
	}

	if (!m_vk->swapchainDepthImages.empty()) {
		for (VkImage image : m_vk->swapchainDepthImages) {
			if (image != VK_NULL_HANDLE)
				vkDestroyImage(m_vk->device, image, nullptr);
		}
		m_vk->swapchainDepthImages.clear();
	}

	if (!m_vk->swapchainDepthMemories.empty()) {
		for (VkDeviceMemory memory : m_vk->swapchainDepthMemories) {
			if (memory != VK_NULL_HANDLE)
				vkFreeMemory(m_vk->device, memory, nullptr);
		}
		m_vk->swapchainDepthMemories.clear();
	}

	if (m_vk->renderPass != VK_NULL_HANDLE) {
		vkDestroyRenderPass(m_vk->device, m_vk->renderPass, nullptr);
		m_vk->renderPass = VK_NULL_HANDLE;
	}

	if (!m_vk->swapchainImageViews.empty()) {
		for (VkImageView imageView : m_vk->swapchainImageViews) {
			if (imageView != VK_NULL_HANDLE)
				vkDestroyImageView(m_vk->device, imageView, nullptr);
		}
		m_vk->swapchainImageViews.clear();
	}

	m_vk->swapchainImages.clear();
	m_vk->swapchainImageFences.clear();
	m_vk->swapchainFormat = VK_FORMAT_UNDEFINED;
	m_vk->depthFormat = VK_FORMAT_UNDEFINED;
	m_vk->swapchainExtent = {};
	m_vk->lastPresentedImageIndex = UINT32_MAX;
	resetFrameRenderData();

	if (m_vk->swapchain == VK_NULL_HANDLE)
		return;
	vkDestroySwapchainKHR(m_vk->device, m_vk->swapchain, nullptr);
	m_vk->swapchain = VK_NULL_HANDLE;
#endif
}

void ContextImpl::destroyPresentSyncObjects()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk || m_vk->device == VK_NULL_HANDLE)
		return;

	m_vk->bindingState.clear();
	m_vk->framebufferStore.clear();
	m_vk->textureStore.destroy();
	m_vk->uploadArena.destroy();

	for (VulkanState::FrameSync & frame : m_vk->frameSync) {
		if (frame.imageAvailable != VK_NULL_HANDLE) {
			vkDestroySemaphore(m_vk->device, frame.imageAvailable, nullptr);
			frame.imageAvailable = VK_NULL_HANDLE;
		}
		if (frame.renderFinished != VK_NULL_HANDLE) {
			vkDestroySemaphore(m_vk->device, frame.renderFinished, nullptr);
			frame.renderFinished = VK_NULL_HANDLE;
		}
		if (frame.inFlight != VK_NULL_HANDLE) {
			vkDestroyFence(m_vk->device, frame.inFlight, nullptr);
			frame.inFlight = VK_NULL_HANDLE;
		}
	}

	m_vk->frameCommandBuffers.clear();
	if (m_vk->commandPool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(m_vk->device, m_vk->commandPool, nullptr);
		m_vk->commandPool = VK_NULL_HANDLE;
	}

	m_vk->frameSync.clear();
	m_vk->frameSyncIndex = 0;
#endif
}

void ContextImpl::destroySurface()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk || m_vk->instance == VK_NULL_HANDLE || m_vk->surface == VK_NULL_HANDLE)
		return;
	vkDestroySurfaceKHR(m_vk->instance, m_vk->surface, nullptr);
	m_vk->surface = VK_NULL_HANDLE;
#endif
}

void ContextImpl::shutdownVulkanCore()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk) {
		m_coreReady = false;
		return;
	}

	if (m_vk->device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(m_vk->device);

	destroySwapchain();
	destroyPresentSyncObjects();

	if (m_vk->device != VK_NULL_HANDLE) {
		vkDestroyDevice(m_vk->device, nullptr);
		m_vk->device = VK_NULL_HANDLE;
	}

	destroySurface();

	if (m_vk->instance != VK_NULL_HANDLE) {
		vkDestroyInstance(m_vk->instance, nullptr);
		m_vk->instance = VK_NULL_HANDLE;
	}
#endif

	m_vk.reset();
	m_coreReady = false;
}

graphics::ObjectHandle ContextImpl::_allocateHandle()
{
	return graphics::ObjectHandle(m_nextHandle++);
}

} // namespace vulkan
