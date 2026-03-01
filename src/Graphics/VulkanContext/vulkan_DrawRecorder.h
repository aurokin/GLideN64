#pragma once

#include <array>
#include <vector>
#include <algorithm>
#include <cmath>
#include <utility>
#include <Types.h>
#include <Graphics/Context.h>
#include <Graphics/ObjectHandle.h>
#include <Graphics/Parameters.h>
#include "vulkan_BindingLimits.h"
#include "vulkan_DrawShaderConfig.h"

namespace vulkan {

enum class PrimitiveType : u8 {
	Triangles,
	TriangleStrip,
	TriangleFan,
	Lines
};

enum class BlendFactor : u8 {
	Zero,
	One,
	SrcAlpha,
	OneMinusSrcAlpha,
	DstAlpha,
	ConstantAlpha,
	OneMinusConstantAlpha,
	Src1Color,
	OneMinusSrc1Color,
	Src1Alpha,
	OneMinusSrc1Alpha
};

enum class CompareMode : u8 {
	kLess,
	kLessOrEqual,
	kAlways
};

enum class CullMode : u8 {
	kNone,
	kFront,
	kBack,
	kFrontAndBack
};

namespace draw_dirty {
constexpr u32 kNone = 0U;
constexpr u32 kViewport = 1U << 0;
constexpr u32 kScissor = 1U << 1;
constexpr u32 kBlend = 1U << 2;
constexpr u32 kDepth = 1U << 3;
constexpr u32 kCull = 1U << 4;
constexpr u32 kLineWidth = 1U << 5;
constexpr u32 kAll = kViewport | kScissor | kBlend | kDepth | kCull | kLineWidth;
}

struct DrawVertex {
	f32 x = 0.0f;
	f32 y = 0.0f;
	f32 z = 0.0f;
	f32 w = 1.0f;
	f32 r = 1.0f;
	f32 g = 1.0f;
	f32 b = 1.0f;
	f32 a = 1.0f;
	f32 s0 = 0.0f;
	f32 t0 = 0.0f;
	f32 s1 = 0.0f;
	f32 t1 = 0.0f;
	f32 bc0 = 0.0f;
	f32 bc1 = 0.0f;
	u32 modify = 0U;
};

enum class VertexTransformMode : u8 {
	Rect,
	Triangle
};

struct RasterState {
	s32 viewportX = 0;
	s32 viewportY = 0;
	s32 viewportWidth = 0;
	s32 viewportHeight = 0;
	bool viewportValid = false;
	s32 scissorX = 0;
	s32 scissorY = 0;
	s32 scissorWidth = 0;
	s32 scissorHeight = 0;
	bool scissorEnabled = false;
};

struct BlendState {
	bool enabled = false;
	BlendFactor srcColor = BlendFactor::One;
	BlendFactor dstColor = BlendFactor::Zero;
	BlendFactor srcAlpha = BlendFactor::One;
	BlendFactor dstAlpha = BlendFactor::Zero;
	std::array<f32, 4> blendColor = { 0.0f, 0.0f, 0.0f, 0.0f };
};

struct DepthState {
	bool testEnabled = false;
	bool writeEnabled = true;
	CompareMode compare = CompareMode::kLessOrEqual;
	bool polygonOffsetEnabled = false;
	f32 polygonOffsetFactor = 0.0f;
	f32 polygonOffsetUnits = 0.0f;
};

struct DrawStateCache {
	RasterState raster;
	BlendState blend;
	DepthState depth;
	CullMode cullMode = CullMode::kNone;
};

struct TextureSlotReference {
	u32 unit = 0;
	graphics::ObjectHandle texture = graphics::ObjectHandle::null;
	graphics::TextureTargetParam target = graphics::textureTarget::TEXTURE_2D;
};

struct ImageSlotReference {
	u32 unit = 0;
	graphics::ObjectHandle texture = graphics::ObjectHandle::null;
	graphics::ImageAccessModeParam accessMode = graphics::textureImageAccessMode::READ_ONLY;
	graphics::InternalColorFormatParam textureFormat = graphics::internalcolorFormat::NOCOLOR;
};

struct DrawPacket {
	PrimitiveType primitive = PrimitiveType::Triangles;
	DrawStateCache state;
	VertexTransformMode transformMode = VertexTransformMode::Rect;
	bool forceRasterRectTransform = false;
	bool positionsNormalized = false;
	u64 debugPacketId = 0;
	u64 debugCombinerMux = 0;
	u32 debugCombinerCycleType = 0;
	u32 debugSource = 0;
	bool debugTexrect = false;
	std::vector<DrawVertex> vertices;
	std::array<TextureSlotReference, binding_limits::kTextureUnits> textureSlots;
	u32 textureSlotMask = 0;
	u32 textureUnit0 = 0;
	u32 textureUnit1 = 1;
	u32 shaderFlags = draw_shader_flags::kShade;
	u32 texrectAlphaTest = 0U;
	u32 texrectFilterMode = 0U;
	u32 blendMux1Packed = 0U;
	u32 blendMux2Packed = 0U;
	u32 blendParamsPacked = 0U;
	f32 texrectTextureWidth = 1.0f;
	f32 texrectTextureHeight = 1.0f;
	f32 gammaLevel = 2.0f;
	f32 textColorR = 1.0f;
	f32 textColorG = 1.0f;
	f32 textColorB = 1.0f;
	f32 textColorA = 1.0f;
	f32 fogColorR = 0.0f;
	f32 fogColorG = 0.0f;
	f32 fogColorB = 0.0f;
	f32 fogColorA = 1.0f;
	std::array<ImageSlotReference, binding_limits::kImageUnits> imageSlots;
	u32 imageSlotMask = 0;
	f32 lineWidth = 1.0f;
	u32 dirtyMask = draw_dirty::kNone;
};

class DrawRecorder
{
public:
	void resetFramePackets()
	{
		m_packets.clear();
	}

