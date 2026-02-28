#include "ContextFactory.h"

#include "OpenGLContext/opengl_ContextImpl.h"
#include "VulkanContext/vulkan_BackendStub.h"

namespace graphics {

	std::unique_ptr<ContextImpl> createContextImpl(GraphicsBackend _backend)
	{
		if (_backend == GraphicsBackend::Vulkan) {
			vulkan::logBackendUnavailable();
		}

		return std::unique_ptr<ContextImpl>(new opengl::ContextImpl);
	}

}
