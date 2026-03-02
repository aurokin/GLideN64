#pragma once

#include <Types.h>

namespace rvk2 {

enum class RuntimePath {
	kLegacy = 0,
	kRealityVK2
};

RuntimePath getRequestedRuntimePath();
bool isRealityVK2Requested();
bool shouldCaptureRDPTrace();
const char * getTraceOutputPath();
const char * getPacketTraceOutputPath();
bool shouldLogTraceSummary();
u64 allocateTraceFrameId();

} // namespace rvk2
