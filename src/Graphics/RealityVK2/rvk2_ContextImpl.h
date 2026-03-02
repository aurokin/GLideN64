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
