#include "ContextFactory.h"

#include "RealityVK2/rvk2_ContextImpl.h"
#include <Log.h>

namespace graphics {

	std::unique_ptr<ContextImpl> createContextImpl()
	{
		LOG(LOG_WARNING, "RealityVK2 runtime path: rvk2::ContextImpl (single runtime path).");
		return std::unique_ptr<ContextImpl>(new rvk2::ContextImpl);
	}

}
