#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "rvk2_SubmissionPlan.h"
#include "rvk2_TextureReplacement.h"

namespace rvk2 {

constexpr u32 kExecutorDebugSurfaceSlots = 4U;
constexpr u32 kExecutorStageDeltaClassBuckets = 32U;

enum ExecutorPresentSelectionReason : u8
{
	kExecutorPresentSelectionNone = 0U,
	kExecutorPresentSelectionLastSurface = 1U,
	kExecutorPresentSelectionVIOriginExact = 2U,
	kExecutorPresentSelectionVIOriginRange = 3U,
	kExecutorPresentSelectionMostWrittenFallback = 4U,
	kExecutorPresentSelectionNoSurface = 5U,
	kExecutorPresentSelectionPreviousSurface = 6U,
};

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
	bool textureReplacementLogSummary = false;
	std::string textureReplacementSummaryPath{};
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
	u8 viRegistersValid = 0U;
	u8 viOriginMatchedSurface = 0U;
	u8 presentSelectionReason = kExecutorPresentSelectionNone;
	u8 viRejectReason = 0U;
	u32 viOriginAddress = 0U;
	u32 selectedPresentSurfaceAddress = 0U;
	u32 selectedPresentSurfaceWidth = 0U;
	u32 selectedPresentSurfaceHeight = 0U;
	u64 selectedPresentSurfaceWriteCount = 0ULL;
	u64 selectedPresentSurfaceWorkCount = 0ULL;
	u8 selectedPresentSurfaceSize = 0U;
	u8 reservedA = 0U;
	u16 reservedB = 0U;
	u32 viResolvedSourceWidth = 0U;
	u32 viResolvedSourceHeight = 0U;
	u32 viResolvedOutputWidth = 0U;
	u32 viResolvedOutputHeight = 0U;
	u32 viResolvedLineStride = 0U;
	u64 viSourceSampleCount = 0ULL;
	u64 viSourceInvalidSampleCount = 0ULL;
	u64 viSourceLumaSum = 0ULL;
	u64 viOutputLumaSum = 0ULL;
	u64 viOutputNonBlackCount = 0ULL;
	u8 viResolvedType = 0U;
	u8 viResolvedUsesRegisters = 0U;
	u8 debugSurfaceSlotCount = 0U;
	u8 reserved0 = 0U;
	std::array<u32, kExecutorDebugSurfaceSlots> debugSurfaceAddress{};
	std::array<u64, kExecutorDebugSurfaceSlots> debugSurfaceWriteCount{};
	std::array<u64, kExecutorDebugSurfaceSlots> debugSurfaceWorkCount{};
	bool textureReplacementEnabled = false;
	u64 textureReplacementEntryCount = 0ULL;
	u64 textureReplacementPixelCount = 0ULL;
	u64 textureReplacementSampleCount = 0ULL;
	u64 textureReplacementHitCount = 0ULL;
	u64 textureReplacementMissCount = 0ULL;
	u64 textureSampleCount = 0ULL;
	u64 textureTmemSampleCount = 0ULL;
	u64 textureTmemAttemptCount = 0ULL;
	u64 textureTmemRejectFormatCount = 0ULL;
	u64 textureTmemRejectSizeCount = 0ULL;
	u64 textureTmemRejectCoordCount = 0ULL;
	u64 textureTmem32CompareCount = 0ULL;
	u64 textureTmem32CompareMismatchCount = 0ULL;
	u64 textureTmem32AltNoXorMismatchCount = 0ULL;
	u64 textureTmem32AltAbsCoordMismatchCount = 0ULL;
	u64 textureTmem32AltDirectMismatchCount = 0ULL;
	u64 textureTmem32AltDirectSwappedMismatchCount = 0ULL;
	u64 textureTmem32AltTileLineXorMismatchCount = 0ULL;
	u64 textureTmem32AltTileLineEvenOddMismatchCount = 0ULL;
	u64 textureTmem32AltDirectEvenOddMismatchCount = 0ULL;
	u64 textureTmem32AltLoadKindAwareMismatchCount = 0ULL;
	u64 textureRdramSampleCount = 0ULL;
	u64 textureSyntheticSampleCount = 0ULL;
	u64 textureLUTSampleCount = 0ULL;
	u64 combinerOpCount = 0ULL;
	u64 combinerCycle2SelectorOpCount = 0ULL;
	u64 blenderOpCount = 0ULL;
	u64 blenderEnabledOpCount = 0ULL;
	u64 blenderForceOpCount = 0ULL;
	u64 blenderAAOpCount = 0ULL;
	u64 blenderDivideOpCount = 0ULL;
	u64 blenderNoDivideOpCount = 0ULL;
	u64 alphaCompareTestCount = 0ULL;
	u64 alphaCompareRejectCount = 0ULL;
	u64 coverageWriteTestCount = 0ULL;
	u64 coverageWriteRejectCount = 0ULL;
	u64 blendCoverageEvalCount = 0ULL;
	u64 blendCoverageZeroCount = 0ULL;
	u64 blendCoverageOverflowCount = 0ULL;
	u64 coverageWriteEvalCount = 0ULL;
	u64 coverageWriteZeroCount = 0ULL;
	u64 coverageWriteOverflowCount = 0ULL;
	u64 depthEvalCount = 0ULL;
	u64 depthRejectCount = 0ULL;
	u64 depthUpdateCount = 0ULL;
	u64 colorDitherApplyCount = 0ULL;
	u64 alphaDitherApplyCount = 0ULL;
	u64 textureEdgeAlphaPromoteCount = 0ULL;
	u64 convertOneAlphaForceCount = 0ULL;
	std::array<u64, 4> blendAlphaASelectorCount{};
	std::array<u64, 4> blendAlphaBSelectorCount{};
	u64 stageTexelToCombinerDeltaCount = 0ULL;
	u64 stageCombinerToBlenderDeltaCount = 0ULL;
	u64 stageBlenderToFinalDeltaCount = 0ULL;
	u64 stageTexelToFinalDeltaCount = 0ULL;
	u64 stageTexturedWriteCount = 0ULL;
	u64 stageTexturedRectWriteCount = 0ULL;
	u64 stageTexturedTriangleWriteCount = 0ULL;
	u64 stageTexelSourceReplacementWriteCount = 0ULL;
	u64 stageTexelSourceTMEMWriteCount = 0ULL;
	u64 stageTexelSourceRdramWriteCount = 0ULL;
	u64 stageTexelSourceSyntheticWriteCount = 0ULL;
	u64 workKindFillCount = 0ULL;
	u64 workKindTexRectCount = 0ULL;
	u64 workKindTriangleCount = 0ULL;
	u64 workTexturedCount = 0ULL;
	u64 writeKindFillCount = 0ULL;
	u64 writeKindTexRectCount = 0ULL;
	u64 writeKindTriangleCount = 0ULL;
	std::array<u64, kExecutorStageDeltaClassBuckets> stageWriteClassCount{};
	std::array<u64, kExecutorStageDeltaClassBuckets> stageTexelToFinalDeltaClassCount{};
	std::array<u64, kExecutorStageDeltaClassBuckets> stageCombinerToBlenderDeltaClassCount{};
	std::array<u64, kExecutorStageDeltaClassBuckets> stageTexelSourceReplacementClassCount{};
	std::array<u64, kExecutorStageDeltaClassBuckets> stageTexelSourceTMEMClassCount{};
	std::array<u64, kExecutorStageDeltaClassBuckets> stageTexelSourceRdramClassCount{};
	std::array<u64, kExecutorStageDeltaClassBuckets> stageTexelSourceSyntheticClassCount{};
	u64 outputLumaSum = 0ULL;
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

struct ExecutorCachedSurface {
	bool valid = false;
	u32 address = 0U;
	u8 format = 0U;
	u8 size = 0U;
	u16 width = 0U;
	u16 height = 0U;
	u64 writeCount = 0ULL;
	u64 workCount = 0ULL;
	u64 lastTouched = 0ULL;
	std::vector<u32> pixels;
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
	ExecutorCachedSurface m_lastSelectedSurface{};
	std::unordered_map<u32, ExecutorCachedSurface> m_surfaceHistory{};
	u64 m_surfaceHistoryStamp = 0ULL;

	void ensureTextureReplacementLoaded();
};

ExecutorConfig loadExecutorConfigFromEnv();

} // namespace rvk2
