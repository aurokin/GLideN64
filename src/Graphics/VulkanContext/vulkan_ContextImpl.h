#pragma once

#include <cstddef>
#include <memory>
#include <Graphics/ContextImpl.h>

namespace vulkan {

	struct DrawPacket;

	class ContextImpl : public graphics::ContextImpl
	{
	public:
		ContextImpl();
		~ContextImpl() override;

		static bool hasVulkanSupport();

		void setPresentationWindowInfo(const graphics::Context::PresentationWindowInfo & _info) override;

		void init() override;

		void destroy() override;

		void setClampMode(graphics::ClampMode _mode) override;

		graphics::ClampMode getClampMode() override;

		void enable(graphics::EnableParam _parameter, bool _enable) override;

		u32 isEnabled(graphics::EnableParam _parameter) override;

		void cullFace(graphics::CullModeParam _mode) override;

		void enableDepthWrite(bool _enable) override;

		void setDepthCompare(graphics::CompareParam _mode) override;

		void setViewport(s32 _x, s32 _y, s32 _width, s32 _height) override;

		void setScissor(s32 _x, s32 _y, s32 _width, s32 _height) override;

		void setBlending(graphics::BlendParam _sfactor, graphics::BlendParam _dfactor) override;

		void setBlendingSeparate(graphics::BlendParam _sfactorcolor, graphics::BlendParam _dfactorcolor, graphics::BlendParam _sfactoralpha, graphics::BlendParam _dfactoralpha) override;

		void setBlendColor(f32 _red, f32 _green, f32 _blue, f32 _alpha) override;

		void clearColorBuffer(f32 _red, f32 _green, f32 _blue, f32 _alpha) override;

		void clearDepthBuffer() override;

		void setPolygonOffset(f32 _factor, f32 _units) override;

		graphics::ObjectHandle createTexture(graphics::Parameter _target) override;

		void deleteTexture(graphics::ObjectHandle _name) override;

		void init2DTexture(const graphics::Context::InitTextureParams & _params) override;

		void update2DTexture(const graphics::Context::UpdateTextureDataParams & _params) override;

		void setTextureParameters(const graphics::Context::TexParameters & _parameters) override;

		void bindTexture(const graphics::Context::BindTextureParameters & _params) override;

		void setTextureUnpackAlignment(s32 _param) override;

		s32 getTextureUnpackAlignment() const override;

		s32 getMaxTextureSize() const override;

		f32 getMaxAnisotropy() const override;

		void bindImageTexture(const graphics::Context::BindImageTextureParameters & _params) override;

		u32 convertInternalTextureFormat(u32 _format) const override;

		void textureBarrier() override;

		graphics::FramebufferTextureFormats * getFramebufferTextureFormats() override;

		graphics::ObjectHandle createFramebuffer() override;

		void deleteFramebuffer(graphics::ObjectHandle _name) override;

		void bindFramebuffer(graphics::BufferTargetParam _target, graphics::ObjectHandle _name) override;

		void addFrameBufferRenderTarget(const graphics::Context::FrameBufferRenderTarget & _params) override;

		graphics::ObjectHandle createRenderbuffer() override;

		void initRenderbuffer(const graphics::Context::InitRenderbufferParams & _params) override;

		bool blitFramebuffers(const graphics::Context::BlitFramebuffersParams & _params) override;

		void setDrawBuffers(u32 _num) override;

		bool readScreen2(void * _dest, int _width, int _height, int _front) override;

		graphics::PixelReadBuffer * createPixelReadBuffer(size_t _sizeInBytes) override;

		graphics::ColorBufferReader * createColorBufferReader(CachedTexture * _pTexture) override;

		graphics::CombinerProgram * createCombinerProgram(Combiner & _color, Combiner & _alpha, const CombinerKey & _key) override;

		bool saveShadersStorage(const graphics::Combiners & _combiners) override;

		bool loadShadersStorage(graphics::Combiners & _combiners) override;

		graphics::ShaderProgram * createDepthFogShader() override;

		graphics::TexrectDrawerShaderProgram * createTexrectDrawerDrawShader() override;

		graphics::ShaderProgram * createTexrectDrawerClearShader() override;

		graphics::ShaderProgram * createTexrectUpscaleCopyShader() override;

		graphics::ShaderProgram * createTexrectColorAndDepthUpscaleCopyShader() override;

		graphics::ShaderProgram * createTexrectDownscaleCopyShader() override;

		graphics::ShaderProgram * createTexrectColorAndDepthDownscaleCopyShader() override;

		graphics::ShaderProgram * createGammaCorrectionShader() override;

		graphics::ShaderProgram * createFXAAShader() override;

		graphics::TextDrawerShaderProgram * createTextDrawerShader() override;

		void resetShaderProgram() override;

		void drawTriangles(const graphics::Context::DrawTriangleParameters & _params) override;

		void drawRects(const graphics::Context::DrawRectParameters & _params) override;

		void drawLine(f32 _width, SPVertex * _vertices) override;

		f32 getMaxLineWidth() override;

		bool present() override;

		bool isSupported(graphics::SpecialFeatures _feature) const override;

		s32 getMaxMSAALevel() override;

		bool isError() const override;

		bool isFramebufferError() const override;

	private:
		struct VulkanState;

		void initFramebufferFormats();
		bool initializeVulkanCore();
		bool createInstance();
		bool createSurface();
		bool selectPhysicalDevice();
		bool createDeviceAndQueue();
		bool createPresentSyncObjects();
		bool createDrawResources();
		bool createShaderModules();
		bool createPipelines();
		void createSwapchain();
		void destroySwapchain();
		void destroyDrawResources();
		void destroyPresentSyncObjects();
		void destroySurface();
		void shutdownVulkanCore();
			void resetFrameRenderData();
			bool isDefaultDrawFramebufferBound() const;
			bool executeOffscreenDrawPacket(const DrawPacket & _packet, graphics::ObjectHandle _drawFramebuffer);

			graphics::ObjectHandle _allocateHandle();

		graphics::ClampMode m_clampMode;
		s32 m_textureUnpackAlignment;
		s32 m_maxTextureSize;
		s32 m_maxMsaaLevel;
		f32 m_maxLineWidth;
		f32 m_maxAnisotropy;
		u32 m_nextHandle;
		bool m_coreReady;
		graphics::Context::PresentationWindowInfo m_presentationWindowInfo;
		std::unique_ptr<graphics::FramebufferTextureFormats> m_fbTexFormats;
		std::unique_ptr<VulkanState> m_vk;
	};

}
