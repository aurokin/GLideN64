#include "rvk2_Validation.h"

namespace rvk2 {

ValidationResult Validation::validateFrame(const FrameTraceRecord & _trace, const RDPStateSnapshot & _snapshot) const
{
	ValidationResult result{};
	if (_trace.commandCount == 0ULL)
		++result.warningCount;

	if (_snapshot.cycleType > 3U)
		++result.errorCount;

	if (_trace.commandCount > 0ULL && _snapshot.colorImageWidth == 0U)
		++result.warningCount;

	if (_trace.unknownRdpOpcodeCount > 0U)
		++result.warningCount;

	if (_trace.truncatedPayloadCount > 0U)
		++result.warningCount;

	result.ok = result.errorCount == 0U;
	return result;
}

} // namespace rvk2
