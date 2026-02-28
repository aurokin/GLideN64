#include "vulkan_ContextImpl.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>
#include <Log.h>
#include <Graphics/ColorBufferReader.h>
#include <Graphics/Parameters.h>
#include <Graphics/ShaderProgram.h>
#include "vulkan_BasicColorShaders.h"

#if defined(OS_LINUX) && !defined(VK_USE_PLATFORM_XLIB_KHR)
#define VK_USE_PLATFORM_XLIB_KHR 1
#endif

#if defined(OS_WINDOWS) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR 1
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

#if GLIDEN64_VULKAN_HEADERS_AVAILABLE
constexpr VkDeviceSize kInitialFrameVertexBufferSize = 64U * 1024U;

VkPrimitiveTopology toVkTopology(const graphics::DrawModeParam & _mode)
{
	if (_mode == graphics::drawmode::TRIANGLES)
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	if (_mode == graphics::drawmode::TRIANGLE_STRIP)
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	if (_mode == graphics::drawmode::TRIANGLE_FAN)
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
	if (_mode == graphics::drawmode::LINES)
		return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
	return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
}
#endif

} // namespace

namespace vulkan {

struct ContextImpl::VulkanState
{
#if GLIDEN64_VULKAN_HEADERS_AVAILABLE
	struct FrameSync {
		VkSemaphore imageAvailable = VK_NULL_HANDLE;
		VkSemaphore renderFinished = VK_NULL_HANDLE;
		VkFence inFlight = VK_NULL_HANDLE;
		VkBuffer vertexBuffer = VK_NULL_HANDLE;
		VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
		VkDeviceSize vertexBufferCapacity = 0;
	};

	struct DrawVertex {
		f32 x = 0.0f;
		f32 y = 0.0f;
		f32 z = 0.0f;
		f32 w = 1.0f;
		f32 r = 1.0f;
		f32 g = 1.0f;
		f32 b = 1.0f;
		f32 a = 1.0f;
	};

	struct DrawBatch {
		VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		u32 firstVertex = 0;
		u32 vertexCount = 0;
		f32 lineWidth = 1.0f;
	};

