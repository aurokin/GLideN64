#include <stdio.h>
#include <cstdlib>
#include <cstring>
#include <Graphics/Context.h>
#include <mupenplus/RealityVK_mupenplus.h>
#include <RealityVK.h>
#include <Config.h>
#include <N64.h>
#include <gSP.h>
#include <Log.h>
#include <Revision.h>
#include <GLideNUI/GLideNUI.h>
#include <DisplayWindow.h>

#ifdef VC
#include <bcm_host.h>
#endif

class DisplayWindowMupen64plus : public DisplayWindow
{
public:
	DisplayWindowMupen64plus() {}

private:
	void _setAttributes();
	void _getDisplaySize();
	void _updatePresentationWindowInfo();

	bool _start() override;
	void _stop() override;
	void _restart() override;
	void _swapBuffers() override;
	void _saveScreenshot() override;
	void _saveBufferContent(graphics::ObjectHandle _fbo, CachedTexture *_pTexture) override;
	bool _resizeWindow() override;
	void _changeWindow() override;
	void _readScreen(void **_pDest, long *_pWidth, long *_pHeight) override {}
	void _readScreen2(void * _dest, int * _width, int * _height, int _front) override;
#ifdef M64P_GLIDENUI
	bool _supportsWithRateFunctions = true;
#endif // M64P_GLIDENUI
	graphics::ObjectHandle _getDefaultFramebuffer() override;
};

DisplayWindow & DisplayWindow::get()
{
	static DisplayWindowMupen64plus video;
	return video;
}

void DisplayWindowMupen64plus::_setAttributes()
{
	LOG(LOG_VERBOSE, "_setAttributes");
	// Vulkan-first branch: legacy context attributes are not used.
}

bool DisplayWindowMupen64plus::_start()
{
	if (CoreVideo_InitWithRenderMode == nullptr) {
		LOG(LOG_ERROR, "VidExt_InitWithRenderMode is required for Vulkan-only mode.");
		return false;
	}

	m64p_error returnValue = CoreVideo_InitWithRenderMode(M64P_RENDER_VULKAN);
	if (returnValue != M64ERR_SUCCESS) {
		LOG(LOG_ERROR, "VidExt_InitWithRenderMode(M64P_RENDER_VULKAN) failed. Error code: %d", returnValue);
		CoreVideo_Quit();
		return false;
	}

	_setAttributes();

	m_bFullscreen = config.video.fullscreen > 0;
	m_screenWidth = config.video.windowedWidth;
	m_screenHeight = config.video.windowedHeight;
	m_screenRefresh = config.video.fullscreenRefresh;

	_getDisplaySize();
	_setBufferSize();

	LOG(LOG_VERBOSE, "Setting video mode %dx%d", m_screenWidth, m_screenHeight);
	const m64p_video_flags flags = M64VIDEOFLAG_SUPPORT_RESIZING;
#ifdef M64P_GLIDENUI
	returnValue = CoreVideo_SetVideoModeWithRate(m_screenWidth, m_screenHeight, m_screenRefresh, 0, m_bFullscreen ? M64VIDEO_FULLSCREEN : M64VIDEO_WINDOWED, flags);
	if (returnValue != M64ERR_SUCCESS)
	{
		_supportsWithRateFunctions = false;
#endif // M64P_GLIDENUI
		returnValue = CoreVideo_SetVideoMode(m_screenWidth, m_screenHeight, 0, m_bFullscreen ? M64VIDEO_FULLSCREEN : M64VIDEO_WINDOWED, flags);
#ifdef M64P_GLIDENUI
	}
#endif // M64P_GLIDENUI
	if (returnValue != M64ERR_SUCCESS) {
		LOG(LOG_ERROR, "Error setting videomode %dx%d @ %d. Error code: %d", m_screenWidth, m_screenHeight, m_screenRefresh, returnValue);
		CoreVideo_Quit();
		return false;
	}

	_updatePresentationWindowInfo();

	char caption[128];
#ifdef PLUGIN_REVISION
# ifdef _DEBUG
	sprintf(caption, "%s debug. Revision %s", pluginName, PLUGIN_REVISION);
# else // _DEBUG
	sprintf(caption, "%s. Revision %s", pluginName, PLUGIN_REVISION);
# endif // _DEBUG
#else // PLUGIN_REVISION
# ifdef _DEBUG
	sprintf(caption, "%s debug", pluginName);
# else // _DEBUG
	sprintf(caption, "%s", pluginName);
# endif // _DEBUG
#endif // PLUGIN_REVISION
	CoreVideo_SetCaption(caption);

	return true;
}

void DisplayWindowMupen64plus::_stop()
{
	CoreVideo_Quit();
}

void DisplayWindowMupen64plus::_restart()
{
#ifdef M64P_GLIDENUI
	m_resizeWidth = 0;
	m_resizeHeight = 0;
#endif // M64P_GLIDENUI
}

