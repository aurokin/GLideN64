#include "rvk2_RuntimeSwitch.h"

#include <atomic>
#include <cstdlib>

namespace rvk2 {

namespace {
std::atomic<u64> g_traceFrameId{1ULL};
} // namespace

RuntimePath getRequestedRuntimePath()
{
	return RuntimePath::kRealityVK2;
}

bool isRealityVK2Requested()
{
	return true;
}

bool shouldCaptureRDPTrace()
{
	return true;
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