	void setViewport(s32 _x, s32 _y, s32 _width, s32 _height)
	{
		RasterState & raster = m_state.raster;
		if (raster.viewportX == _x
			&& raster.viewportY == _y
			&& raster.viewportWidth == _width
			&& raster.viewportHeight == _height
			&& raster.viewportValid) {
			return;
		}
		raster.viewportX = _x;
		raster.viewportY = _y;
		raster.viewportWidth = _width;
		raster.viewportHeight = _height;
		raster.viewportValid = true;
		m_dirtyMask |= draw_dirty::kViewport;
	}

	void setScissor(s32 _x, s32 _y, s32 _width, s32 _height)
	{
		RasterState & raster = m_state.raster;
		if (raster.scissorX == _x
			&& raster.scissorY == _y
			&& raster.scissorWidth == _width
			&& raster.scissorHeight == _height) {
			return;
		}
		raster.scissorX = _x;
		raster.scissorY = _y;
		raster.scissorWidth = _width;
		raster.scissorHeight = _height;
		m_dirtyMask |= draw_dirty::kScissor;
	}

	void setBlending(graphics::BlendParam _src, graphics::BlendParam _dst)
	{
		const BlendFactor src = _toBlendFactor(_src);
		const BlendFactor dst = _toBlendFactor(_dst);
		if (m_state.blend.srcColor == src
			&& m_state.blend.dstColor == dst
			&& m_state.blend.srcAlpha == src
			&& m_state.blend.dstAlpha == dst) {
			return;
		}
		m_state.blend.srcColor = src;
		m_state.blend.dstColor = dst;
		m_state.blend.srcAlpha = src;
		m_state.blend.dstAlpha = dst;
		m_dirtyMask |= draw_dirty::kBlend;
	}

	void setBlendingSeparate(graphics::BlendParam _srcColor, graphics::BlendParam _dstColor, graphics::BlendParam _srcAlpha, graphics::BlendParam _dstAlpha)
	{
		const BlendFactor srcColor = _toBlendFactor(_srcColor);
		const BlendFactor dstColor = _toBlendFactor(_dstColor);
		const BlendFactor srcAlpha = _toBlendFactor(_srcAlpha);
		const BlendFactor dstAlpha = _toBlendFactor(_dstAlpha);
		if (m_state.blend.srcColor == srcColor
			&& m_state.blend.dstColor == dstColor
			&& m_state.blend.srcAlpha == srcAlpha
			&& m_state.blend.dstAlpha == dstAlpha) {
			return;
		}
		m_state.blend.srcColor = srcColor;
		m_state.blend.dstColor = dstColor;
		m_state.blend.srcAlpha = srcAlpha;
		m_state.blend.dstAlpha = dstAlpha;
		m_dirtyMask |= draw_dirty::kBlend;
	}