	struct RasterState {
		s32 viewportX = 0;
		s32 viewportY = 0;
		s32 viewportWidth = 0;
		s32 viewportHeight = 0;
		s32 scissorX = 0;
		s32 scissorY = 0;
		s32 scissorWidth = 0;
		s32 scissorHeight = 0;
		bool scissorEnabled = false;
		std::array<f32, 4> clearColor = { 0.0f, 0.0f, 0.0f, 1.0f };
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
	std::vector<VkImage> swapchainImages;
	std::vector<VkImageView> swapchainImageViews;
	VkRenderPass renderPass = VK_NULL_HANDLE;
	std::vector<VkFramebuffer> swapchainFramebuffers;
	std::vector<VkFence> swapchainImageFences;
	VkCommandPool commandPool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> frameCommandBuffers;
	std::vector<FrameSync> frameSync;
	std::vector<DrawVertex> queuedVertices;
	std::vector<DrawBatch> queuedBatches;
	RasterState rasterState;
	VkShaderModule colorVertexShader = VK_NULL_HANDLE;
	VkShaderModule colorFragmentShader = VK_NULL_HANDLE;
	VkPipelineLayout colorPipelineLayout = VK_NULL_HANDLE;
	VkPipeline colorTrianglePipeline = VK_NULL_HANDLE;
	VkPipeline colorTriangleStripPipeline = VK_NULL_HANDLE;
	VkPipeline colorTriangleFanPipeline = VK_NULL_HANDLE;
	VkPipeline colorLinePipeline = VK_NULL_HANDLE;
	u32 frameSyncIndex = 0;
	bool wideLinesEnabled = false;
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
	, m_drawFramebufferBinding(graphics::ObjectHandle::defaultFramebuffer)
	, m_readFramebufferBinding(graphics::ObjectHandle::defaultFramebuffer)
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
	m_drawFramebufferBinding = graphics::ObjectHandle::defaultFramebuffer;
	m_readFramebufferBinding = graphics::ObjectHandle::defaultFramebuffer;
	if (!initializeVulkanCore()) {
		LOG(LOG_WARNING, "Vulkan core init failed. Backend remains in bootstrap mode.");
	}
}

void ContextImpl::destroy()
{
	shutdownVulkanCore();
	m_drawFramebufferBinding = graphics::ObjectHandle::defaultFramebuffer;
	m_readFramebufferBinding = graphics::ObjectHandle::defaultFramebuffer;
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
#if GLIDEN64_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->rasterState.viewportX = _x;
	m_vk->rasterState.viewportY = _y;
	m_vk->rasterState.viewportWidth = _width;
	m_vk->rasterState.viewportHeight = _height;
#else
	(void)_x;
	(void)_y;
	(void)_width;
	(void)_height;
#endif
}

void ContextImpl::setScissor(s32 _x, s32 _y, s32 _width, s32 _height)
{
#if GLIDEN64_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->rasterState.scissorX = _x;
	m_vk->rasterState.scissorY = _y;
	m_vk->rasterState.scissorWidth = _width;
	m_vk->rasterState.scissorHeight = _height;
	m_vk->rasterState.scissorEnabled = _width > 0 && _height > 0;
#else
	(void)_x;
	(void)_y;
	(void)_width;
	(void)_height;
#endif
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
#if GLIDEN64_VULKAN_HEADERS_AVAILABLE
	if (!m_vk || !isDefaultDrawFramebufferBound())
		return;
	m_vk->rasterState.clearColor[0] = _red;
	m_vk->rasterState.clearColor[1] = _green;
	m_vk->rasterState.clearColor[2] = _blue;
	m_vk->rasterState.clearColor[3] = _alpha;
#else
	(void)_red;
	(void)_green;
	(void)_blue;
	(void)_alpha;
#endif
}

void ContextImpl::clearDepthBuffer()
{
	// Depth attachment support will be introduced with Vulkan framebuffer targets.
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
	if (_target == graphics::bufferTarget::FRAMEBUFFER) {
		m_drawFramebufferBinding = _name;
		m_readFramebufferBinding = _name;
		return;
	}
	if (_target == graphics::bufferTarget::DRAW_FRAMEBUFFER) {
		m_drawFramebufferBinding = _name;
		return;
	}
	if (_target == graphics::bufferTarget::READ_FRAMEBUFFER)
		m_readFramebufferBinding = _name;
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
#if !GLIDEN64_VULKAN_HEADERS_AVAILABLE
	(void)_params;
	return;
#else
	if (!m_vk || !isDefaultDrawFramebufferBound())
		return;
	if (_params.vertices == nullptr || _params.verticesCount == 0)
		return;

	const VkPrimitiveTopology topology = toVkTopology(_params.mode);
	if (topology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM)
		return;

	const u32 firstVertex = static_cast<u32>(m_vk->queuedVertices.size());
	auto appendVertex = [this, &_params](u32 _index) {
		if (_index >= _params.verticesCount)
			return;
		const SPVertex & src = _params.vertices[_index];
		VulkanState::DrawVertex dst{};
		dst.x = src.x;
		dst.y = src.y;
		dst.z = src.z;
		dst.w = src.w;
		if (_params.flatColors) {
			dst.r = src.flat_r;
			dst.g = src.flat_g;
			dst.b = src.flat_b;
			dst.a = src.flat_a;
		} else {
			dst.r = src.r;
			dst.g = src.g;
			dst.b = src.b;
			dst.a = src.a;
		}
		m_vk->queuedVertices.push_back(dst);
	};

	if (_params.elements != nullptr && _params.elementsCount > 0) {
		if (_params.elementsType != graphics::datatype::UNSIGNED_SHORT) {
			static bool warnedUnsupportedIndexType = false;
			if (!warnedUnsupportedIndexType) {
				LOG(LOG_WARNING, "Vulkan draw path currently supports only UNSIGNED_SHORT indices; draw call skipped.");
				warnedUnsupportedIndexType = true;
			}
			return;
		}
		const u16 * elements = reinterpret_cast<const u16 *>(_params.elements);
		for (u32 i = 0; i < _params.elementsCount; ++i)
			appendVertex(elements[i]);
	} else {
		for (u32 i = 0; i < _params.verticesCount; ++i)
			appendVertex(i);
	}

	const u32 vertexCount = static_cast<u32>(m_vk->queuedVertices.size()) - firstVertex;
	if (vertexCount == 0)
		return;

	VulkanState::DrawBatch batch{};
	batch.topology = topology;
	batch.firstVertex = firstVertex;
	batch.vertexCount = vertexCount;
	m_vk->queuedBatches.push_back(batch);
#endif
}

void ContextImpl::drawRects(const graphics::Context::DrawRectParameters & _params)
{
#if !GLIDEN64_VULKAN_HEADERS_AVAILABLE
	(void)_params;
	return;
#else
	if (!m_vk || !isDefaultDrawFramebufferBound())
		return;
	if (_params.vertices == nullptr || _params.verticesCount == 0)
		return;

	const VkPrimitiveTopology topology = toVkTopology(_params.mode);
	if (topology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM)
		return;

	const u32 firstVertex = static_cast<u32>(m_vk->queuedVertices.size());
	for (u32 i = 0; i < _params.verticesCount; ++i) {
		const RectVertex & src = _params.vertices[i];
		VulkanState::DrawVertex dst{};
		dst.x = src.x;
		dst.y = src.y;
		dst.z = src.z;
		dst.w = src.w;
		dst.r = 1.0f;
		dst.g = 1.0f;
		dst.b = 1.0f;
		dst.a = 1.0f;
		m_vk->queuedVertices.push_back(dst);
	}

	VulkanState::DrawBatch batch{};
	batch.topology = topology;
	batch.firstVertex = firstVertex;
	batch.vertexCount = _params.verticesCount;
	m_vk->queuedBatches.push_back(batch);
#endif
}

void ContextImpl::drawLine(f32 _width, SPVertex * _vertices)
{
#if !GLIDEN64_VULKAN_HEADERS_AVAILABLE
	(void)_width;
	(void)_vertices;
	return;
#else
	if (!m_vk || !isDefaultDrawFramebufferBound())
		return;
	if (_vertices == nullptr)
		return;

	const u32 firstVertex = static_cast<u32>(m_vk->queuedVertices.size());
	for (u32 i = 0; i < 2; ++i) {
		const SPVertex & src = _vertices[i];
		VulkanState::DrawVertex dst{};
		dst.x = src.x;
		dst.y = src.y;
		dst.z = src.z;
		dst.w = src.w;
		dst.r = src.r;
		dst.g = src.g;
		dst.b = src.b;
		dst.a = src.a;
		m_vk->queuedVertices.push_back(dst);
	}

	VulkanState::DrawBatch batch{};
	batch.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
	batch.firstVertex = firstVertex;
	batch.vertexCount = 2;
	batch.lineWidth = std::max(1.0f, _width);
	m_vk->queuedBatches.push_back(batch);
#endif
}

f32 ContextImpl::getMaxLineWidth()
{
	return m_maxLineWidth;
}

bool ContextImpl::present()
{
#if !GLIDEN64_VULKAN_HEADERS_AVAILABLE
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

		const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(m_vk->queuedVertices.size()) * sizeof(VulkanState::DrawVertex);
		const bool hasDrawBatches = !m_vk->queuedVertices.empty() && !m_vk->queuedBatches.empty();
		if (hasDrawBatches) {
			if (m_vk->colorPipelineLayout == VK_NULL_HANDLE
				|| m_vk->colorTrianglePipeline == VK_NULL_HANDLE
				|| m_vk->colorTriangleStripPipeline == VK_NULL_HANDLE
				|| m_vk->colorTriangleFanPipeline == VK_NULL_HANDLE
				|| m_vk->colorLinePipeline == VK_NULL_HANDLE) {
				LOG(LOG_WARNING, "Vulkan draw pipelines are not initialized.");
				break;
			}
			if (!ensureFrameVertexBuffer(m_vk->frameSyncIndex, static_cast<size_t>(vertexBytes))) {
				LOG(LOG_WARNING, "Failed to allocate Vulkan frame vertex buffer.");
				break;
			}
			if (currentFrame.vertexBuffer == VK_NULL_HANDLE || currentFrame.vertexBufferMemory == VK_NULL_HANDLE) {
				LOG(LOG_WARNING, "Vulkan frame vertex buffer is not ready.");
				break;
			}
			void * mappedData = nullptr;
			if (vkMapMemory(m_vk->device, currentFrame.vertexBufferMemory, 0, vertexBytes, 0, &mappedData) != VK_SUCCESS || mappedData == nullptr) {
				LOG(LOG_WARNING, "vkMapMemory failed for frame vertex buffer.");
				break;
			}
			std::memcpy(mappedData, m_vk->queuedVertices.data(), static_cast<size_t>(vertexBytes));
			vkUnmapMemory(m_vk->device, currentFrame.vertexBufferMemory);
		}

		VkCommandBufferBeginInfo commandBufferBeginInfo{};
		commandBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		commandBufferBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (vkBeginCommandBuffer(commandBuffer, &commandBufferBeginInfo) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkBeginCommandBuffer failed.");
			break;
		}

		VkClearValue clearValue{};
		clearValue.color.float32[0] = m_vk->rasterState.clearColor[0];
		clearValue.color.float32[1] = m_vk->rasterState.clearColor[1];
		clearValue.color.float32[2] = m_vk->rasterState.clearColor[2];
		clearValue.color.float32[3] = m_vk->rasterState.clearColor[3];
		VkRenderPassBeginInfo renderPassBeginInfo{};
		renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		renderPassBeginInfo.renderPass = m_vk->renderPass;
		renderPassBeginInfo.framebuffer = m_vk->swapchainFramebuffers[currentImageIndex];
		renderPassBeginInfo.renderArea.offset = { 0, 0 };
		renderPassBeginInfo.renderArea.extent = m_vk->swapchainExtent;
		renderPassBeginInfo.clearValueCount = 1;
		renderPassBeginInfo.pClearValues = &clearValue;
		vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

		VkViewport viewport{};
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		if (m_vk->rasterState.viewportWidth > 0 && m_vk->rasterState.viewportHeight > 0) {
			const s32 vx = std::max<s32>(0, m_vk->rasterState.viewportX);
			const s32 vy = std::max<s32>(0, m_vk->rasterState.viewportY);
			const s32 vw = std::max<s32>(1, m_vk->rasterState.viewportWidth);
			const s32 vh = std::max<s32>(1, m_vk->rasterState.viewportHeight);
			viewport.x = static_cast<f32>(vx);
			viewport.y = static_cast<f32>(vy);
			viewport.width = static_cast<f32>(vw);
			viewport.height = static_cast<f32>(vh);
		} else {
			viewport.x = 0.0f;
			viewport.y = 0.0f;
			viewport.width = static_cast<f32>(m_vk->swapchainExtent.width);
			viewport.height = static_cast<f32>(m_vk->swapchainExtent.height);
		}
		vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

		VkRect2D scissor{};
		if (m_vk->rasterState.scissorEnabled) {
			const s32 sx = std::max<s32>(0, m_vk->rasterState.scissorX);
			const s32 sy = std::max<s32>(0, m_vk->rasterState.scissorY);
			const s32 sw = std::max<s32>(1, m_vk->rasterState.scissorWidth);
			const s32 sh = std::max<s32>(1, m_vk->rasterState.scissorHeight);
			if (static_cast<u32>(sx) >= m_vk->swapchainExtent.width || static_cast<u32>(sy) >= m_vk->swapchainExtent.height) {
				scissor.offset = { 0, 0 };
				scissor.extent = m_vk->swapchainExtent;
			} else {
				scissor.offset.x = sx;
				scissor.offset.y = sy;
				scissor.extent.width = std::min<u32>(static_cast<u32>(sw), m_vk->swapchainExtent.width - static_cast<u32>(sx));
				scissor.extent.height = std::min<u32>(static_cast<u32>(sh), m_vk->swapchainExtent.height - static_cast<u32>(sy));
			}
		} else {
			scissor.offset = { 0, 0 };
			scissor.extent = m_vk->swapchainExtent;
		}
		vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

		if (hasDrawBatches) {
			VkDeviceSize vertexBufferOffset = 0;
			vkCmdBindVertexBuffers(commandBuffer, 0, 1, &currentFrame.vertexBuffer, &vertexBufferOffset);

			for (const VulkanState::DrawBatch & batch : m_vk->queuedBatches) {
				if (batch.vertexCount == 0)
					continue;

				VkPipeline pipeline = VK_NULL_HANDLE;
				switch (batch.topology) {
				case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
					pipeline = m_vk->colorTrianglePipeline;
					break;
				case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
					pipeline = m_vk->colorTriangleStripPipeline;
					break;
				case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
					pipeline = m_vk->colorTriangleFanPipeline;
					break;
				case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
					pipeline = m_vk->colorLinePipeline;
					break;
				default:
					break;
				}
				if (pipeline == VK_NULL_HANDLE)
					continue;

				vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
				if (batch.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST) {
					const f32 lineWidth = m_vk->wideLinesEnabled
						? std::min(std::max(1.0f, batch.lineWidth), m_maxLineWidth)
						: 1.0f;
					vkCmdSetLineWidth(commandBuffer, lineWidth);
				}
				vkCmdDraw(commandBuffer, batch.vertexCount, 1, batch.firstVertex, 0);
			}
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

		m_vk->frameSyncIndex = (m_vk->frameSyncIndex + 1U) % static_cast<u32>(m_vk->frameSync.size());
		success = true;
	} while (false);

	resetFrameRenderData();
	return success;
#endif
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
#if defined(OS_WINDOWS)
		case graphics::Context::PresentationWindowInfo::WindowSystem::Win32:
			if (hasInstanceExtension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) {
				enabledInstanceExtensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
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
				enabledInstanceExtensions.insert(enabledInstanceExtensions.begin(), VK_KHR_SURFACE_EXTENSION_NAME);
				m_vk->surfaceExtensionEnabled = true;
			} else {
				LOG(LOG_WARNING, "Vulkan extension %s is unavailable; surface creation is disabled.", VK_KHR_SURFACE_EXTENSION_NAME);
				m_vk->xlibSurfaceExtensionEnabled = false;
				m_vk->win32SurfaceExtensionEnabled = false;
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
#if !GLIDEN64_VULKAN_HEADERS_AVAILABLE
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
#if !GLIDEN64_VULKAN_HEADERS_AVAILABLE
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
	if (!m_vk->wideLinesEnabled)
		m_maxLineWidth = 1.0f;
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
#if !GLIDEN64_VULKAN_HEADERS_AVAILABLE
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

	m_vk->frameSyncIndex = 0;
	return true;
#endif
}

bool ContextImpl::createDrawResources()
{
#if !GLIDEN64_VULKAN_HEADERS_AVAILABLE
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
	return true;
#endif
}

bool ContextImpl::createShaderModules()
{
#if !GLIDEN64_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk || m_vk->device == VK_NULL_HANDLE)
		return false;

	auto createShaderModule = [this](const unsigned char * _bytes, size_t _size, VkShaderModule & _outShader) {
		std::vector<u32> alignedCode((_size + sizeof(u32) - 1U) / sizeof(u32), 0U);
		std::memcpy(alignedCode.data(), _bytes, _size);

		VkShaderModuleCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		createInfo.codeSize = _size;
		createInfo.pCode = alignedCode.data();
		return vkCreateShaderModule(m_vk->device, &createInfo, nullptr, &_outShader) == VK_SUCCESS;
	};

	if (!createShaderModule(kBasicColorVertSpv, kBasicColorVertSpv_len, m_vk->colorVertexShader))
		return false;
	if (!createShaderModule(kBasicColorFragSpv, kBasicColorFragSpv_len, m_vk->colorFragmentShader))
		return false;
	return true;
#endif
}

bool ContextImpl::createPipelines()
{
#if !GLIDEN64_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk || m_vk->device == VK_NULL_HANDLE || m_vk->renderPass == VK_NULL_HANDLE)
		return false;
	if (m_vk->colorVertexShader == VK_NULL_HANDLE || m_vk->colorFragmentShader == VK_NULL_HANDLE)
		return false;

	VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{};
	pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	if (vkCreatePipelineLayout(m_vk->device, &pipelineLayoutCreateInfo, nullptr, &m_vk->colorPipelineLayout) != VK_SUCCESS)
		return false;

	const VkVertexInputBindingDescription bindingDescription = {
		0,
		static_cast<u32>(sizeof(VulkanState::DrawVertex)),
		VK_VERTEX_INPUT_RATE_VERTEX
	};
	const VkVertexInputAttributeDescription attributeDescriptions[2] = {
		{ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 }
	};
	VkPipelineVertexInputStateCreateInfo vertexInputStateCreateInfo{};
	vertexInputStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputStateCreateInfo.vertexBindingDescriptionCount = 1;
	vertexInputStateCreateInfo.pVertexBindingDescriptions = &bindingDescription;
	vertexInputStateCreateInfo.vertexAttributeDescriptionCount = 2;
	vertexInputStateCreateInfo.pVertexAttributeDescriptions = attributeDescriptions;

	VkPipelineViewportStateCreateInfo viewportStateCreateInfo{};
	viewportStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportStateCreateInfo.viewportCount = 1;
	viewportStateCreateInfo.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rasterizationStateCreateInfo{};
	rasterizationStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizationStateCreateInfo.depthClampEnable = VK_FALSE;
	rasterizationStateCreateInfo.rasterizerDiscardEnable = VK_FALSE;
	rasterizationStateCreateInfo.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizationStateCreateInfo.cullMode = VK_CULL_MODE_NONE;
	rasterizationStateCreateInfo.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizationStateCreateInfo.depthBiasEnable = VK_FALSE;
	rasterizationStateCreateInfo.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisampleStateCreateInfo{};
	multisampleStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampleStateCreateInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	multisampleStateCreateInfo.sampleShadingEnable = VK_FALSE;

	VkPipelineColorBlendAttachmentState colorBlendAttachmentState{};
	colorBlendAttachmentState.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	colorBlendAttachmentState.blendEnable = VK_TRUE;
	colorBlendAttachmentState.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	colorBlendAttachmentState.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	colorBlendAttachmentState.colorBlendOp = VK_BLEND_OP_ADD;
	colorBlendAttachmentState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	colorBlendAttachmentState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	colorBlendAttachmentState.alphaBlendOp = VK_BLEND_OP_ADD;

	VkPipelineColorBlendStateCreateInfo colorBlendStateCreateInfo{};
	colorBlendStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlendStateCreateInfo.logicOpEnable = VK_FALSE;
	colorBlendStateCreateInfo.attachmentCount = 1;
	colorBlendStateCreateInfo.pAttachments = &colorBlendAttachmentState;

	const VkDynamicState dynamicStates[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
		VK_DYNAMIC_STATE_LINE_WIDTH
	};
	VkPipelineDynamicStateCreateInfo dynamicStateCreateInfo{};
	dynamicStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicStateCreateInfo.dynamicStateCount = static_cast<u32>(std::size(dynamicStates));
	dynamicStateCreateInfo.pDynamicStates = dynamicStates;

	const VkPipelineShaderStageCreateInfo shaderStages[2] = {
		{
			VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			nullptr,
			0,
			VK_SHADER_STAGE_VERTEX_BIT,
			m_vk->colorVertexShader,
			"main",
			nullptr
		},
		{
			VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			nullptr,
			0,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			m_vk->colorFragmentShader,
			"main",
			nullptr
		}
	};

	auto createPipeline = [&](VkPrimitiveTopology _topology, VkPipeline & _pipeline) {
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateCreateInfo{};
		inputAssemblyStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssemblyStateCreateInfo.topology = _topology;
		inputAssemblyStateCreateInfo.primitiveRestartEnable = VK_FALSE;

		VkGraphicsPipelineCreateInfo pipelineCreateInfo{};
		pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineCreateInfo.stageCount = 2;
		pipelineCreateInfo.pStages = shaderStages;
		pipelineCreateInfo.pVertexInputState = &vertexInputStateCreateInfo;
		pipelineCreateInfo.pInputAssemblyState = &inputAssemblyStateCreateInfo;
		pipelineCreateInfo.pViewportState = &viewportStateCreateInfo;
		pipelineCreateInfo.pRasterizationState = &rasterizationStateCreateInfo;
		pipelineCreateInfo.pMultisampleState = &multisampleStateCreateInfo;
		pipelineCreateInfo.pColorBlendState = &colorBlendStateCreateInfo;
		pipelineCreateInfo.pDynamicState = &dynamicStateCreateInfo;
		pipelineCreateInfo.layout = m_vk->colorPipelineLayout;
		pipelineCreateInfo.renderPass = m_vk->renderPass;
		pipelineCreateInfo.subpass = 0;
		return vkCreateGraphicsPipelines(m_vk->device, VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr, &_pipeline) == VK_SUCCESS;
	};

	if (!createPipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, m_vk->colorTrianglePipeline))
		return false;
	if (!createPipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, m_vk->colorTriangleStripPipeline))
		return false;
	if (!createPipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, m_vk->colorTriangleFanPipeline))
		return false;
	if (!createPipeline(VK_PRIMITIVE_TOPOLOGY_LINE_LIST, m_vk->colorLinePipeline))
		return false;
	return true;
#endif
}

