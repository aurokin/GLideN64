#include "ContextFactory.h"

#include "VulkanContext/vulkan_ContextImpl.h"
#include <Log.h>

namespace graphics {

	std::unique_ptr<ContextImpl> createContextImpl()
	{
		if (!vulkan::ContextImpl::hasVulkanSupport()) {
			LOG(LOG_WARNING, "Vulkan backend selected, but Vulkan SDK headers are unavailable.");
		}
		return std::unique_ptr<ContextImpl>(new vulkan::ContextImpl);
	}

}
