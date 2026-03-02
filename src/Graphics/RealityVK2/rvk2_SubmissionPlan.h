#pragma once

#include <vector>

#include "rvk2_RenderPlan.h"

namespace rvk2 {

enum class SubmissionSplitReason : u8 {
	kStart = 0U,
	kBarrier = 1U,
	kPhaseChange = 2U,
	kCycleTypeChange = 3U,
	kRenderTargetChange = 4U,
	kScissorChange = 5U
};

struct SubmissionBatchPacket {
	u32 batchIndex = 0U;
	u8 splitReason = static_cast<u8>(SubmissionSplitReason::kStart);
	u8 splitBarrierMask = render_barrier::kNone;
	u8 phase = static_cast<u8>(RenderPhase::kUnknown);
	u8 cycleType = 0U;
	u32 firstWorkIndex = 0U;
	u32 lastWorkIndex = 0U;
	u32 workCount = 0U;
	u8 barrierMaskUnion = render_barrier::kNone;
	u32 texturedWorkCount = 0U;
	u32 depthTestWorkCount = 0U;
	PacketId firstSourcePacketId = 0ULL;
	PacketId lastSourcePacketId = 0ULL;
	u8 colorImageFormat = 0U;
	u8 colorImageSize = 0U;
	u16 colorImageWidth = 0U;
	u32 colorImageAddress = 0U;
	u32 depthImageAddress = 0U;
	u8 scissorMode = 0U;
	u16 scissorXH = 0U;
	u16 scissorYH = 0U;
	u16 scissorXL = 0U;
	u16 scissorYL = 0U;
};

bool isSubmittableRenderWork(const RenderWorkPacket & _work);

void appendRenderWorkToSubmissionPlan(
	const RenderWorkPacket & _work,
	u32 _workIndex,
	std::vector<SubmissionBatchPacket> & _batches);

} // namespace rvk2
