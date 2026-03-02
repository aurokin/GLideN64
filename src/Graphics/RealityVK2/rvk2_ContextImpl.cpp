#include "rvk2_ContextImpl.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include <Log.h>
#include <Graphics/Parameters.h>
#include <N64.h>

#include "rvk2_Runtime.h"

namespace {

void buildFullscreenRect(RectVertex (&_vertices)[4])
{
	_vertices[0] = RectVertex{-1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f};
	_vertices[1] = RectVertex{1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
	_vertices[2] = RectVertex{-1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f};
	_vertices[3] = RectVertex{1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f};
}

u32 readRegValue(const u32 * _reg)
{
	return _reg != nullptr ? *_reg : 0U;
}

rvk2::ExecutorConfig buildExecutorConfigFromVIRegisters()
{
	rvk2::ExecutorConfig config = rvk2::loadExecutorConfigFromEnv();
	if (REG.VI_STATUS == nullptr || REG.VI_ORIGIN == nullptr)
		return config;

	config.viRegistersValid = true;
	config.viStatus = readRegValue(REG.VI_STATUS);
	config.viOrigin = readRegValue(REG.VI_ORIGIN);
	config.viWidth = readRegValue(REG.VI_WIDTH);
	config.viVCurrentLine = readRegValue(REG.VI_V_CURRENT_LINE);
	config.viVSync = readRegValue(REG.VI_V_SYNC);
	config.viHStart = readRegValue(REG.VI_H_START);
	config.viVStart = readRegValue(REG.VI_V_START);
	config.viXScale = readRegValue(REG.VI_X_SCALE);
	config.viYScale = readRegValue(REG.VI_Y_SCALE);
	return config;
}

void writeTextureReplacementSummaryFile(
	const rvk2::ExecutorConfig & _config,
	const rvk2::ExecutorSummary & _summary)
{
	if (_config.textureReplacementSummaryPath.empty())
		return;

	std::FILE * file = std::fopen(_config.textureReplacementSummaryPath.c_str(), "wb");
	if (file == nullptr)
		return;

	const double hitRate = _summary.textureReplacementSampleCount > 0ULL
		? static_cast<double>(_summary.textureReplacementHitCount)
			/ static_cast<double>(_summary.textureReplacementSampleCount)
		: 0.0;
	std::fprintf(
		file,
		"enabled=%u\nentries=%llu\npixels=%llu\nsamples=%llu\nhits=%llu\nmisses=%llu\nhit_rate=%.6f\npresent_hash=0x%016llX\npresent_width=%u\npresent_height=%u\n",
		_summary.textureReplacementEnabled ? 1U : 0U,
		static_cast<unsigned long long>(_summary.textureReplacementEntryCount),
		static_cast<unsigned long long>(_summary.textureReplacementPixelCount),
		static_cast<unsigned long long>(_summary.textureReplacementSampleCount),
		static_cast<unsigned long long>(_summary.textureReplacementHitCount),
		static_cast<unsigned long long>(_summary.textureReplacementMissCount),
		hitRate,
		static_cast<unsigned long long>(_summary.presentHash),
		_summary.presentWidth,
		_summary.presentHeight);
	std::fclose(file);
}

} // namespace

namespace rvk2 {

ContextImpl::ContextImpl()
	: vulkan::ContextImpl()
	, m_windowInfo()
	, m_presentTexture(graphics::ObjectHandle::null)
	, m_presentTextureWidth(0U)
	, m_presentTextureHeight(0U)
	, m_presentUploadBytes()
	, m_executor(loadExecutorConfigFromEnv())
{
}

ContextImpl::~ContextImpl() = default;

void ContextImpl::setPresentationWindowInfo(const graphics::Context::PresentationWindowInfo & _info)
{
	m_windowInfo = _info;
	vulkan::ContextImpl::setPresentationWindowInfo(_info);
}

void ContextImpl::init()
{
	vulkan::ContextImpl::init();
}

void ContextImpl::destroy()
{
	if (m_presentTexture.isNotNull()) {
		vulkan::ContextImpl::deleteTexture(m_presentTexture);
		m_presentTexture = graphics::ObjectHandle::null;
	}
	m_presentTextureWidth = 0U;
	m_presentTextureHeight = 0U;
	m_presentUploadBytes.clear();
	vulkan::ContextImpl::destroy();
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

void ContextImpl::setBlendingSeparate(
	graphics::BlendParam _sfactorcolor,
	graphics::BlendParam _dfactorcolor,
	graphics::BlendParam _sfactoralpha,
	graphics::BlendParam _dfactoralpha)
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

void ContextImpl::setPolygonOffset(f32 _factor, f32 _units)
{
	(void)_factor;
	(void)_units;
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

void ContextImpl::convertToRgbaBytes(const std::vector<u32> & _srcPixels, std::vector<u8> & _dstBytes)
{
	_dstBytes.resize(_srcPixels.size() * 4U);
	for (size_t i = 0; i < _srcPixels.size(); ++i) {
		const u32 pixel = _srcPixels[i];
		const size_t base = i * 4U;
		_dstBytes[base + 0U] = static_cast<u8>((pixel >> 24U) & 0xFFU);
		_dstBytes[base + 1U] = static_cast<u8>((pixel >> 16U) & 0xFFU);
		_dstBytes[base + 2U] = static_cast<u8>((pixel >> 8U) & 0xFFU);
		_dstBytes[base + 3U] = static_cast<u8>((pixel >> 0U) & 0xFFU);
	}
}

void ContextImpl::ensurePresenterTexture(u32 _width, u32 _height)
{
	if (!m_presentTexture.isNotNull()) {
		m_presentTexture = vulkan::ContextImpl::createTexture(graphics::textureTarget::TEXTURE_2D);
		graphics::Context::TexParameters texParams{};
		texParams.handle = m_presentTexture;
		texParams.textureUnitIndex = graphics::textureIndices::Tex[0];
		texParams.target = graphics::textureTarget::TEXTURE_2D;
		texParams.magFilter = graphics::textureParameters::FILTER_NEAREST;
		texParams.minFilter = graphics::textureParameters::FILTER_NEAREST;
		texParams.wrapS = graphics::textureParameters::WRAP_CLAMP_TO_EDGE;
		texParams.wrapT = graphics::textureParameters::WRAP_CLAMP_TO_EDGE;
		texParams.maxMipmapLevel = graphics::Parameter(0U);
		texParams.maxAnisotropy = graphics::Parameter(0U);
		vulkan::ContextImpl::setTextureParameters(texParams);
	}

	if (_width == m_presentTextureWidth && _height == m_presentTextureHeight)
		return;

	graphics::Context::InitTextureParams initParams{};
	initParams.handle = m_presentTexture;
	initParams.textureUnitIndex = graphics::textureIndices::Tex[0];
	initParams.target = graphics::textureTarget::TEXTURE_2D;
	initParams.width = _width;
	initParams.height = _height;
	initParams.mipMapLevel = 0U;
	initParams.mipMapLevels = 1U;
	initParams.format = graphics::colorFormat::RGBA;
	initParams.internalFormat =
		vulkan::ContextImpl::convertInternalTextureFormat(static_cast<u32>(graphics::internalcolorFormat::RGBA8));
	initParams.dataType = graphics::datatype::UNSIGNED_BYTE;
	initParams.data = nullptr;
	vulkan::ContextImpl::init2DTexture(initParams);
	m_presentTextureWidth = _width;
	m_presentTextureHeight = _height;
}

void ContextImpl::uploadPresenterTexture(const ExecutorPresentFrame & _frame)
{
	if (_frame.width == 0U || _frame.height == 0U || _frame.pixels.empty())
		return;
	ensurePresenterTexture(_frame.width, _frame.height);
	convertToRgbaBytes(_frame.pixels, m_presentUploadBytes);

	graphics::Context::UpdateTextureDataParams updateParams{};
	updateParams.handle = m_presentTexture;
	updateParams.textureUnitIndex = graphics::textureIndices::Tex[0];
	updateParams.x = 0U;
	updateParams.y = 0U;
	updateParams.width = _frame.width;
	updateParams.height = _frame.height;
	updateParams.mipMapLevel = 0U;
	updateParams.format = graphics::colorFormat::RGBA;
	updateParams.internalFormat =
		vulkan::ContextImpl::convertInternalTextureFormat(static_cast<u32>(graphics::internalcolorFormat::RGBA8));
	updateParams.dataType = graphics::datatype::UNSIGNED_BYTE;
	updateParams.data = m_presentUploadBytes.data();
	vulkan::ContextImpl::update2DTexture(updateParams);
}

void ContextImpl::renderPresentedFrame(const ExecutorOutput & _output)
{
	if (_output.presentFrame.width == 0U
		|| _output.presentFrame.height == 0U
		|| _output.presentFrame.pixels.empty()) {
		const u32 viewportWidth = std::max<u32>(1U, m_windowInfo.width > 0U ? m_windowInfo.width : 1U);
		const u32 viewportHeight = std::max<u32>(1U, m_windowInfo.height > 0U ? m_windowInfo.height : 1U);
		vulkan::ContextImpl::bindFramebuffer(graphics::bufferTarget::DRAW_FRAMEBUFFER, graphics::ObjectHandle::defaultFramebuffer);
		vulkan::ContextImpl::setViewport(0, 0, static_cast<s32>(viewportWidth), static_cast<s32>(viewportHeight));
		vulkan::ContextImpl::setScissor(0, 0, static_cast<s32>(viewportWidth), static_cast<s32>(viewportHeight));
		vulkan::ContextImpl::enable(graphics::enable::SCISSOR_TEST, false);
		vulkan::ContextImpl::clearColorBuffer(0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}

	uploadPresenterTexture(_output.presentFrame);

	graphics::Context::BindTextureParameters bindParams{};
	bindParams.texture = m_presentTexture;
	bindParams.textureUnitIndex = graphics::textureIndices::Tex[0];
	bindParams.target = graphics::textureTarget::TEXTURE_2D;
	vulkan::ContextImpl::bindTexture(bindParams);

	const u32 viewportWidth = std::max<u32>(1U, m_windowInfo.width > 0U ? m_windowInfo.width : _output.presentFrame.width);
	const u32 viewportHeight = std::max<u32>(1U, m_windowInfo.height > 0U ? m_windowInfo.height : _output.presentFrame.height);
	vulkan::ContextImpl::bindFramebuffer(graphics::bufferTarget::DRAW_FRAMEBUFFER, graphics::ObjectHandle::defaultFramebuffer);
	vulkan::ContextImpl::setViewport(0, 0, static_cast<s32>(viewportWidth), static_cast<s32>(viewportHeight));
	vulkan::ContextImpl::setScissor(0, 0, static_cast<s32>(viewportWidth), static_cast<s32>(viewportHeight));
	vulkan::ContextImpl::enable(graphics::enable::SCISSOR_TEST, true);
	vulkan::ContextImpl::enable(graphics::enable::CULL_FACE, false);
	vulkan::ContextImpl::enable(graphics::enable::BLEND, false);
	vulkan::ContextImpl::enable(graphics::enable::DEPTH_TEST, false);
	vulkan::ContextImpl::enableDepthWrite(false);
	vulkan::ContextImpl::setDepthCompare(graphics::compare::ALWAYS);

	RectVertex vertices[4]{};
	buildFullscreenRect(vertices);

	graphics::Context::DrawRectParameters drawParams{};
	drawParams.mode = graphics::drawmode::TRIANGLE_STRIP;
	drawParams.texrect = true;
	drawParams.verticesCount = 4U;
	drawParams.vertices = vertices;
	drawParams.combiner = nullptr;
	vulkan::ContextImpl::drawRects(drawParams);
}

bool ContextImpl::present()
{
	const ExecutorConfig config = buildExecutorConfigFromVIRegisters();
	m_executor.updateConfig(config);
	const ExecutorOutput output =
		m_executor.executeWithOutput(runtime().renderPlan(), runtime().submissionPlan());
	if (config.textureReplacementLogSummary && output.summary.textureReplacementEnabled) {
		LOG(
			LOG_WARNING,
			"rvk2 tx summary: enabled=1 entries=%llu pixels=%llu samples=%llu hits=%llu misses=%llu",
			static_cast<unsigned long long>(output.summary.textureReplacementEntryCount),
			static_cast<unsigned long long>(output.summary.textureReplacementPixelCount),
			static_cast<unsigned long long>(output.summary.textureReplacementSampleCount),
			static_cast<unsigned long long>(output.summary.textureReplacementHitCount),
			static_cast<unsigned long long>(output.summary.textureReplacementMissCount));
	}
	writeTextureReplacementSummaryFile(config, output.summary);
	renderPresentedFrame(output);
	return vulkan::ContextImpl::present();
}

} // namespace rvk2
