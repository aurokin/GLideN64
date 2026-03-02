#include "ContextFactory.h"

#include "RealityVK2/rvk2_ContextImpl.h"
#include "RealityVK2/rvk2_RuntimeSwitch.h"
#include "VulkanContext/vulkan_ContextImpl.h"
#include <Log.h>

namespace graphics {

	std::unique_ptr<ContextImpl> createContextImpl()
	{
		if (rvk2::isRealityVK2Requested()) {
			LOG(LOG_WARNING, "RealityVK2 path requested: using rvk2::ContextImpl.");
			return std::unique_ptr<ContextImpl>(new rvk2::ContextImpl);
		}
		if (!vulkan::ContextImpl::hasVulkanSupport()) {
			LOG(LOG_WARNING, "Vulkan backend selected, but Vulkan SDK headers are unavailable.");
		}
		return std::unique_ptr<ContextImpl>(new vulkan::ContextImpl);
	}

}
