#pragma once

#include <memory>
#include "Context.h"

namespace graphics {

	class ContextImpl;

	std::unique_ptr<ContextImpl> createContextImpl(GraphicsBackend _backend);

}