void ContextImpl::destroyDrawResources()
{
#if GLIDEN64_VULKAN_HEADERS_AVAILABLE
	if (!m_vk || m_vk->device == VK_NULL_HANDLE)
		return;

	if (m_vk->colorTrianglePipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(m_vk->device, m_vk->colorTrianglePipeline, nullptr);
		m_vk->colorTrianglePipeline = VK_NULL_HANDLE;
	}
	if (m_vk->colorTriangleStripPipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(m_vk->device, m_vk->colorTriangleStripPipeline, nullptr);
		m_vk->colorTriangleStripPipeline = VK_NULL_HANDLE;
	}
	if (m_vk->colorTriangleFanPipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(m_vk->device, m_vk->colorTriangleFanPipeline, nullptr);
		m_vk->colorTriangleFanPipeline = VK_NULL_HANDLE;
	}
	if (m_vk->colorLinePipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(m_vk->device, m_vk->colorLinePipeline, nullptr);
		m_vk->colorLinePipeline = VK_NULL_HANDLE;
	}
	if (m_vk->colorPipelineLayout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(m_vk->device, m_vk->colorPipelineLayout, nullptr);
		m_vk->colorPipelineLayout = VK_NULL_HANDLE;
	}
	if (m_vk->colorVertexShader != VK_NULL_HANDLE) {
		vkDestroyShaderModule(m_vk->device, m_vk->colorVertexShader, nullptr);
		m_vk->colorVertexShader = VK_NULL_HANDLE;
	}
	if (m_vk->colorFragmentShader != VK_NULL_HANDLE) {
		vkDestroyShaderModule(m_vk->device, m_vk->colorFragmentShader, nullptr);
		m_vk->colorFragmentShader = VK_NULL_HANDLE;
	}
#endif
}

