#pragma once

#include <Types.h>

namespace rvk2 {

const char * getTraceOutputPath();
const char * getPacketTraceOutputPath();
bool shouldLogTraceSummary();
u64 allocateTraceFrameId();

} // namespace rvk2
