#include "rvk2_TraceConfig.h"

#include <atomic>

#include "rvk2_Env.h"

namespace rvk2 {

namespace {
std::atomic<u64> g_traceFrameId{1ULL};
} // namespace

const char * getTraceOutputPath()
{
	return rvk2::envStringOrNull("REALITYVK_RVK2_TRACE_FILE");
}

const char * getPacketTraceOutputPath()
{
	return rvk2::envStringOrNull("REALITYVK_RVK2_PACKET_TRACE_FILE");
}

bool shouldLogTraceSummary()
{
	return rvk2::envFlagEnabled("REALITYVK_RVK2_TRACE_LOG_SUMMARY", false);
}

u64 allocateTraceFrameId()
{
	return g_traceFrameId.fetch_add(1ULL, std::memory_order_relaxed);
}

} // namespace rvk2
