#include "vulkan_BackendStub.h"

#include <Log.h>

namespace vulkan {

	void logBackendUnavailable()
	{
		LOG(LOG_WARNING, "Vulkan backend selected, but implementation is not ready. Falling back to OpenGL.");
	}

}
