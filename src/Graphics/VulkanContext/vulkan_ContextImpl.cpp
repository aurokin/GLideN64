#include "vulkan_ContextImpl.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>
#include <Log.h>
#include <Graphics/ColorBufferReader.h>
#include <Graphics/Parameters.h>
#include <Graphics/ShaderProgram.h>

#if defined(OS_LINUX) && !defined(VK_USE_PLATFORM_XLIB_KHR)
#define VK_USE_PLATFORM_XLIB_KHR 1
#endif

#if __has_include(<vulkan/vulkan.h>)
#define GLIDEN64_VULKAN_HEADERS_AVAILABLE 1
#include <vulkan/vulkan.h>
#else
#define GLIDEN64_VULKAN_HEADERS_AVAILABLE 0
#endif

namespace {

class DummyPixelReadBuffer final : public graphics::PixelReadBuffer
{
public:
	explicit DummyPixelReadBuffer(size_t _sizeInBytes)
		: m_data(_sizeInBytes, 0U)
	{
	}

	void readPixels(s32 _x, s32 _y, u32 _width, u32 _height, graphics::Parameter _format, graphics::Parameter _type) override
	{
		(void)_x;
		(void)_y;
		(void)_width;
		(void)_height;
		(void)_format;
		(void)_type;
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
	std::vector<u8> m_data;
};

class DummyColorBufferReader final : public graphics::ColorBufferReader
{
public:
	explicit DummyColorBufferReader(CachedTexture * _pTexture)
		: ColorBufferReader(_pTexture)
	{
	}

	void cleanUp() override {}

private:
	const u8 * _readPixels(const ReadColorBufferParams & _params, u32 & _heightOffset, u32 & _stride) override
	{
		_heightOffset = 0;
		_stride = _params.width;
		const size_t requestedBytes = static_cast<size_t>(_params.width) * _params.height * std::max<u32>(1U, _params.colorFormatBytes);
		if (m_tempPixelData.size() < requestedBytes)
			m_tempPixelData.resize(requestedBytes);
		std::fill(m_tempPixelData.begin(), m_tempPixelData.end(), 0U);
		return m_tempPixelData.data();
	}
};

class DummyCombinerProgram final : public graphics::CombinerProgram
{
public:
	explicit DummyCombinerProgram(const CombinerKey & _key)
		: m_key(_key)
	{
	}

	void activate() override {}

	void update(bool _force) override
	{
		(void)_force;
	}

	const CombinerKey & getKey() const override { return m_key; }
	bool usesTexture() const override { return true; }
	bool usesTile(u32 _t) const override { return _t < 2U; }
	bool usesShade() const override { return false; }
	bool usesLOD() const override { return false; }
	bool usesHwLighting() const override { return false; }
	bool getBinaryForm(std::vector<char> & _buffer) override
	{
		_buffer.clear();
		return false;
	}

private:
	CombinerKey m_key;
};

class DummyShaderProgram final : public graphics::ShaderProgram
{
public:
	void activate() override {}
};

class DummyTexrectDrawerShaderProgram final : public graphics::TexrectDrawerShaderProgram
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
	void setTextureSize(u32 _width, u32 _height) override { (void)_width; (void)_height; }
	void setEnableAlphaTest(int _enable) override { (void)_enable; }
};

class DummyTextDrawerShaderProgram final : public graphics::TextDrawerShaderProgram
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
	void setTextColor(float * _color) override { (void)_color; }
};

} // namespace