	void setDepthCompare(graphics::CompareParam _mode)
	{
		const CompareMode compare = _toCompareMode(_mode);
		if (m_state.depth.compare == compare)
			return;
		m_state.depth.compare = compare;
		m_dirtyMask |= draw_dirty::kDepth;
	}

	void enableDepthWrite(bool _enable)
	{
		if (m_state.depth.writeEnabled == _enable)
			return;
		m_state.depth.writeEnabled = _enable;
		m_dirtyMask |= draw_dirty::kDepth;
	}

	void setBlendColor(f32 _red, f32 _green, f32 _blue, f32 _alpha)
	{
		const std::array<f32, 4> clamped = {
			std::max(0.0f, std::min(1.0f, _red)),
			std::max(0.0f, std::min(1.0f, _green)),
			std::max(0.0f, std::min(1.0f, _blue)),
			std::max(0.0f, std::min(1.0f, _alpha))
		};
		const auto approxEq = [](f32 _a, f32 _b) {
			return std::fabs(_a - _b) <= 1e-6f;
		};
		if (approxEq(m_state.blend.blendColor[0], clamped[0])
			&& approxEq(m_state.blend.blendColor[1], clamped[1])
			&& approxEq(m_state.blend.blendColor[2], clamped[2])
			&& approxEq(m_state.blend.blendColor[3], clamped[3])) {
			return;
		}
		m_state.blend.blendColor = clamped;
		m_dirtyMask |= draw_dirty::kBlend;
	}

	void setPolygonOffset(f32 _factor, f32 _units)
	{
		const auto approxEq = [](f32 _a, f32 _b) {
			return std::fabs(_a - _b) <= 1e-6f;
		};
		if (approxEq(m_state.depth.polygonOffsetFactor, _factor)
			&& approxEq(m_state.depth.polygonOffsetUnits, _units)) {
			return;
		}
		m_state.depth.polygonOffsetFactor = _factor;
		m_state.depth.polygonOffsetUnits = _units;
		m_dirtyMask |= draw_dirty::kDepth;
	}

	void cullFace(graphics::CullModeParam _mode)
	{
		const CullMode cull = _toCullMode(_mode);
		if (m_state.cullMode == cull)
			return;
		m_state.cullMode = cull;
		m_dirtyMask |= draw_dirty::kCull;
	}

	void enable(graphics::EnableParam _parameter, bool _enable)
	{
		if (_parameter == graphics::enable::BLEND) {
			if (m_state.blend.enabled != _enable) {
				m_state.blend.enabled = _enable;
				m_dirtyMask |= draw_dirty::kBlend;
			}
			return;
		}
		if (_parameter == graphics::enable::DEPTH_TEST) {
			if (m_state.depth.testEnabled != _enable) {
				m_state.depth.testEnabled = _enable;
				m_dirtyMask |= draw_dirty::kDepth;
			}
			return;
		}
		if (_parameter == graphics::enable::SCISSOR_TEST) {
			if (m_state.raster.scissorEnabled != _enable) {
				m_state.raster.scissorEnabled = _enable;
				m_dirtyMask |= draw_dirty::kScissor;
			}
			return;
		}
		if (_parameter == graphics::enable::POLYGON_OFFSET_FILL) {
			if (m_state.depth.polygonOffsetEnabled != _enable) {
				m_state.depth.polygonOffsetEnabled = _enable;
				m_dirtyMask |= draw_dirty::kDepth;
			}
		}
	}

	u32 isEnabled(graphics::EnableParam _parameter) const
	{
		if (_parameter == graphics::enable::BLEND)
			return m_state.blend.enabled ? 1U : 0U;
		if (_parameter == graphics::enable::DEPTH_TEST)
			return m_state.depth.testEnabled ? 1U : 0U;
		if (_parameter == graphics::enable::SCISSOR_TEST)
			return m_state.raster.scissorEnabled ? 1U : 0U;
		if (_parameter == graphics::enable::POLYGON_OFFSET_FILL)
			return m_state.depth.polygonOffsetEnabled ? 1U : 0U;
		return 0U;
	}

	void setClearColor(f32 _r, f32 _g, f32 _b, f32 _a)
	{
		m_clearColor[0] = _r;
		m_clearColor[1] = _g;
		m_clearColor[2] = _b;
		m_clearColor[3] = _a;
	}

