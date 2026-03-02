#pragma once

#include <string>
#include <vector>

#include "rvk2_SubmissionPlan.h"
#include "rvk2_TextureReplacement.h"

namespace rvk2 {

struct ExecutorConfig {
	u16 maxSurfaceWidth = 2048U;
	u16 maxSurfaceHeight = 2048U;
	u8 presentAspectX = 4U;
	u8 presentAspectY = 3U;
	bool textureReplacementEnable = false;
	std::string textureReplacementCachePath{};
	std::string textureReplacementPackPath{};
	u32 textureReplacementMaxEntries = 0U;
	u64 textureReplacementMaxPixels = 0ULL;
	u64 textureReplacementReloadToken = 0ULL;
	u64 textureReplacementInvalidateToken = 0ULL;
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
	void updateConfig(const ExecutorConfig & _config);

	ExecutorOutput executeWithOutput(
		const std::vector<RenderWorkPacket> & _workPackets,
		const std::vector<SubmissionBatchPacket> & _batches);

	ExecutorSummary execute(
		const std::vector<RenderWorkPacket> & _workPackets,
		const std::vector<SubmissionBatchPacket> & _batches);

private:
	ExecutorConfig m_config;
	bool m_textureReplacementLoaded = false;
	TextureReplacementStore m_textureReplacementStore{};

	void ensureTextureReplacementLoaded();
};

ExecutorConfig loadExecutorConfigFromEnv();

} // namespace rvk2
