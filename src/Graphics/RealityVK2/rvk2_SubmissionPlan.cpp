#include "rvk2_SubmissionPlan.h"

namespace {

bool hasSameRenderTarget(const rvk2::SubmissionBatchPacket & _batch, const rvk2::RenderWorkPacket & _work)
{
	return _batch.colorImageFormat == _work.colorImageFormat
		&& _batch.colorImageSize == _work.colorImageSize
		&& _batch.colorImageWidth == _work.colorImageWidth
		&& _batch.colorImageAddress == _work.colorImageAddress
		&& _batch.depthImageAddress == _work.depthImageAddress;
}

bool hasSameScissor(const rvk2::SubmissionBatchPacket & _batch, const rvk2::RenderWorkPacket & _work)
{
	return _batch.scissorMode == _work.scissorMode
		&& _batch.scissorXH == _work.scissorXH
		&& _batch.scissorYH == _work.scissorYH
		&& _batch.scissorXL == _work.scissorXL
		&& _batch.scissorYL == _work.scissorYL;
}

u8 classifySplitReason(const rvk2::SubmissionBatchPacket & _batch, const rvk2::RenderWorkPacket & _work)
{
	if (_work.barrierMask != rvk2::render_barrier::kNone)
		return static_cast<u8>(rvk2::SubmissionSplitReason::kBarrier);
	if (_batch.phase != _work.phase)
		return static_cast<u8>(rvk2::SubmissionSplitReason::kPhaseChange);
	if (_batch.cycleType != _work.cycleType)
		return static_cast<u8>(rvk2::SubmissionSplitReason::kCycleTypeChange);
	if (!hasSameRenderTarget(_batch, _work))
		return static_cast<u8>(rvk2::SubmissionSplitReason::kRenderTargetChange);
	if (!hasSameScissor(_batch, _work))
		return static_cast<u8>(rvk2::SubmissionSplitReason::kScissorChange);
	return static_cast<u8>(rvk2::SubmissionSplitReason::kBarrier);
}

void initializeBatchFromWork(
	rvk2::SubmissionBatchPacket & _batch,
	u32 _batchIndex,
	u32 _workIndex,
	u8 _splitReason,
	const rvk2::RenderWorkPacket & _work)
{
	_batch = rvk2::SubmissionBatchPacket{};
	_batch.batchIndex = _batchIndex;
	_batch.splitReason = _splitReason;
	_batch.splitBarrierMask = _work.barrierMask;
	_batch.phase = _work.phase;
	_batch.cycleType = _work.cycleType;
	_batch.firstWorkIndex = _workIndex;
	_batch.lastWorkIndex = _workIndex;
	_batch.workCount = 1U;
	_batch.barrierMaskUnion = _work.barrierMask;
	_batch.texturedWorkCount = _work.textured ? 1U : 0U;
	_batch.depthTestWorkCount = _work.depthTest ? 1U : 0U;
	_batch.firstSourcePacketId = _work.sourcePacketId;
	_batch.lastSourcePacketId = _work.sourcePacketId;
	_batch.colorImageFormat = _work.colorImageFormat;
	_batch.colorImageSize = _work.colorImageSize;
	_batch.colorImageWidth = _work.colorImageWidth;
	_batch.colorImageAddress = _work.colorImageAddress;
	_batch.depthImageAddress = _work.depthImageAddress;
	_batch.scissorMode = _work.scissorMode;
	_batch.scissorXH = _work.scissorXH;
	_batch.scissorYH = _work.scissorYH;
	_batch.scissorXL = _work.scissorXL;
	_batch.scissorYL = _work.scissorYL;
}

bool canAppendWork(const rvk2::SubmissionBatchPacket & _batch, const rvk2::RenderWorkPacket & _work)
{
	if (_work.barrierMask != rvk2::render_barrier::kNone)
		return false;
	if (_batch.phase != _work.phase || _batch.cycleType != _work.cycleType)
		return false;
	return hasSameRenderTarget(_batch, _work) && hasSameScissor(_batch, _work);
}

void extendBatchWithWork(
	rvk2::SubmissionBatchPacket & _batch,
	u32 _workIndex,
	const rvk2::RenderWorkPacket & _work)
{
	_batch.lastWorkIndex = _workIndex;
	++_batch.workCount;
	_batch.barrierMaskUnion |= _work.barrierMask;
	if (_work.textured)
		++_batch.texturedWorkCount;
	if (_work.depthTest)
		++_batch.depthTestWorkCount;
	_batch.lastSourcePacketId = _work.sourcePacketId;
}

} // namespace

namespace rvk2 {

bool isSubmittableRenderWork(const RenderWorkPacket & _work)
{
	return _work.phase != static_cast<u8>(RenderPhase::kUnknown);
}

void appendRenderWorkToSubmissionPlan(
	const RenderWorkPacket & _work,
	u32 _workIndex,
	std::vector<SubmissionBatchPacket> & _batches)
{
	if (!isSubmittableRenderWork(_work))
		return;

	if (_batches.empty()) {
		SubmissionBatchPacket batch{};
		initializeBatchFromWork(
			batch,
			0U,
			_workIndex,
			static_cast<u8>(SubmissionSplitReason::kStart),
			_work);
		_batches.push_back(batch);
		return;
	}

	SubmissionBatchPacket & last = _batches.back();
	if (canAppendWork(last, _work)) {
		extendBatchWithWork(last, _workIndex, _work);
		return;
	}

	SubmissionBatchPacket batch{};
	initializeBatchFromWork(
		batch,
		static_cast<u32>(_batches.size()),
		_workIndex,
		classifySplitReason(last, _work),
		_work);
	_batches.push_back(batch);
}

} // namespace rvk2
