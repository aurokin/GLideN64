#pragma once

#include <vector>

#include <Graphics/VulkanContext/vulkan_ContextImpl.h>

#include "rvk2_Executor.h"

namespace rvk2 {

class ContextImpl final : public vulkan::ContextImpl
{
public:
	ContextImpl();
	~ContextImpl() override;

	void setPresentationWindowInfo(const graphics::Context::PresentationWindowInfo & _info) override;
	void init() override;
	void destroy() override;
	void enable(graphics::EnableParam _parameter, bool _enable) override;
	u32 isEnabled(graphics::EnableParam _parameter) override;
	void cullFace(graphics::CullModeParam _mode) override;
	void enableDepthWrite(bool _enable) override;
	void setDepthCompare(graphics::CompareParam _mode) override;
	void setViewport(s32 _x, s32 _y, s32 _width, s32 _height) override;
	void setScissor(s32 _x, s32 _y, s32 _width, s32 _height) override;
	void setBlending(graphics::BlendParam _sfactor, graphics::BlendParam _dfactor) override;
	void setBlendingSeparate(
		graphics::BlendParam _sfactorcolor,
		graphics::BlendParam _dfactorcolor,
		graphics::BlendParam _sfactoralpha,
		graphics::BlendParam _dfactoralpha) override;
	void setBlendColor(f32 _red, f32 _green, f32 _blue, f32 _alpha) override;
	void setPolygonOffset(f32 _factor, f32 _units) override;

	void drawTriangles(const graphics::Context::DrawTriangleParameters & _params) override;
	void drawRects(const graphics::Context::DrawRectParameters & _params) override;
	void drawLine(f32 _width, SPVertex * _vertices) override;
	bool present() override;

private:
	struct FrontendIngressCounters {
		u64 frameId = 0ULL;
		u64 stateCallCount = 0ULL;
		u64 triangleCallCount = 0ULL;
		u64 triangleVertexCount = 0ULL;
		bool triangleBoundsValid = false;
		f32 triangleMinX = 0.0f;
		f32 triangleMinY = 0.0f;
		f32 triangleMaxX = 0.0f;
		f32 triangleMaxY = 0.0f;
		u64 rectCallCount = 0ULL;
		u64 rectTexrectCallCount = 0ULL;
		u64 rectVertexCount = 0ULL;
		bool rectBoundsValid = false;
		f32 rectMinX = 0.0f;
		f32 rectMinY = 0.0f;
		f32 rectMaxX = 0.0f;
		f32 rectMaxY = 0.0f;
		u64 lineCallCount = 0ULL;
		u64 lineVertexCount = 0ULL;
		bool lineBoundsValid = false;
		f32 lineMinX = 0.0f;
		f32 lineMinY = 0.0f;
		f32 lineMaxX = 0.0f;
		f32 lineMaxY = 0.0f;
	};

	struct SameFramePresentAccumulator {
		bool active = false;
		u64 frameId = 0ULL;
		u64 passCount = 0ULL;
		u64 retainedNonBlackOverBlackCount = 0ULL;
		u64 promotedBlackToNonBlackCount = 0ULL;
		u64 replacedNonBlackCount = 0ULL;
		u64 nonBlackCount = 0ULL;
		u32 width = 0U;
		u32 height = 0U;
		std::vector<u32> pixels;
	};

	void syncIngressFrame();
	void resetIngressCounters(u64 _frameId);
	bool shadowDrawForwardingEnabled() const;
	bool shadowPresentEnabled() const;

	void ensurePresenterTexture(u32 _width, u32 _height);
	void renderPresentedFrame(const ExecutorOutput & _output);
	void uploadPresenterTexture(const ExecutorPresentFrame & _frame);
	static void convertToRgbaBytes(const std::vector<u32> & _srcPixels, std::vector<u8> & _dstBytes);

	graphics::Context::PresentationWindowInfo m_windowInfo{};
	graphics::ObjectHandle m_presentTexture = graphics::ObjectHandle::null;
	u32 m_presentTextureWidth = 0U;
	u32 m_presentTextureHeight = 0U;
	std::vector<u8> m_presentUploadBytes;
	FrontendIngressCounters m_ingressCounters{};
	SameFramePresentAccumulator m_sameFramePresent{};
	Executor m_executor;
};

} // namespace rvk2
