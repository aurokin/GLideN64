#include "ContextFactory.h"

#include "OpenGLContext/opengl_ContextImpl.h"
#include "VulkanContext/vulkan_ContextImpl.h"
#include <Log.h>

namespace graphics {

	std::unique_ptr<ContextImpl> createContextImpl(GraphicsBackend _backend)
	{
		if (_backend == GraphicsBackend::Vulkan) {
			if (vulkan::ContextImpl::hasVulkanSupport()) {
				return std::unique_ptr<ContextImpl>(new vulkan::ContextImpl);
			}
			LOG(LOG_WARNING, "Vulkan backend selected, but Vulkan SDK headers are unavailable. Falling back to OpenGL.");
		}

		return std::unique_ptr<ContextImpl>(new opengl::ContextImpl);
	}

}