void DisplayWindowMupen64plus::_swapBuffers()
{
	// if emulator defined a render callback function, call it before buffer swap
	if (renderCallback != nullptr) {
		gfxContext.resetShaderProgram();
		if (config.frameBufferEmulation.N64DepthCompare == Config::dcDisable) {
			gfxContext.setViewport(0, getHeightOffset(), getScreenWidth(), getScreenHeight());
			gSP.changed |= CHANGED_VIEWPORT;
		}
		gDP.changed |= CHANGED_COMBINE;
		(*renderCallback)((gDP.changed&CHANGED_CPU_FB_WRITE) == 0 ? 1 : 0);
	}

	// Vulkan-only branch: never fall back to a legacy swap path.
	if (!gfxContext.present()) {
		static bool warned = false;
		if (!warned) {
			LOG(LOG_WARNING, "Vulkan present failed; legacy swap fallback is disabled in Vulkan-only branch.");
			warned = true;
		}
	}
}

void DisplayWindowMupen64plus::_saveScreenshot()
{
}

void DisplayWindowMupen64plus::_saveBufferContent(graphics::ObjectHandle /*_fbo*/, CachedTexture* /*_pTexture*/)
{
}

bool DisplayWindowMupen64plus::_resizeWindow()
{
#ifdef M64P_GLIDENUI
	if (m_resizeWidth == 0 && m_resizeHeight == 0) {
		return true;
	}
#endif // M64P_GLIDENUI

	_setAttributes();

	m_width = m_screenWidth = m_resizeWidth;
	m_height = m_screenHeight = m_resizeHeight;

	_setBufferSize();
	_updatePresentationWindowInfo();
	return true;
}

void DisplayWindowMupen64plus::_changeWindow()
{
#ifdef M64P_GLIDENUI
	if (_supportsWithRateFunctions) {
		m64p_error returnValue;
		m_bFullscreen = !m_bFullscreen;
		if (m_bFullscreen) {
			m_screenWidth = config.video.fullscreenWidth;
			m_screenHeight = config.video.fullscreenHeight;
			m_screenRefresh = config.video.fullscreenRefresh;
		} else {
			m_screenWidth = config.video.windowedWidth;
			m_screenHeight = config.video.windowedHeight;
		}

		m64p_video_flags flags = {};
		returnValue = CoreVideo_SetVideoModeWithRate(m_screenWidth, m_screenHeight, m_screenRefresh, 0, m_bFullscreen ? M64VIDEO_FULLSCREEN : M64VIDEO_WINDOWED, flags);

		if (returnValue != M64ERR_SUCCESS) {
			LOG(LOG_ERROR, "Error setting videomode %dx%d @ %d. Error code: %d", m_screenWidth, m_screenHeight, m_screenRefresh, returnValue);
			CoreVideo_Quit();
		}
	} else {
#endif // M64P_GLIDENUI
		CoreVideo_ToggleFullScreen();
#ifdef M64P_GLIDENUI
		}
#endif // M64P_GLIDENUI

	_updatePresentationWindowInfo();
}

void DisplayWindowMupen64plus::_getDisplaySize()
{
#ifdef VC
	if( m_bFullscreen ) {
		// Use VC get_display_size function to get the current screen resolution
		u32 fb_width;
		u32 fb_height;
		auto returnValue = graphics_get_display_size(0 /* LCD */, &fb_width, &fb_height);
		if (returnValue < 0)
			LOG(LOG_ERROR, "Failed to get display size. Error code: %d", returnValue);
		else {
			LOG(LOG_VERBOSE, "Display size %dx%d", fb_width, fb_height);
			m_screenWidth = fb_width;
			m_screenHeight = fb_height;
		}
	}
#endif
}

void DisplayWindowMupen64plus::_updatePresentationWindowInfo()
{
	graphics::Context::PresentationWindowInfo presentationInfo;
	presentationInfo.width = m_screenWidth;
	presentationInfo.height = m_screenHeight;

	gfxContext.setPresentationWindowInfo(presentationInfo);
}

void DisplayWindowMupen64plus::_readScreen2(void * _dest, int * _width, int * _height, int _front)
{
	static const bool debugReadScreenCall = std::getenv("REALITYVK_DEBUG_READSCREEN_CALL") != nullptr;
	if (debugReadScreenCall) {
		LOG(LOG_WARNING, "DisplayWindow::_readScreen2 called; front=%d size=%ux%u",
			_front,
			m_screenWidth,
			m_screenHeight);
	}

	if (_width == nullptr || _height == nullptr)
		return;

	*_width = m_screenWidth;
	*_height = m_screenHeight;

	if (_dest == nullptr)
		return;

	if (!gfxContext.readScreen2(_dest, m_screenWidth, m_screenHeight, _front)) {
		const size_t pixelCount = static_cast<size_t>(m_screenWidth) * static_cast<size_t>(m_screenHeight);
		std::memset(_dest, 0, pixelCount * 3U);
	}
}

graphics::ObjectHandle DisplayWindowMupen64plus::_getDefaultFramebuffer()
{
	// Vulkan uses internal framebuffer handles; there is no host GL default FBO.
	return graphics::ObjectHandle::null;
}
