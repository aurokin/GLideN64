#pragma once

#include "rvk2_Types.h"

namespace rvk2 {

struct ValidationResult {
	bool ok = true;
	u32 warningCount = 0U;
	u32 errorCount = 0U;
};

class Validation
{
public:
	ValidationResult validateFrame(const FrameTraceRecord & _trace, const RDPStateSnapshot & _snapshot) const;
};

} // namespace rvk2