namespace vulkan {

struct ContextImpl::VulkanState
{
#if GLIDEN64_VULKAN_HEADERS_AVAILABLE
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue graphicsQueue = VK_NULL_HANDLE;
	u32 graphicsQueueFamily = UINT32_MAX;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	bool surfaceExtensionEnabled = false;
	bool xlibSurfaceExtensionEnabled = false;
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
#if GLIDEN64_VULKAN_HEADERS_AVAILABLE
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

#if GLIDEN64_VULKAN_HEADERS_AVAILABLE
	if (!changed || !m_vk || m_vk->instance == VK_NULL_HANDLE)
		return;

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
	(void)_parameter;
	(void)_enable;
}

u32 ContextImpl::isEnabled(graphics::EnableParam _parameter)
{
	(void)_parameter;
	return 0U;
}

void ContextImpl::cullFace(graphics::CullModeParam _mode)
{
	(void)_mode;
}

void ContextImpl::enableDepthWrite(bool _enable)
{
	(void)_enable;
}

void ContextImpl::setDepthCompare(graphics::CompareParam _mode)
{
	(void)_mode;
}

void ContextImpl::setViewport(s32 _x, s32 _y, s32 _width, s32 _height)
{
	(void)_x;
	(void)_y;
	(void)_width;
	(void)_height;
}

void ContextImpl::setScissor(s32 _x, s32 _y, s32 _width, s32 _height)
{
	(void)_x;
	(void)_y;
	(void)_width;
	(void)_height;
}

void ContextImpl::setBlending(graphics::BlendParam _sfactor, graphics::BlendParam _dfactor)
{
	(void)_sfactor;
	(void)_dfactor;
}

void ContextImpl::setBlendingSeparate(graphics::BlendParam _sfactorcolor, graphics::BlendParam _dfactorcolor, graphics::BlendParam _sfactoralpha, graphics::BlendParam _dfactoralpha)
{
	(void)_sfactorcolor;
	(void)_dfactorcolor;
	(void)_sfactoralpha;
	(void)_dfactoralpha;
}

void ContextImpl::setBlendColor(f32 _red, f32 _green, f32 _blue, f32 _alpha)
{
	(void)_red;
	(void)_green;
	(void)_blue;
	(void)_alpha;
}

void ContextImpl::clearColorBuffer(f32 _red, f32 _green, f32 _blue, f32 _alpha)
{
	(void)_red;
	(void)_green;
	(void)_blue;
	(void)_alpha;
}

void ContextImpl::clearDepthBuffer()
{
}

void ContextImpl::setPolygonOffset(f32 _factor, f32 _units)
{
	(void)_factor;
	(void)_units;
}

graphics::ObjectHandle ContextImpl::createTexture(graphics::Parameter _target)
{
	(void)_target;
	return _allocateHandle();
}

void ContextImpl::deleteTexture(graphics::ObjectHandle _name)
{
	(void)_name;
}

void ContextImpl::init2DTexture(const graphics::Context::InitTextureParams & _params)
{
	(void)_params;
}

void ContextImpl::update2DTexture(const graphics::Context::UpdateTextureDataParams & _params)
{
	(void)_params;
}

void ContextImpl::setTextureParameters(const graphics::Context::TexParameters & _parameters)
{
	(void)_parameters;
}

void ContextImpl::bindTexture(const graphics::Context::BindTextureParameters & _params)
{
	(void)_params;
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
	(void)_params;
}

u32 ContextImpl::convertInternalTextureFormat(u32 _format) const
{
	return _format;
}

void ContextImpl::textureBarrier()
{
}

graphics::FramebufferTextureFormats * ContextImpl::getFramebufferTextureFormats()
{
	return new graphics::FramebufferTextureFormats(*m_fbTexFormats);
}

graphics::ObjectHandle ContextImpl::createFramebuffer()
{
	return _allocateHandle();
}

void ContextImpl::deleteFramebuffer(graphics::ObjectHandle _name)
{
	(void)_name;
}

void ContextImpl::bindFramebuffer(graphics::BufferTargetParam _target, graphics::ObjectHandle _name)
{
	(void)_target;
	(void)_name;
}

void ContextImpl::addFrameBufferRenderTarget(const graphics::Context::FrameBufferRenderTarget & _params)
{
	(void)_params;
}

graphics::ObjectHandle ContextImpl::createRenderbuffer()
{
	return _allocateHandle();
}

void ContextImpl::initRenderbuffer(const graphics::Context::InitRenderbufferParams & _params)
{
	(void)_params;
}

bool ContextImpl::blitFramebuffers(const graphics::Context::BlitFramebuffersParams & _params)
{
	(void)_params;
	return false;
}

void ContextImpl::setDrawBuffers(u32 _num)
{
	(void)_num;
}

graphics::PixelReadBuffer * ContextImpl::createPixelReadBuffer(size_t _sizeInBytes)
{
	return new DummyPixelReadBuffer(_sizeInBytes);
}

graphics::ColorBufferReader * ContextImpl::createColorBufferReader(CachedTexture * _pTexture)
{
	if (_pTexture == nullptr)
		return nullptr;
	return new DummyColorBufferReader(_pTexture);
}

bool ContextImpl::isCombinerProgramBuilderObsolete()
{
	return false;
}

void ContextImpl::resetCombinerProgramBuilder()
{
}

graphics::CombinerProgram * ContextImpl::createCombinerProgram(Combiner & _color, Combiner & _alpha, const CombinerKey & _key)
{
	(void)_color;
	(void)_alpha;
	return new DummyCombinerProgram(_key);
}

bool ContextImpl::saveShadersStorage(const graphics::Combiners & _combiners)
{
	(void)_combiners;
	return false;
}

bool ContextImpl::loadShadersStorage(graphics::Combiners & _combiners)
{
	(void)_combiners;
	return false;
}

graphics::ShaderProgram * ContextImpl::createDepthFogShader()
{
	return new DummyShaderProgram;
}

graphics::TexrectDrawerShaderProgram * ContextImpl::createTexrectDrawerDrawShader()
{
	return new DummyTexrectDrawerShaderProgram;
}

graphics::ShaderProgram * ContextImpl::createTexrectDrawerClearShader()
{
	return new DummyShaderProgram;
}

graphics::ShaderProgram * ContextImpl::createTexrectUpscaleCopyShader()
{
	return new DummyShaderProgram;
}

graphics::ShaderProgram * ContextImpl::createTexrectColorAndDepthUpscaleCopyShader()
{
	return new DummyShaderProgram;
}

graphics::ShaderProgram * ContextImpl::createTexrectDownscaleCopyShader()
{
	return new DummyShaderProgram;
}

graphics::ShaderProgram * ContextImpl::createTexrectColorAndDepthDownscaleCopyShader()
{
	return new DummyShaderProgram;
}

graphics::ShaderProgram * ContextImpl::createGammaCorrectionShader()
{
	return new DummyShaderProgram;
}

graphics::ShaderProgram * ContextImpl::createFXAAShader()
{
	return new DummyShaderProgram;
}

graphics::TextDrawerShaderProgram * ContextImpl::createTextDrawerShader()
{
	return new DummyTextDrawerShaderProgram;
}

void ContextImpl::resetShaderProgram()
{
}

void ContextImpl::drawTriangles(const graphics::Context::DrawTriangleParameters & _params)
{
	(void)_params;
}

void ContextImpl::drawRects(const graphics::Context::DrawRectParameters & _params)
{
	(void)_params;
}

void ContextImpl::drawLine(f32 _width, SPVertex * _vertices)
{
	(void)_width;
	(void)_vertices;
}

f32 ContextImpl::getMaxLineWidth()
{
	return m_maxLineWidth;
}

bool ContextImpl::isSupported(graphics::SpecialFeatures _feature) const
{
	switch (_feature) {
	case graphics::SpecialFeatures::Multisampling:
		return m_maxMsaaLevel > 1;
	case graphics::SpecialFeatures::BlitFramebuffer:
	case graphics::SpecialFeatures::WeakBlitFramebuffer:
	case graphics::SpecialFeatures::ShaderProgramBinary:
	case graphics::SpecialFeatures::ImageTextures:
	case graphics::SpecialFeatures::N64DepthWithFbFetchDepth:
	case graphics::SpecialFeatures::FramebufferFetchColor:
	case graphics::SpecialFeatures::TextureBarrier:
	case graphics::SpecialFeatures::EglImage:
	case graphics::SpecialFeatures::EglImageFramebuffer:
	case graphics::SpecialFeatures::DualSourceBlending:
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
	return false;
}

bool ContextImpl::isFramebufferError() const
{
	return false;
}

void ContextImpl::initFramebufferFormats()
{
	m_fbTexFormats.reset(new graphics::FramebufferTextureFormats);
	m_fbTexFormats->colorInternalFormat = graphics::internalcolorFormat::RGBA8;
	m_fbTexFormats->colorFormat = graphics::colorFormat::RGBA;
	m_fbTexFormats->colorType = graphics::datatype::UNSIGNED_BYTE;
	m_fbTexFormats->colorFormatBytes = 4;

	m_fbTexFormats->monochromeInternalFormat = graphics::internalcolorFormat::RG;
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

	m_fbTexFormats->fontInternalFormat = graphics::internalcolorFormat::RG;
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
#if !GLIDEN64_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk)
		m_vk.reset(new VulkanState);

	m_vk->surfaceExtensionEnabled = false;
	m_vk->xlibSurfaceExtensionEnabled = false;
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
	const bool hasPresentationTarget = m_presentationWindowInfo.system != graphics::Context::PresentationWindowInfo::WindowSystem::Unknown
		&& m_presentationWindowInfo.window != 0;
	if (hasPresentationTarget) {
		bool platformExtensionReady = false;

		switch (m_presentationWindowInfo.system) {
#if defined(OS_LINUX)
		case graphics::Context::PresentationWindowInfo::WindowSystem::Xlib:
			if (hasInstanceExtension(VK_KHR_XLIB_SURFACE_EXTENSION_NAME)) {
				enabledInstanceExtensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
				m_vk->xlibSurfaceExtensionEnabled = true;
				platformExtensionReady = true;
			} else {
				LOG(LOG_WARNING, "Vulkan extension %s is unavailable; Xlib surface creation is disabled.", VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
			}
			break;
#endif
		case graphics::Context::PresentationWindowInfo::WindowSystem::Win32:
			LOG(LOG_WARNING, "Win32 Vulkan presentation wiring is not implemented yet.");
			break;
		case graphics::Context::PresentationWindowInfo::WindowSystem::Unknown:
		default:
			break;
		}

		if (platformExtensionReady) {
			if (hasInstanceExtension(VK_KHR_SURFACE_EXTENSION_NAME)) {
				enabledInstanceExtensions.insert(enabledInstanceExtensions.begin(), VK_KHR_SURFACE_EXTENSION_NAME);
				m_vk->surfaceExtensionEnabled = true;
			} else {
				LOG(LOG_WARNING, "Vulkan extension %s is unavailable; surface creation is disabled.", VK_KHR_SURFACE_EXTENSION_NAME);
				m_vk->xlibSurfaceExtensionEnabled = false;
				enabledInstanceExtensions.clear();
			}
		}
	}

	VkApplicationInfo appInfo{};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "GLideN64 Vulkan";
	appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
	appInfo.pEngineName = "GLideN64";
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
#if !GLIDEN64_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk || m_vk->instance == VK_NULL_HANDLE)
		return false;
	if (m_vk->surface != VK_NULL_HANDLE)
		return true;

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
	case graphics::Context::PresentationWindowInfo::WindowSystem::Win32:
		return false;
	case graphics::Context::PresentationWindowInfo::WindowSystem::Unknown:
	default:
		return false;
	}
#endif
}

bool ContextImpl::selectPhysicalDevice()
{
#if !GLIDEN64_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk || m_vk->instance == VK_NULL_HANDLE)
		return false;

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
		for (u32 i = 0; i < queueFamilyCount; ++i) {
			if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
				continue;

			if (m_vk->surface != VK_NULL_HANDLE) {
				VkBool32 presentSupported = VK_FALSE;
				if (vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_vk->surface, &presentSupported) != VK_SUCCESS || presentSupported == VK_FALSE)
					continue;
			}

			m_vk->physicalDevice = device;
			m_vk->graphicsQueueFamily = i;

			VkPhysicalDeviceProperties properties{};
			vkGetPhysicalDeviceProperties(device, &properties);
			m_maxTextureSize = static_cast<s32>(std::max<u32>(1U, properties.limits.maxImageDimension2D));
			m_maxLineWidth = std::max(1.0f, properties.limits.lineWidthRange[1]);
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
	}

	if (m_vk->surface != VK_NULL_HANDLE) {
		LOG(LOG_WARNING, "No Vulkan queue family with both graphics and presentation support was found.");
	} else {
		LOG(LOG_WARNING, "No Vulkan queue family with graphics capability was found.");
	}
	return false;
#endif
}

bool ContextImpl::createDeviceAndQueue()
{
#if !GLIDEN64_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk || m_vk->physicalDevice == VK_NULL_HANDLE || m_vk->graphicsQueueFamily == UINT32_MAX)
		return false;

	m_vk->swapchainExtensionEnabled = false;

	const float queuePriority = 1.0f;
	VkDeviceQueueCreateInfo queueCreateInfo{};
	queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueCreateInfo.queueFamilyIndex = m_vk->graphicsQueueFamily;
	queueCreateInfo.queueCount = 1;
	queueCreateInfo.pQueuePriorities = &queuePriority;

	VkPhysicalDeviceFeatures availableFeatures{};
	vkGetPhysicalDeviceFeatures(m_vk->physicalDevice, &availableFeatures);
	VkPhysicalDeviceFeatures requestedFeatures{};
	if (availableFeatures.samplerAnisotropy == VK_TRUE)
		requestedFeatures.samplerAnisotropy = VK_TRUE;

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
	createInfo.queueCreateInfoCount = 1;
	createInfo.pQueueCreateInfos = &queueCreateInfo;
	createInfo.pEnabledFeatures = &requestedFeatures;
	createInfo.enabledExtensionCount = static_cast<u32>(extensions.size());
	createInfo.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

	const VkResult result = vkCreateDevice(m_vk->physicalDevice, &createInfo, nullptr, &m_vk->device);
	if (result != VK_SUCCESS) {
		LOG(LOG_WARNING, "vkCreateDevice failed: %d", static_cast<int>(result));
		return false;
	}

	vkGetDeviceQueue(m_vk->device, m_vk->graphicsQueueFamily, 0, &m_vk->graphicsQueue);
	return true;
#endif
}

void ContextImpl::createSwapchain()
{
#if GLIDEN64_VULKAN_HEADERS_AVAILABLE
	if (!m_vk || m_vk->device == VK_NULL_HANDLE || !m_vk->swapchainExtensionEnabled)
		return;
	if (m_vk->surface == VK_NULL_HANDLE) {
		LOG(LOG_WARNING, "Vulkan surface is not initialized yet; swapchain creation is deferred.");
		return;
	}

	if (m_vk->swapchain != VK_NULL_HANDLE)
		destroySwapchain();

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
	createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
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

	LOG(LOG_VERBOSE, "Vulkan swapchain created: %ux%u", extent.width, extent.height);
#endif
}

void ContextImpl::destroySwapchain()
{
#if GLIDEN64_VULKAN_HEADERS_AVAILABLE
	if (!m_vk || m_vk->device == VK_NULL_HANDLE || m_vk->swapchain == VK_NULL_HANDLE)
		return;
	vkDestroySwapchainKHR(m_vk->device, m_vk->swapchain, nullptr);
	m_vk->swapchain = VK_NULL_HANDLE;
#endif
}

void ContextImpl::destroySurface()
{
#if GLIDEN64_VULKAN_HEADERS_AVAILABLE
	if (!m_vk || m_vk->instance == VK_NULL_HANDLE || m_vk->surface == VK_NULL_HANDLE)
		return;
	vkDestroySurfaceKHR(m_vk->instance, m_vk->surface, nullptr);
	m_vk->surface = VK_NULL_HANDLE;
#endif
}

void ContextImpl::shutdownVulkanCore()
{
#if GLIDEN64_VULKAN_HEADERS_AVAILABLE
	if (!m_vk) {
		m_coreReady = false;
		return;
	}

	if (m_vk->device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(m_vk->device);

	destroySwapchain();

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
