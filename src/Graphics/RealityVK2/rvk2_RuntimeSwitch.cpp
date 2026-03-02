#include "rvk2_RuntimeSwitch.h"

#include <atomic>
#include <cctype>
#include <cstdlib>

namespace {

bool equalsIgnoreCase(const char * _lhs, const char * _rhs)
{
	if (_lhs == nullptr || _rhs == nullptr)
		return false;
	for (;;) {
		const unsigned char lhs = static_cast<unsigned char>(*_lhs);
		const unsigned char rhs = static_cast<unsigned char>(*_rhs);
		if (std::tolower(lhs) != std::tolower(rhs))
			return false;
		if (lhs == '\0')
			return true;
		++_lhs;
		++_rhs;
	}
}

} // namespace

namespace rvk2 {

namespace {
std::atomic<u64> g_traceFrameId{1ULL};
} // namespace

RuntimePath getRequestedRuntimePath()
{
	const char * path = std::getenv("REALITYVK_RENDER_PATH");
	if (path == nullptr || path[0] == '\0')
		path = std::getenv("REALITYVK2_RENDER_PATH");
	if (path == nullptr || path[0] == '\0')
		return RuntimePath::kLegacy;

	if (equalsIgnoreCase(path, "rvk2")
		|| equalsIgnoreCase(path, "realityvk2")
		|| equalsIgnoreCase(path, "2")) {
		return RuntimePath::kRealityVK2;
	}
	return RuntimePath::kLegacy;
}

bool isRealityVK2Requested()
{
	return getRequestedRuntimePath() == RuntimePath::kRealityVK2;
}

bool shouldCaptureRDPTrace()
{
	if (isRealityVK2Requested())
		return true;
	return std::getenv("REALITYVK2_CAPTURE_RDP_TRACE") != nullptr;
}

const char * getTraceOutputPath()
{
	return std::getenv("REALITYVK2_TRACE_FILE");
}

const char * getPacketTraceOutputPath()
{
	return std::getenv("REALITYVK2_PACKET_TRACE_FILE");
}

bool shouldLogTraceSummary()
{
	return std::getenv("REALITYVK2_TRACE_LOG_SUMMARY") != nullptr;
}

u64 allocateTraceFrameId()
{
	return g_traceFrameId.fetch_add(1ULL, std::memory_order_relaxed);
}

} // namespace rvk2