void ContextImpl::resetFrameRenderData()
{
#if GLIDEN64_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->queuedVertices.clear();
	m_vk->queuedBatches.clear();
#endif
}

bool ContextImpl::ensureFrameVertexBuffer(u32 _frameIndex, size_t _requiredBytes)
{
#if !GLIDEN64_VULKAN_HEADERS_AVAILABLE
	(void)_frameIndex;
	(void)_requiredBytes;
	return false;
#else
	if (!m_vk || m_vk->device == VK_NULL_HANDLE || _frameIndex >= m_vk->frameSync.size())
		return false;
	if (_requiredBytes == 0)
		return true;

	VulkanState::FrameSync & frame = m_vk->frameSync[_frameIndex];
	if (frame.vertexBuffer != VK_NULL_HANDLE && frame.vertexBufferMemory != VK_NULL_HANDLE && frame.vertexBufferCapacity >= _requiredBytes)
		return true;

	if (frame.vertexBuffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(m_vk->device, frame.vertexBuffer, nullptr);
		frame.vertexBuffer = VK_NULL_HANDLE;
	}
	if (frame.vertexBufferMemory != VK_NULL_HANDLE) {
		vkFreeMemory(m_vk->device, frame.vertexBufferMemory, nullptr);
		frame.vertexBufferMemory = VK_NULL_HANDLE;
	}
	frame.vertexBufferCapacity = 0;

	const VkDeviceSize requestedSize = std::max<VkDeviceSize>(kInitialFrameVertexBufferSize, static_cast<VkDeviceSize>(_requiredBytes));
	VkBufferCreateInfo bufferCreateInfo{};
	bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferCreateInfo.size = requestedSize;
	bufferCreateInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(m_vk->device, &bufferCreateInfo, nullptr, &frame.vertexBuffer) != VK_SUCCESS)
		return false;

	VkMemoryRequirements memoryRequirements{};
	vkGetBufferMemoryRequirements(m_vk->device, frame.vertexBuffer, &memoryRequirements);
	const u32 memoryType = findMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (memoryType == UINT32_MAX) {
		vkDestroyBuffer(m_vk->device, frame.vertexBuffer, nullptr);
		frame.vertexBuffer = VK_NULL_HANDLE;
		return false;
	}

	VkMemoryAllocateInfo allocateInfo{};
	allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocateInfo.allocationSize = memoryRequirements.size;
	allocateInfo.memoryTypeIndex = memoryType;
	if (vkAllocateMemory(m_vk->device, &allocateInfo, nullptr, &frame.vertexBufferMemory) != VK_SUCCESS) {
		vkDestroyBuffer(m_vk->device, frame.vertexBuffer, nullptr);
		frame.vertexBuffer = VK_NULL_HANDLE;
		return false;
	}
	if (vkBindBufferMemory(m_vk->device, frame.vertexBuffer, frame.vertexBufferMemory, 0) != VK_SUCCESS) {
		vkDestroyBuffer(m_vk->device, frame.vertexBuffer, nullptr);
		vkFreeMemory(m_vk->device, frame.vertexBufferMemory, nullptr);
		frame.vertexBuffer = VK_NULL_HANDLE;
		frame.vertexBufferMemory = VK_NULL_HANDLE;
		return false;
	}

	frame.vertexBufferCapacity = static_cast<size_t>(requestedSize);
	return true;