	void setClearDepth(f32 _depth)
	{
		m_clearDepth = std::max(0.0f, std::min(1.0f, _depth));
	}

	const std::array<f32, 4> & clearColor() const
	{
		return m_clearColor;
	}

	f32 clearDepth() const
	{
		return m_clearDepth;
	}

	void bindFramebuffer(graphics::BufferTargetParam _target, graphics::ObjectHandle _name)
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

		bool isDefaultDrawFramebufferBound() const
		{
			return m_drawFramebufferBinding == graphics::ObjectHandle::defaultFramebuffer;
		}

		graphics::ObjectHandle drawFramebufferBinding() const
		{
			return m_drawFramebufferBinding;
		}

		graphics::ObjectHandle readFramebufferBinding() const
		{
			return m_readFramebufferBinding;
		}

		void pushPacket(DrawPacket && _packet, u32 _extraDirtyMask = draw_dirty::kNone)
		{
			if (_packet.vertices.empty())
				return;
			applyStateToPacket(_packet, _extraDirtyMask);
			m_packets.push_back(std::move(_packet));
		}

		void applyStateToPacket(DrawPacket & _packet, u32 _extraDirtyMask = draw_dirty::kNone)
		{
			_packet.state = m_state;
			_packet.dirtyMask = m_dirtyMask | _extraDirtyMask;
			m_dirtyMask = draw_dirty::kNone;
		}

	const std::vector<DrawPacket> & packets() const
	{
		return m_packets;
	}

	void markAllStateDirty()
	{
		m_dirtyMask = draw_dirty::kAll;
	}

private:
	static BlendFactor _toBlendFactor(graphics::BlendParam _factor)
	{
		if (_factor == graphics::blend::ZERO)
			return BlendFactor::Zero;
		if (_factor == graphics::blend::ONE)
			return BlendFactor::One;
		if (_factor == graphics::blend::SRC_ALPHA)
			return BlendFactor::SrcAlpha;
		if (_factor == graphics::blend::ONE_MINUS_SRC_ALPHA)
			return BlendFactor::OneMinusSrcAlpha;
		if (_factor == graphics::blend::DST_ALPHA)
			return BlendFactor::DstAlpha;
		if (_factor == graphics::blend::CONSTANT_ALPHA)
			return BlendFactor::ConstantAlpha;
		if (_factor == graphics::blend::ONE_MINUS_CONSTANT_ALPHA)
			return BlendFactor::OneMinusConstantAlpha;
		if (_factor == graphics::blend::SRC1_COLOR)
			return BlendFactor::Src1Color;
		if (_factor == graphics::blend::ONE_MINUS_SRC1_COLOR)
			return BlendFactor::OneMinusSrc1Color;
		if (_factor == graphics::blend::SRC1_ALPHA)
			return BlendFactor::Src1Alpha;
		if (_factor == graphics::blend::ONE_MINUS_SRC1_ALPHA)
			return BlendFactor::OneMinusSrc1Alpha;
		return BlendFactor::One;
	}

	static CompareMode _toCompareMode(graphics::CompareParam _mode)
	{
		if (_mode == graphics::compare::LESS)
			return CompareMode::kLess;
		if (_mode == graphics::compare::ALWAYS)
			return CompareMode::kAlways;
		return CompareMode::kLessOrEqual;
	}

	static CullMode _toCullMode(graphics::CullModeParam _mode)
	{
		if (_mode == graphics::cullMode::FRONT)
			return CullMode::kFront;
		if (_mode == graphics::cullMode::BACK)
			return CullMode::kBack;
		if (_mode == graphics::cullMode::FRONT_AND_BACK)
			return CullMode::kFrontAndBack;
		return CullMode::kNone;
	}

	DrawStateCache m_state;
	u32 m_dirtyMask = draw_dirty::kAll;
	std::array<f32, 4> m_clearColor = { 0.0f, 0.0f, 0.0f, 1.0f };
	f32 m_clearDepth = 1.0f;
	graphics::ObjectHandle m_drawFramebufferBinding = graphics::ObjectHandle::defaultFramebuffer;
	graphics::ObjectHandle m_readFramebufferBinding = graphics::ObjectHandle::defaultFramebuffer;
	std::vector<DrawPacket> m_packets;
};

} // namespace vulkan
