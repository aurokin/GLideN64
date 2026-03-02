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
	void ensurePresenterTexture(u32 _width, u32 _height);
	void renderPresentedFrame(const ExecutorOutput & _output);
	void uploadPresenterTexture(const ExecutorPresentFrame & _frame);
	static void convertToRgbaBytes(const std::vector<u32> & _srcPixels, std::vector<u8> & _dstBytes);

	graphics::Context::PresentationWindowInfo m_windowInfo{};
	graphics::ObjectHandle m_presentTexture = graphics::ObjectHandle::null;
	u32 m_presentTextureWidth = 0U;
	u32 m_presentTextureHeight = 0U;
	std::vector<u8> m_presentUploadBytes;
	Executor m_executor;
};

} // namespace rvk2