#endif
}

u32 ContextImpl::findMemoryType(u32 _typeFilter, u32 _propertyFlags) const
{
#if !GLIDEN64_VULKAN_HEADERS_AVAILABLE
	(void)_typeFilter;
	(void)_propertyFlags;
	return UINT32_MAX;
#else
	if (!m_vk || m_vk->physicalDevice == VK_NULL_HANDLE)
		return UINT32_MAX;

	VkPhysicalDeviceMemoryProperties memoryProperties{};
	vkGetPhysicalDeviceMemoryProperties(m_vk->physicalDevice, &memoryProperties);
	for (u32 i = 0; i < memoryProperties.memoryTypeCount; ++i) {
		const bool typeMatch = (_typeFilter & (1U << i)) != 0U;
		const bool propertyMatch = (memoryProperties.memoryTypes[i].propertyFlags & _propertyFlags) == _propertyFlags;
		if (typeMatch && propertyMatch)
			return i;
	}
	return UINT32_MAX;
#endif
}

bool ContextImpl::isDefaultDrawFramebufferBound() const
{
	return m_drawFramebufferBinding == graphics::ObjectHandle::defaultFramebuffer;
}

void ContextImpl::createSwapchain()
{
#if GLIDEN64_VULKAN_HEADERS_AVAILABLE
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

	VkAttachmentDescription colorAttachment{};
	colorAttachment.format = selectedFormat.format;
	colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkAttachmentReference colorAttachmentReference{};
	colorAttachmentReference.attachment = 0;
	colorAttachmentReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpassDescription{};
	subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpassDescription.colorAttachmentCount = 1;
	subpassDescription.pColorAttachments = &colorAttachmentReference;

	VkSubpassDependency subpassDependency{};
	subpassDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	subpassDependency.dstSubpass = 0;
	subpassDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	subpassDependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	subpassDependency.srcAccessMask = 0;
	subpassDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo renderPassCreateInfo{};
	renderPassCreateInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassCreateInfo.attachmentCount = 1;
	renderPassCreateInfo.pAttachments = &colorAttachment;
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
	for (VkImageView imageView : m_vk->swapchainImageViews) {
		VkFramebufferCreateInfo framebufferCreateInfo{};
		framebufferCreateInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebufferCreateInfo.renderPass = m_vk->renderPass;
		framebufferCreateInfo.attachmentCount = 1;
		framebufferCreateInfo.pAttachments = &imageView;
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
	resetFrameRenderData();

	LOG(LOG_VERBOSE, "Vulkan swapchain created: %ux%u", extent.width, extent.height);
#endif
}

void ContextImpl::destroySwapchain()
{
#if GLIDEN64_VULKAN_HEADERS_AVAILABLE
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
	m_vk->swapchainExtent = {};
	resetFrameRenderData();

	if (m_vk->swapchain == VK_NULL_HANDLE)
		return;
	vkDestroySwapchainKHR(m_vk->device, m_vk->swapchain, nullptr);
	m_vk->swapchain = VK_NULL_HANDLE;
#endif
}

void ContextImpl::destroyPresentSyncObjects()
{
#if GLIDEN64_VULKAN_HEADERS_AVAILABLE
	if (!m_vk || m_vk->device == VK_NULL_HANDLE)
		return;

	for (VulkanState::FrameSync & frame : m_vk->frameSync) {
		if (frame.vertexBuffer != VK_NULL_HANDLE) {
			vkDestroyBuffer(m_vk->device, frame.vertexBuffer, nullptr);
			frame.vertexBuffer = VK_NULL_HANDLE;
		}
		if (frame.vertexBufferMemory != VK_NULL_HANDLE) {
			vkFreeMemory(m_vk->device, frame.vertexBufferMemory, nullptr);
			frame.vertexBufferMemory = VK_NULL_HANDLE;
		}
		frame.vertexBufferCapacity = 0;

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
