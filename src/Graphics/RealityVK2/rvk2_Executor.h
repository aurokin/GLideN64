#pragma once

#include <vector>

#include "rvk2_SubmissionPlan.h"

namespace rvk2 {

struct ExecutorConfig {
	u16 maxSurfaceWidth = 2048U;
	u16 maxSurfaceHeight = 2048U;
	u8 presentAspectX = 4U;
	u8 presentAspectY = 3U;
	bool viRegistersValid = false;
	u32 viStatus = 0U;
	u32 viOrigin = 0U;
	u32 viWidth = 0U;
	u32 viVCurrentLine = 0U;
	u32 viVSync = 0U;
	u32 viHStart = 0U;
	u32 viVStart = 0U;
	u32 viXScale = 0U;
	u32 viYScale = 0U;
};

struct ExecutorSummary {
	u64 executedWorkCount = 0ULL;
	u64 executedBatchCount = 0ULL;
	u64 colorWriteCount = 0ULL;
	u64 surfaceCount = 0ULL;
	u64 presentHash = 1469598103934665603ULL;
	u32 presentWidth = 0U;
	u32 presentHeight = 0U;
	u8 presentAspectX = 4U;
	u8 presentAspectY = 3U;
};

struct ExecutorPresentFrame {
	u32 width = 0U;
	u32 height = 0U;
	std::vector<u32> pixels;
};

struct ExecutorOutput {
	ExecutorSummary summary{};
	ExecutorPresentFrame presentFrame{};
};

class Executor
{
public:
	explicit Executor(const ExecutorConfig & _config = ExecutorConfig{});

	ExecutorOutput executeWithOutput(
		const std::vector<RenderWorkPacket> & _workPackets,
		const std::vector<SubmissionBatchPacket> & _batches);

	ExecutorSummary execute(
		const std::vector<RenderWorkPacket> & _workPackets,
		const std::vector<SubmissionBatchPacket> & _batches);

private:
	ExecutorConfig m_config;
};

ExecutorConfig loadExecutorConfigFromEnv();

} // namespace rvk2
