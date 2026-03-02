#include <cstdio>
#include <vector>

#include "rvk2_Executor.h"
#include "rvk2_RasterPipeline.h"
#include "rvk2_RenderPlan.h"
#include "rvk2_SubmissionPlan.h"

namespace {

int g_failures = 0;

void expectTrue(bool _condition, const char * _message)
{
	if (!_condition) {
		++g_failures;
		std::fprintf(stderr, "FAIL: %s\n", _message);
	}
}

template <typename T>
void expectEq(const T & _lhs, const T & _rhs, const char * _message)
{
	expectTrue(_lhs == _rhs, _message);
}

rvk2::SubmissionBatchPacket makeSingleBatch()
{
	rvk2::SubmissionBatchPacket batch{};
	batch.batchIndex = 0U;
	batch.splitReason = static_cast<u8>(rvk2::SubmissionSplitReason::kStart);
	batch.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	batch.cycleType = 0U;
	batch.firstWorkIndex = 0U;
	batch.lastWorkIndex = 0U;
	batch.workCount = 1U;
	return batch;
}

rvk2::SubmissionBatchPacket makeBatchForWorkCount(u32 _workCount)
{
	rvk2::SubmissionBatchPacket batch = makeSingleBatch();
	if (_workCount == 0U)
		return batch;
	batch.lastWorkIndex = _workCount - 1U;
	batch.workCount = _workCount;
	return batch;
}

rvk2::RenderWorkPacket makeTriangleWork()
{
	rvk2::RenderWorkPacket work{};
	work.sourcePacketId = 1ULL;
	work.sourceOpcode = 0x0FU;
	work.opKind = static_cast<u8>(rvk2::RasterOpKind::kTriangle);
	work.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	work.cycleType = 0U;
	work.tile = 0U;
	work.textured = false;
	work.depthTest = false;
	work.rectULX = 0U;
	work.rectULY = 0U;
	work.rectLRX = 3U;
	work.rectLRY = 3U;
	work.triangleLMajor = true;
	work.triangleYH = 0U;
	work.triangleYM = 4U;
	work.triangleYL = 8U;
	work.triangleXH = 0x00000000U;
	work.triangleXL = 0x00010000U;
	work.triangleXM = 0x00000000U;
	work.triangleDxHDY = 0x00000000U;
	work.triangleDxLDY = static_cast<s32>(0xFFFFC000U);
	work.triangleDxMDY = 0x00004000U;
	work.colorImageFormat = 0U;
	work.colorImageSize = 3U;
	work.colorImageWidth = 8U;
	work.colorImageAddress = 0x00100000U;
	work.depthImageAddress = 0x00200000U;
	work.triangleShadeEnable = true;
	work.triangleTextureEnable = false;
	work.triangleZBufferEnable = false;
	work.triangleShadeR = 0xA000;
	work.triangleShadeG = 0x4000;
	work.triangleShadeB = 0x2000;
	work.triangleShadeA = 0xFF00;
	work.combineMux = 0x123456789ABCDEF0ULL;
	work.blendParams = 0x01020304U;
	return work;
}

rvk2::RenderWorkPacket makeFillWork(
	u64 _packetId,
	u32 _colorAddress,
	u32 _fillColor)
{
	rvk2::RenderWorkPacket work{};
	work.sourcePacketId = _packetId;
	work.sourceOpcode = 0x36U;
	work.opKind = static_cast<u8>(rvk2::RasterOpKind::kFillRect);
	work.phase = static_cast<u8>(rvk2::RenderPhase::kFill);
	work.cycleType = 3U;
	work.rectULX = 0U;
	work.rectULY = 0U;
	work.rectLRX = 4U;
	work.rectLRY = 4U;
	work.colorImageFormat = 0U;
	work.colorImageSize = 3U;
	work.colorImageWidth = 8U;
	work.colorImageAddress = _colorAddress;
	work.fillColor = _fillColor;
	return work;
}

rvk2::RenderWorkPacket makePixelFillWork(
	u64 _packetId,
	u32 _colorAddress,
	u16 _colorWidth,
	u32 _x,
	u32 _y,
	u32 _fillColor)
{
	rvk2::RenderWorkPacket work = makeFillWork(_packetId, _colorAddress, _fillColor);
	work.colorImageWidth = _colorWidth;
	work.rectULX = _x;
	work.rectULY = _y;
	work.rectLRX = _x;
	work.rectLRY = _y;
	return work;
}

rvk2::ExecutorConfig makeVIExecutorConfig(
	u32 _status,
	u32 _origin,
	u32 _width,
	u32 _hEnd,
	u32 _vEnd)
{
	rvk2::ExecutorConfig config{};
	config.presentAspectX = 1U;
	config.presentAspectY = 1U;
	config.viRegistersValid = true;
	config.viStatus = _status;
	config.viOrigin = _origin;
	config.viWidth = _width;
	config.viVSync = 525U;
	config.viHStart = (0U << 16U) | (_hEnd & 0x3FFU);
	config.viVStart = (0U << 16U) | (_vEnd & 0x3FFU);
	config.viXScale = 1024U;
	config.viYScale = 1024U;
	return config;
}

rvk2::RenderWorkPacket makeTexRectWork(bool _flip)
{
	rvk2::RenderWorkPacket work{};
	work.sourcePacketId = _flip ? 31ULL : 30ULL;
	work.sourceOpcode = 0x24U;
	work.opKind = static_cast<u8>(rvk2::RasterOpKind::kTexRect);
	work.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	work.cycleType = 0U;
	work.tile = 0U;
	work.texRectFlip = _flip;
	work.textured = true;
	work.rectULX = 0U;
	work.rectULY = 0U;
	work.rectLRX = 4U;
	work.rectLRY = 2U;
	work.texS = 0x0010;
	work.texT = 0x0100;
	work.texDSDX = 0x0080;
	work.texDTDY = 0x0040;
	work.colorImageFormat = 0U;
	work.colorImageSize = 3U;
	work.colorImageWidth = 8U;
	work.colorImageAddress = 0x00400000U;
	work.textureImageAddress = 0x00500000U;
	work.tileTmem = 0x40U;
	work.tileLine = 0x10U;
	work.combineMux = 0x33221100FFEEDDCCULL;
	work.blendParams = 0x000000FFU;
	return work;
}

std::vector<rvk2::SubmissionBatchPacket> makeSingleWorkBatches(
	const std::vector<rvk2::RenderWorkPacket> & _workPackets)
{
	std::vector<rvk2::SubmissionBatchPacket> batches;
	batches.reserve(_workPackets.size());
	for (u32 workIndex = 0U; workIndex < static_cast<u32>(_workPackets.size()); ++workIndex) {
		const rvk2::RenderWorkPacket & work = _workPackets[workIndex];
		rvk2::SubmissionBatchPacket batch{};
		batch.batchIndex = workIndex;
		batch.splitReason = static_cast<u8>(
			workIndex == 0U
			? rvk2::SubmissionSplitReason::kStart
			: rvk2::SubmissionSplitReason::kPhaseChange);
		batch.phase = work.phase;
		batch.cycleType = work.cycleType;
		batch.firstWorkIndex = workIndex;
		batch.lastWorkIndex = workIndex;
		batch.workCount = 1U;
		batches.push_back(batch);
	}
	return batches;
}

bool presentFramesEqual(
	const rvk2::ExecutorOutput & _a,
	const rvk2::ExecutorOutput & _b)
{
	return _a.presentFrame.width == _b.presentFrame.width
		&& _a.presentFrame.height == _b.presentFrame.height
		&& _a.presentFrame.pixels == _b.presentFrame.pixels;
}

struct MixedStateStressScene
{
	std::vector<rvk2::RenderWorkPacket> workPackets{};
	size_t nearDepthIndex = 0U;
	size_t farDepthIndex = 0U;
	size_t cycle2RectIndex = 0U;
	size_t cycle1RectIndex = 0U;
	size_t scissoredFillIndex = 0U;
	size_t offscreenTargetIndex = 0U;
};

MixedStateStressScene buildMixedStateStressScene()
{
	constexpr u32 kTargetA = 0x00C00000U;
	constexpr u32 kTargetB = 0x00C10000U;
	constexpr u32 kDepthA = 0x00D00000U;
	constexpr u32 kDepthB = 0x00D10000U;

	MixedStateStressScene scene{};
	scene.workPackets.reserve(10U);

	rvk2::RenderWorkPacket fillBackgroundA = makeFillWork(100ULL, kTargetA, 0x202020FFU);
	fillBackgroundA.rectLRX = 7U;
	fillBackgroundA.rectLRY = 7U;
	scene.workPackets.push_back(fillBackgroundA);

	rvk2::RenderWorkPacket nearTriangle = makeTriangleWork();
	nearTriangle.sourcePacketId = 101ULL;
	nearTriangle.colorImageAddress = kTargetA;
	nearTriangle.colorImageWidth = 8U;
	nearTriangle.depthImageAddress = kDepthA;
	nearTriangle.depthTest = true;
	nearTriangle.triangleZBufferEnable = true;
	nearTriangle.triangleZ = 100;
	nearTriangle.textured = true;
	nearTriangle.triangleTextureEnable = true;
	nearTriangle.triangleShadeEnable = true;
	nearTriangle.blendParams = 0x000000FFU;
	nearTriangle.combineMux = 0x1133557799BBDDFFULL;
	nearTriangle.rectULX = 0U;
	nearTriangle.rectULY = 0U;
	nearTriangle.rectLRX = 3U;
	nearTriangle.rectLRY = 3U;
	scene.nearDepthIndex = scene.workPackets.size();
	scene.workPackets.push_back(nearTriangle);

	rvk2::RenderWorkPacket farTriangle = nearTriangle;
	farTriangle.sourcePacketId = 102ULL;
	farTriangle.triangleShadeR = 0x7000;
	farTriangle.triangleShadeG = 0x1000;
	farTriangle.triangleShadeB = 0xE000;
	farTriangle.triangleZ = 220;
	scene.farDepthIndex = scene.workPackets.size();
	scene.workPackets.push_back(farTriangle);

	rvk2::RenderWorkPacket cycle2Rect = makeTexRectWork(false);
	cycle2Rect.sourcePacketId = 103ULL;
	cycle2Rect.colorImageAddress = kTargetA;
	cycle2Rect.colorImageWidth = 8U;
	cycle2Rect.phase = static_cast<u8>(rvk2::RenderPhase::kCycle2);
	cycle2Rect.cycleType = 1U;
	cycle2Rect.rectULX = 7U;
	cycle2Rect.rectULY = 0U;
	cycle2Rect.rectLRX = 4U;
	cycle2Rect.rectLRY = 3U;
	cycle2Rect.blendParams = 0x00402080U;
	cycle2Rect.combineMux = 0x7766554433221100ULL;
	scene.cycle2RectIndex = scene.workPackets.size();
	scene.workPackets.push_back(cycle2Rect);

	rvk2::RenderWorkPacket copyRect = makeTexRectWork(false);
	copyRect.sourcePacketId = 104ULL;
	copyRect.colorImageAddress = kTargetA;
	copyRect.colorImageWidth = 8U;
	copyRect.phase = static_cast<u8>(rvk2::RenderPhase::kCopy);
	copyRect.cycleType = 2U;
	copyRect.rectULX = 0U;
	copyRect.rectULY = 4U;
	copyRect.rectLRX = 3U;
	copyRect.rectLRY = 7U;
	copyRect.blendParams = 0x00004080U;
	copyRect.combineMux = 0xFFEEDDCCBBAA9988ULL;
	scene.workPackets.push_back(copyRect);

	rvk2::RenderWorkPacket cycle1Rect = makeTexRectWork(true);
	cycle1Rect.sourcePacketId = 105ULL;
	cycle1Rect.colorImageAddress = kTargetA;
	cycle1Rect.colorImageWidth = 8U;
	cycle1Rect.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	cycle1Rect.cycleType = 0U;
	cycle1Rect.rectULX = 4U;
	cycle1Rect.rectULY = 4U;
	cycle1Rect.rectLRX = 7U;
	cycle1Rect.rectLRY = 7U;
	cycle1Rect.blendParams = 0x00004080U;
	cycle1Rect.combineMux = 0x89ABCDEF01234567ULL;
	scene.cycle1RectIndex = scene.workPackets.size();
	scene.workPackets.push_back(cycle1Rect);

	rvk2::RenderWorkPacket scissoredFill = makeFillWork(106ULL, kTargetA, 0x80FF40FFU);
	scissoredFill.rectULX = 2U;
	scissoredFill.rectULY = 1U;
	scissoredFill.rectLRX = 6U;
	scissoredFill.rectLRY = 5U;
	scissoredFill.scissorXH = 3U;
	scissoredFill.scissorYH = 2U;
	scissoredFill.scissorXL = 5U;
	scissoredFill.scissorYL = 4U;
	scene.scissoredFillIndex = scene.workPackets.size();
	scene.workPackets.push_back(scissoredFill);

	rvk2::RenderWorkPacket fillOffscreen = makeFillWork(107ULL, kTargetB, 0x304060FFU);
	fillOffscreen.rectLRX = 7U;
	fillOffscreen.rectLRY = 7U;
	scene.workPackets.push_back(fillOffscreen);

	rvk2::RenderWorkPacket offscreenTriangle = nearTriangle;
	offscreenTriangle.sourcePacketId = 108ULL;
	offscreenTriangle.colorImageAddress = kTargetB;
	offscreenTriangle.depthImageAddress = kDepthB;
	offscreenTriangle.triangleZ = 90;
	offscreenTriangle.combineMux = 0x0102030405060708ULL;
	scene.offscreenTargetIndex = scene.workPackets.size();
	scene.workPackets.push_back(offscreenTriangle);

	rvk2::RenderWorkPacket finalCycle2Rect = makeTexRectWork(true);
	finalCycle2Rect.sourcePacketId = 109ULL;
	finalCycle2Rect.colorImageAddress = kTargetA;
	finalCycle2Rect.colorImageWidth = 8U;
	finalCycle2Rect.phase = static_cast<u8>(rvk2::RenderPhase::kCycle2);
	finalCycle2Rect.cycleType = 1U;
	finalCycle2Rect.rectULX = 6U;
	finalCycle2Rect.rectULY = 6U;
	finalCycle2Rect.rectLRX = 3U;
	finalCycle2Rect.rectLRY = 4U;
	finalCycle2Rect.blendParams = 0x0000A0C0U;
	finalCycle2Rect.combineMux = 0xA1B2C3D4E5F60718ULL;
	scene.workPackets.push_back(finalCycle2Rect);

	return scene;
}

void testCyclePhaseConformance()
{
	rvk2::RasterOpPacket op{};
	op.opKind = static_cast<u8>(rvk2::RasterOpKind::kTexRect);
	op.tile = 0U;
	op.rectULX = 0U;
	op.rectULY = 0U;
	op.rectLRX = 3U;
	op.rectLRY = 3U;
	rvk2::RDPStateSnapshot rdp{};
	rvk2::TMEMSnapshot tmem{};
	rvk2::RenderPlanState planState{};

	op.cycleType = 0U;
	expectEq(
		rvk2::buildRenderWorkPacket(op, rdp, tmem, planState).phase,
		static_cast<u8>(rvk2::RenderPhase::kCycle1),
		"cycle type 0 should map to Cycle1 phase");
	op.cycleType = 1U;
	expectEq(
		rvk2::buildRenderWorkPacket(op, rdp, tmem, planState).phase,
		static_cast<u8>(rvk2::RenderPhase::kCycle2),
		"cycle type 1 should map to Cycle2 phase");
	op.cycleType = 2U;
	expectEq(
		rvk2::buildRenderWorkPacket(op, rdp, tmem, planState).phase,
		static_cast<u8>(rvk2::RenderPhase::kCopy),
		"cycle type 2 should map to Copy phase");
	op.cycleType = 3U;
	expectEq(
		rvk2::buildRenderWorkPacket(op, rdp, tmem, planState).phase,
		static_cast<u8>(rvk2::RenderPhase::kFill),
		"cycle type 3 should map to Fill phase");

	op.opKind = static_cast<u8>(rvk2::RasterOpKind::kFillRect);
	op.cycleType = 0U;
	expectEq(
		rvk2::buildRenderWorkPacket(op, rdp, tmem, planState).phase,
		static_cast<u8>(rvk2::RenderPhase::kFill),
		"fill op should force Fill phase regardless of cycle type");
}

void testBlendSensitivityConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket workA = makeTriangleWork();
	workA.triangleShadeEnable = false;
	workA.triangleTextureEnable = false;
	workA.textured = false;
	workA.blendParams = 0x00010010U;

	rvk2::RenderWorkPacket workB = workA;
	workB.blendParams = 0x00330044U;

	const std::vector<rvk2::SubmissionBatchPacket> batches{makeSingleBatch()};
	const rvk2::ExecutorOutput outA =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{workA}, batches);
	const rvk2::ExecutorOutput outB =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{workB}, batches);
	expectTrue(outA.summary.colorWriteCount > 0ULL, "blend sensitivity scene should write pixels");
	expectTrue(
		outA.summary.presentHash != outB.summary.presentHash,
		"blend params should affect synthetic triangle output hash");
}

void testBlendDestinationDependencyConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket backgroundDark = makeFillWork(40ULL, 0x00600000U, 0x101010FFU);
	rvk2::RenderWorkPacket backgroundBright = backgroundDark;
	backgroundBright.fillColor = 0xE0E0E0FFU;

	rvk2::RenderWorkPacket blendedTri = makeTriangleWork();
	blendedTri.sourcePacketId = 41ULL;
	blendedTri.colorImageAddress = backgroundDark.colorImageAddress;
	blendedTri.triangleShadeEnable = true;
	blendedTri.triangleShadeA = 0x4000;
	blendedTri.blendParams = 0x00004080U;
	blendedTri.combineMux &= ~(
		(0x7ULL << 36U)
		| (0x7ULL << 39U)
		| (0x7ULL << 42U)
		| (0x7ULL << 45U));
	blendedTri.combineMux |=
		(1ULL << 36U)
		| (1ULL << 39U)
		| (1ULL << 42U)
		| (1ULL << 45U);

	const std::vector<rvk2::SubmissionBatchPacket> twoWorkBatch{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput darkOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{backgroundDark, blendedTri},
		twoWorkBatch);
	const rvk2::ExecutorOutput brightOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{backgroundBright, blendedTri},
		twoWorkBatch);

	expectTrue(darkOut.summary.colorWriteCount > 0ULL, "blend destination scene should write pixels");
	expectTrue(
		darkOut.summary.presentHash != brightOut.summary.presentHash,
		"destination color should influence blended output hash");
}

void testCombinerMuxConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket workA = makeTriangleWork();
	workA.textured = true;
	workA.triangleTextureEnable = true;
	workA.triangleShadeEnable = true;
	workA.triangleTexS = 0x01000000;
	workA.triangleTexT = 0x00800000;
	workA.triangleTexW = 0x00100000;
	workA.triangleTexDSDX = 0x00004000;
	workA.triangleTexDTDX = 0x00002000;
	workA.triangleTexDWDX = 0x00001000;
	workA.triangleTexDSDY = 0x00001000;
	workA.triangleTexDTDY = 0x00000800;
	workA.triangleTexDWDY = 0x00000400;
	workA.combineMux = 0x0123456789ABCDEFULL;
	workA.blendParams = 0x000000FFU;

	rvk2::RenderWorkPacket workB = workA;
	workB.combineMux = 0x0FEDCBA987654321ULL;

	const std::vector<rvk2::SubmissionBatchPacket> batches{makeSingleBatch()};
	const rvk2::ExecutorOutput outA =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{workA}, batches);
	const rvk2::ExecutorOutput outB =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{workB}, batches);
	expectTrue(outA.summary.colorWriteCount > 0ULL, "combiner sensitivity scene should write pixels");
	expectTrue(
		outA.summary.presentHash != outB.summary.presentHash,
		"combine mux should affect synthetic triangle output hash");
}

void testDepthOrderingConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket nearWork = makeTriangleWork();
	nearWork.depthTest = true;
	nearWork.triangleZBufferEnable = true;
	nearWork.triangleZ = 100;
	nearWork.blendParams = 0x000000FFU;
	nearWork.combineMux =
		(1ULL << 0U) | (1ULL << 3U) | (1ULL << 6U) | (1ULL << 9U)
		| (1ULL << 12U) | (1ULL << 15U) | (1ULL << 18U) | (1ULL << 21U)
		| (1ULL << 24U) | (1ULL << 27U) | (1ULL << 30U) | (1ULL << 33U)
		| (1ULL << 36U) | (1ULL << 39U) | (1ULL << 42U) | (1ULL << 45U);

	rvk2::RenderWorkPacket farWork = nearWork;
	farWork.sourcePacketId = 2ULL;
	farWork.triangleShadeR = 0x2000;
	farWork.triangleShadeG = 0xA000;
	farWork.triangleShadeB = 0x5000;
	farWork.triangleZ = 200;

	const std::vector<rvk2::SubmissionBatchPacket> oneBatch{makeSingleBatch()};
	const rvk2::ExecutorOutput nearOnly =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{nearWork}, oneBatch);
	expectTrue(nearOnly.summary.colorWriteCount > 0ULL, "near-only depth scene should write pixels");

	rvk2::SubmissionBatchPacket twoBatch = makeSingleBatch();
	twoBatch.lastWorkIndex = 1U;
	twoBatch.workCount = 2U;
	const std::vector<rvk2::SubmissionBatchPacket> combinedBatches{twoBatch};

	const rvk2::ExecutorOutput nearThenFar = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{nearWork, farWork},
		combinedBatches);
	expectEq(
		nearThenFar.summary.colorWriteCount,
		nearOnly.summary.colorWriteCount,
		"far fragment should fail depth test after near write");
	expectEq(
		nearThenFar.summary.presentHash,
		nearOnly.summary.presentHash,
		"far fragment should not change present hash when depth fails");

	const rvk2::ExecutorOutput farThenNear = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{farWork, nearWork},
		combinedBatches);
	expectEq(
		farThenNear.summary.presentHash,
		nearOnly.summary.presentHash,
		"near fragment should overwrite farther fragment in depth buffer");
	expectTrue(
		farThenNear.summary.colorWriteCount >= nearOnly.summary.colorWriteCount * 2ULL,
		"near-over-far ordering should produce two color write passes");
}

void testDepthPhaseParticipationConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket nearWork = makeTriangleWork();
	nearWork.depthTest = true;
	nearWork.triangleZBufferEnable = true;
	nearWork.triangleZ = 100;
	nearWork.blendParams = 0x000000FFU;
	nearWork.combineMux =
		(1ULL << 0U) | (1ULL << 3U) | (1ULL << 6U) | (1ULL << 9U)
		| (1ULL << 12U) | (1ULL << 15U) | (1ULL << 18U) | (1ULL << 21U)
		| (1ULL << 24U) | (1ULL << 27U) | (1ULL << 30U) | (1ULL << 33U)
		| (1ULL << 36U) | (1ULL << 39U) | (1ULL << 42U) | (1ULL << 45U);

	rvk2::RenderWorkPacket farWork = nearWork;
	farWork.sourcePacketId = 3ULL;
	farWork.triangleShadeR = 0x2000;
	farWork.triangleShadeG = 0xA000;
	farWork.triangleShadeB = 0x5000;
	farWork.triangleZ = 200;

	const std::vector<rvk2::SubmissionBatchPacket> oneBatch{makeSingleBatch()};
	const rvk2::ExecutorOutput nearOnly =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{nearWork}, oneBatch);
	expectTrue(nearOnly.summary.colorWriteCount > 0ULL, "phase-depth near-only scene should write pixels");

	rvk2::SubmissionBatchPacket twoBatch = makeBatchForWorkCount(2U);
	const std::vector<rvk2::SubmissionBatchPacket> combinedBatches{twoBatch};

	const rvk2::ExecutorOutput cycle1NearThenFar = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{nearWork, farWork},
		combinedBatches);
	expectEq(
		cycle1NearThenFar.summary.presentHash,
		nearOnly.summary.presentHash,
		"cycle1 should reject farther fragment with depth enabled");

	rvk2::RenderWorkPacket cycle2Near = nearWork;
	cycle2Near.phase = static_cast<u8>(rvk2::RenderPhase::kCycle2);
	cycle2Near.cycleType = 1U;
	rvk2::RenderWorkPacket cycle2Far = farWork;
	cycle2Far.phase = static_cast<u8>(rvk2::RenderPhase::kCycle2);
	cycle2Far.cycleType = 1U;
	const rvk2::ExecutorOutput cycle2NearOnly =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{cycle2Near}, oneBatch);
	const rvk2::ExecutorOutput cycle2NearThenFar = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{cycle2Near, cycle2Far},
		combinedBatches);
	expectEq(
		cycle2NearThenFar.summary.presentHash,
		cycle2NearOnly.summary.presentHash,
		"cycle2 should also honor depth test");

	rvk2::RenderWorkPacket copyNear = nearWork;
	copyNear.phase = static_cast<u8>(rvk2::RenderPhase::kCopy);
	copyNear.cycleType = 2U;
	rvk2::RenderWorkPacket copyFar = farWork;
	copyFar.phase = static_cast<u8>(rvk2::RenderPhase::kCopy);
	copyFar.cycleType = 2U;
	const rvk2::ExecutorOutput copyNearOnly =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{copyNear}, oneBatch);
	const rvk2::ExecutorOutput copyNearThenFar = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{copyNear, copyFar},
		combinedBatches);
	expectTrue(
		copyNearThenFar.summary.colorWriteCount >= copyNearOnly.summary.colorWriteCount * 2ULL,
		"copy phase should bypass depth compare/update");
	expectTrue(
		copyNearThenFar.summary.presentHash != copyNearOnly.summary.presentHash,
		"copy phase far fragment should overwrite near fragment");
}

void testPrimitiveDepthSourceConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket primNear = makeTriangleWork();
	primNear.depthTest = true;
	primNear.triangleZBufferEnable = true;
	primNear.triangleZ = 300;
	primNear.depthSource = 1U;
	primNear.primDepthZ = 100U;
	primNear.primDepthDelta = 0U;
	primNear.blendParams = 0x000000FFU;
	primNear.combineMux =
		(1ULL << 0U) | (1ULL << 3U) | (1ULL << 6U) | (1ULL << 9U)
		| (1ULL << 12U) | (1ULL << 15U) | (1ULL << 18U) | (1ULL << 21U)
		| (1ULL << 24U) | (1ULL << 27U) | (1ULL << 30U) | (1ULL << 33U)
		| (1ULL << 36U) | (1ULL << 39U) | (1ULL << 42U) | (1ULL << 45U);

	rvk2::RenderWorkPacket primFar = primNear;
	primFar.sourcePacketId = 5ULL;
	primFar.triangleShadeR = 0x2000;
	primFar.triangleShadeG = 0xA000;
	primFar.triangleShadeB = 0x5000;
	primFar.triangleZ = 50;
	primFar.primDepthZ = 220U;

	const std::vector<rvk2::SubmissionBatchPacket> oneBatch{makeSingleBatch()};
	const std::vector<rvk2::SubmissionBatchPacket> twoBatch{makeBatchForWorkCount(2U)};

	rvk2::RenderWorkPacket pixelNear = primNear;
	pixelNear.depthSource = 0U;
	rvk2::RenderWorkPacket pixelFar = primFar;
	pixelFar.depthSource = 0U;
	const rvk2::ExecutorOutput pixelNearOnly = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{pixelNear},
		oneBatch);
	const rvk2::ExecutorOutput pixelNearThenFar = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{pixelNear, pixelFar},
		twoBatch);
	expectTrue(
		pixelNearThenFar.summary.presentHash != pixelNearOnly.summary.presentHash,
		"pixel depth source should allow lower triangle-Z fragment to overwrite");

	const rvk2::ExecutorOutput primNearOnly = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{primNear},
		oneBatch);
	const rvk2::ExecutorOutput primNearThenFar = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{primNear, primFar},
		twoBatch);
	expectEq(
		primNearThenFar.summary.presentHash,
		primNearOnly.summary.presentHash,
		"primitive depth source should ignore triangle-Z coefficients");
	expectEq(
		primNearThenFar.summary.colorWriteCount,
		primNearOnly.summary.colorWriteCount,
		"primitive depth source should reject farther primitive-depth fragment");
}

void testDepthCompareUpdateModeConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket nearWork = makeTriangleWork();
	nearWork.depthTest = true;
	nearWork.triangleZBufferEnable = true;
	nearWork.triangleZ = 100;
	nearWork.blendParams = 0x000000FFU;
	nearWork.combineMux =
		(1ULL << 0U) | (1ULL << 3U) | (1ULL << 6U) | (1ULL << 9U)
		| (1ULL << 12U) | (1ULL << 15U) | (1ULL << 18U) | (1ULL << 21U)
		| (1ULL << 24U) | (1ULL << 27U) | (1ULL << 30U) | (1ULL << 33U)
		| (1ULL << 36U) | (1ULL << 39U) | (1ULL << 42U) | (1ULL << 45U);
	nearWork.depthCompareEnable = true;
	nearWork.depthUpdateEnable = true;

	rvk2::RenderWorkPacket farWork = nearWork;
	farWork.sourcePacketId = 7ULL;
	farWork.triangleShadeR = 0x2000;
	farWork.triangleShadeG = 0xA000;
	farWork.triangleShadeB = 0x5000;
	farWork.triangleZ = 200;

	const std::vector<rvk2::SubmissionBatchPacket> oneBatch{makeSingleBatch()};
	const std::vector<rvk2::SubmissionBatchPacket> twoBatch{makeBatchForWorkCount(2U)};

	const rvk2::ExecutorOutput compareUpdateNearOnly =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{nearWork}, oneBatch);
	const rvk2::ExecutorOutput compareUpdateNearThenFar = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{nearWork, farWork},
		twoBatch);
	expectEq(
		compareUpdateNearThenFar.summary.presentHash,
		compareUpdateNearOnly.summary.presentHash,
		"depth compare+update should reject farther fragment after near write");

	rvk2::RenderWorkPacket noUpdateNear = nearWork;
	noUpdateNear.depthUpdateEnable = false;
	rvk2::RenderWorkPacket noUpdateFar = farWork;
	noUpdateFar.depthUpdateEnable = false;
	const rvk2::ExecutorOutput noUpdateNearThenFar = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{noUpdateNear, noUpdateFar},
		twoBatch);
	expectTrue(
		noUpdateNearThenFar.summary.presentHash != compareUpdateNearOnly.summary.presentHash,
		"depth compare without depth update should allow far fragment overwrite");
	expectTrue(
		noUpdateNearThenFar.summary.colorWriteCount > compareUpdateNearOnly.summary.colorWriteCount,
		"depth compare without update should increase color writes");

	rvk2::RenderWorkPacket noCompareNear = nearWork;
	noCompareNear.depthCompareEnable = false;
	rvk2::RenderWorkPacket noCompareFar = farWork;
	noCompareFar.depthCompareEnable = false;
	const rvk2::ExecutorOutput noCompareNearThenFar = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{noCompareNear, noCompareFar},
		twoBatch);
	expectTrue(
		noCompareNearThenFar.summary.presentHash != compareUpdateNearOnly.summary.presentHash,
		"depth update without depth compare should allow far fragment overwrite");
	expectTrue(
		noCompareNearThenFar.summary.colorWriteCount > compareUpdateNearOnly.summary.colorWriteCount,
		"depth update without compare should increase color writes");
}

void testDepthSurfaceAliasIsolationConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket nearWork = makeTriangleWork();
	nearWork.depthTest = true;
	nearWork.triangleZBufferEnable = true;
	nearWork.triangleZ = 100;
	nearWork.blendParams = 0x000000FFU;
	nearWork.combineMux =
		(1ULL << 0U) | (1ULL << 3U) | (1ULL << 6U) | (1ULL << 9U)
		| (1ULL << 12U) | (1ULL << 15U) | (1ULL << 18U) | (1ULL << 21U)
		| (1ULL << 24U) | (1ULL << 27U) | (1ULL << 30U) | (1ULL << 33U)
		| (1ULL << 36U) | (1ULL << 39U) | (1ULL << 42U) | (1ULL << 45U);

	rvk2::RenderWorkPacket farWork = nearWork;
	farWork.sourcePacketId = 4ULL;
	farWork.triangleShadeR = 0x2000;
	farWork.triangleShadeG = 0xA000;
	farWork.triangleShadeB = 0x5000;
	farWork.triangleZ = 200;

	const std::vector<rvk2::SubmissionBatchPacket> oneBatch{makeSingleBatch()};
	rvk2::SubmissionBatchPacket twoBatch = makeBatchForWorkCount(2U);
	const std::vector<rvk2::SubmissionBatchPacket> combinedBatches{twoBatch};

	rvk2::RenderWorkPacket aliasNear = nearWork;
	aliasNear.depthImageAddress = 0U;
	rvk2::RenderWorkPacket aliasFar = farWork;
	aliasFar.depthImageAddress = 0U;
	const rvk2::ExecutorOutput aliasNearOnly =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{aliasNear}, oneBatch);
	const rvk2::ExecutorOutput aliasNearThenFar = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{aliasNear, aliasFar},
		combinedBatches);
	expectEq(
		aliasNearThenFar.summary.presentHash,
		aliasNearOnly.summary.presentHash,
		"depthImageAddress=0 should alias depth surface to color target and reject far fragment");

	rvk2::RenderWorkPacket isolatedFar = farWork;
	isolatedFar.depthImageAddress = farWork.depthImageAddress ^ 0x00001000U;
	const rvk2::ExecutorOutput isolatedNearThenFar = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{nearWork, isolatedFar},
		combinedBatches);
	expectTrue(
		isolatedNearThenFar.summary.colorWriteCount >= aliasNearOnly.summary.colorWriteCount * 2ULL,
		"separate depth surfaces should not cross-occlude");
	expectTrue(
		isolatedNearThenFar.summary.presentHash != aliasNearOnly.summary.presentHash,
		"depth-surface isolation should allow far fragment overwrite on color target");
}

void testCoverageBlendFlagConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket background = makeFillWork(80ULL, 0x00B00000U, 0x606060FFU);

	rvk2::RenderWorkPacket triNoCoverage = makeTriangleWork();
	triNoCoverage.sourcePacketId = 81ULL;
	triNoCoverage.colorImageAddress = background.colorImageAddress;
	triNoCoverage.triangleShadeEnable = true;
	triNoCoverage.blendParams = 0x00004080U;

	rvk2::RenderWorkPacket triWithCoverage = triNoCoverage;
	triWithCoverage.blendParams |= 0x80000000U;

	const std::vector<rvk2::SubmissionBatchPacket> twoWorkBatch{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput noCoverageOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, triNoCoverage},
		twoWorkBatch);
	const rvk2::ExecutorOutput withCoverageOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, triWithCoverage},
		twoWorkBatch);

	expectEq(
		noCoverageOut.summary.colorWriteCount,
		withCoverageOut.summary.colorWriteCount,
		"coverage blend flag should not change covered pixel count");
	expectTrue(
		noCoverageOut.summary.presentHash != withCoverageOut.summary.presentHash,
		"coverage blend flag should alter blended output hash");
}

void testCoverageModeFlagConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket background = makeFillWork(110ULL, 0x00B30000U, 0x40404000U);
	rvk2::RenderWorkPacket baselineTri = makeTriangleWork();
	baselineTri.sourcePacketId = 111ULL;
	baselineTri.colorImageAddress = background.colorImageAddress;
	baselineTri.triangleShadeEnable = true;
	baselineTri.blendParams = 0x00004080U;

	const std::vector<rvk2::SubmissionBatchPacket> oneWorkBatch{makeBatchForWorkCount(1U)};
	const std::vector<rvk2::SubmissionBatchPacket> twoWorkBatch{makeBatchForWorkCount(2U)};

	const rvk2::ExecutorOutput backgroundOnly =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{background}, oneWorkBatch);
	const rvk2::ExecutorOutput baselineOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, baselineTri},
		twoWorkBatch);
	expectTrue(
		baselineOut.summary.colorWriteCount > backgroundOnly.summary.colorWriteCount,
		"baseline coverage mode scene should add triangle writes");

	rvk2::RenderWorkPacket colorOnCvgSave = baselineTri;
	colorOnCvgSave.colorOnCvg = true;
	colorOnCvgSave.cvgDest = 3U;
	const rvk2::ExecutorOutput gatedOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, colorOnCvgSave},
		twoWorkBatch);
	expectEq(
		gatedOut.summary.presentHash,
		backgroundOnly.summary.presentHash,
		"colorOnCvg + cvgDest=save should suppress writes when destination coverage is empty");
	expectEq(
		gatedOut.summary.colorWriteCount,
		backgroundOnly.summary.colorWriteCount,
		"coverage-gated write suppression should preserve background-only write count");

	rvk2::RenderWorkPacket coverageModeVariant = baselineTri;
	coverageModeVariant.cvgDest = 1U;
	coverageModeVariant.blendMask = 0xBU;
	coverageModeVariant.cvgXAlpha = true;
	coverageModeVariant.alphaCvgSel = true;
	coverageModeVariant.forceBlender = true;
	const rvk2::ExecutorOutput coverageModeOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, coverageModeVariant},
		twoWorkBatch);
	expectTrue(
		coverageModeOut.summary.presentHash != baselineOut.summary.presentHash,
		"coverage mode flags should alter blended output hash");
}

void testAlphaCompareConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket background = makeFillWork(90ULL, 0x00B20000U, 0x202020FFU);
	rvk2::RenderWorkPacket alphaFill = makeFillWork(91ULL, background.colorImageAddress, 0xC0808040U);
	alphaFill.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	alphaFill.cycleType = 0U;
	alphaFill.alphaCompare = 1U;
	alphaFill.blendColor = 0x00000080U;

	const std::vector<rvk2::SubmissionBatchPacket> oneWorkBatch{makeBatchForWorkCount(1U)};
	const std::vector<rvk2::SubmissionBatchPacket> twoWorkBatch{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput backgroundOnly =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{background}, oneWorkBatch);
	const rvk2::ExecutorOutput strictOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, alphaFill},
		twoWorkBatch);
	expectEq(
		strictOut.summary.presentHash,
		backgroundOnly.summary.presentHash,
		"alpha compare should reject fill when alpha is below blend threshold");
	expectEq(
		strictOut.summary.colorWriteCount,
		backgroundOnly.summary.colorWriteCount,
		"alpha compare reject should preserve color write count");

	rvk2::RenderWorkPacket relaxedFill = alphaFill;
	relaxedFill.blendColor = 0x00000020U;
	const rvk2::ExecutorOutput relaxedOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, relaxedFill},
		twoWorkBatch);
	expectTrue(
		relaxedOut.summary.presentHash != backgroundOnly.summary.presentHash,
		"alpha compare should accept fill when alpha passes blend threshold");
	expectTrue(
		relaxedOut.summary.colorWriteCount > backgroundOnly.summary.colorWriteCount,
		"alpha compare pass should increase color writes");
}

void testMixedStateBatchSegmentationConformance()
{
	rvk2::Executor executor;
	const MixedStateStressScene scene = buildMixedStateStressScene();
	const std::vector<rvk2::SubmissionBatchPacket> singleBatch{
		makeBatchForWorkCount(static_cast<u32>(scene.workPackets.size()))
	};
	const std::vector<rvk2::SubmissionBatchPacket> splitBatches =
		makeSingleWorkBatches(scene.workPackets);

	const rvk2::ExecutorOutput baseOut =
		executor.executeWithOutput(scene.workPackets, singleBatch);
	const rvk2::ExecutorOutput repeatOut =
		executor.executeWithOutput(scene.workPackets, singleBatch);
	const rvk2::ExecutorOutput splitOut =
		executor.executeWithOutput(scene.workPackets, splitBatches);

	expectTrue(
		baseOut.summary.colorWriteCount > 0ULL,
		"mixed-state scene should produce color writes");
	expectEq(
		baseOut.summary.presentHash,
		repeatOut.summary.presentHash,
		"mixed-state single-batch execution should be deterministic");
	expectEq(
		baseOut.summary.colorWriteCount,
		repeatOut.summary.colorWriteCount,
		"mixed-state single-batch color write count should be deterministic");
	expectTrue(
		presentFramesEqual(baseOut, repeatOut),
		"mixed-state single-batch present frame should be deterministic");
	expectEq(
		baseOut.summary.presentHash,
		splitOut.summary.presentHash,
		"mixed-state split batches should preserve present hash");
	expectEq(
		baseOut.summary.colorWriteCount,
		splitOut.summary.colorWriteCount,
		"mixed-state split batches should preserve color write count");
	expectTrue(
		presentFramesEqual(baseOut, splitOut),
		"mixed-state split batches should preserve present frame");
}

void testMixedStateRapidTransitionMatrixConformance()
{
	rvk2::Executor executor;
	const MixedStateStressScene scene = buildMixedStateStressScene();
	const std::vector<rvk2::SubmissionBatchPacket> batches{
		makeBatchForWorkCount(static_cast<u32>(scene.workPackets.size()))
	};
	const rvk2::ExecutorOutput baseOut =
		executor.executeWithOutput(scene.workPackets, batches);

	expectTrue(
		baseOut.summary.colorWriteCount > 0ULL,
		"mixed-state matrix base scene should produce color writes");

	std::vector<rvk2::RenderWorkPacket> isolatedDepth = scene.workPackets;
	isolatedDepth[scene.farDepthIndex].depthImageAddress ^= 0x00002000U;
	const rvk2::ExecutorOutput isolatedDepthOut =
		executor.executeWithOutput(isolatedDepth, batches);
	expectTrue(
		isolatedDepthOut.summary.colorWriteCount > baseOut.summary.colorWriteCount,
		"isolated depth surfaces should increase writes from far fragment participation");
	expectTrue(
		isolatedDepthOut.summary.presentHash != baseOut.summary.presentHash,
		"isolated depth surfaces should alter mixed-state output hash");

	std::vector<rvk2::RenderWorkPacket> copyDepthBypass = scene.workPackets;
	copyDepthBypass[scene.farDepthIndex].phase = static_cast<u8>(rvk2::RenderPhase::kCopy);
	copyDepthBypass[scene.farDepthIndex].cycleType = 2U;
	const rvk2::ExecutorOutput copyDepthBypassOut =
		executor.executeWithOutput(copyDepthBypass, batches);
	expectTrue(
		copyDepthBypassOut.summary.colorWriteCount > baseOut.summary.colorWriteCount,
		"copy-phase transition should bypass depth and increase write count");
	expectTrue(
		copyDepthBypassOut.summary.presentHash != baseOut.summary.presentHash,
		"copy-phase transition should alter mixed-state output hash");

	std::vector<rvk2::RenderWorkPacket> coverageVariant = scene.workPackets;
	coverageVariant[scene.cycle1RectIndex].blendParams |= 0x80000000U;
	const rvk2::ExecutorOutput coverageOut =
		executor.executeWithOutput(coverageVariant, batches);
	expectEq(
		coverageOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"coverage modulation should preserve write count in mixed-state matrix");
	expectTrue(
		coverageOut.summary.presentHash != baseOut.summary.presentHash,
		"coverage modulation should alter mixed-state output hash");

	std::vector<rvk2::RenderWorkPacket> combinerVariant = scene.workPackets;
	combinerVariant[scene.cycle2RectIndex].combineMux ^= 0x00FF00FF00FF00FFULL;
	const rvk2::ExecutorOutput combinerOut =
		executor.executeWithOutput(combinerVariant, batches);
	expectTrue(
		combinerOut.summary.presentHash != baseOut.summary.presentHash,
		"cycle2 combiner transition should alter mixed-state output hash");

	std::vector<rvk2::RenderWorkPacket> scissorVariant = scene.workPackets;
	scissorVariant[scene.scissoredFillIndex].scissorXH = 4U;
	scissorVariant[scene.scissoredFillIndex].scissorXL = 4U;
	scissorVariant[scene.scissoredFillIndex].scissorYH = 3U;
	scissorVariant[scene.scissoredFillIndex].scissorYL = 3U;
	const rvk2::ExecutorOutput scissorOut =
		executor.executeWithOutput(scissorVariant, batches);
	expectTrue(
		scissorOut.summary.colorWriteCount < baseOut.summary.colorWriteCount,
		"tighter scissor should reduce mixed-state write count");
	expectTrue(
		scissorOut.summary.presentHash != baseOut.summary.presentHash,
		"tighter scissor should alter mixed-state output hash");

	std::vector<rvk2::RenderWorkPacket> offscreenOnlyVariant = scene.workPackets;
	offscreenOnlyVariant[scene.offscreenTargetIndex].combineMux ^= 0xFFFFFFFF00000000ULL;
	offscreenOnlyVariant[scene.offscreenTargetIndex].blendParams ^= 0x00FF00FFU;
	const rvk2::ExecutorOutput offscreenOnlyOut =
		executor.executeWithOutput(offscreenOnlyVariant, batches);
	expectEq(
		offscreenOnlyOut.summary.presentHash,
		baseOut.summary.presentHash,
		"offscreen-only transitions should not affect present hash");
	expectTrue(
		presentFramesEqual(offscreenOnlyOut, baseOut),
		"offscreen-only transitions should not affect present frame");
}

void testCoverageScissorConformance()
{
	rvk2::Executor executor;
	rvk2::SubmissionBatchPacket batch = makeSingleBatch();
	batch.phase = static_cast<u8>(rvk2::RenderPhase::kFill);
	batch.cycleType = 3U;
	const std::vector<rvk2::SubmissionBatchPacket> batches{batch};

	rvk2::RenderWorkPacket full{};
	full.sourcePacketId = 10ULL;
	full.sourceOpcode = 0x36U;
	full.opKind = static_cast<u8>(rvk2::RasterOpKind::kFillRect);
	full.phase = static_cast<u8>(rvk2::RenderPhase::kFill);
	full.cycleType = 3U;
	full.rectULX = 0U;
	full.rectULY = 0U;
	full.rectLRX = 4U;
	full.rectLRY = 4U;
	full.colorImageFormat = 0U;
	full.colorImageSize = 3U;
	full.colorImageWidth = 8U;
	full.colorImageAddress = 0x00300000U;
	full.fillColor = 0xFFFFFFFFU;
	full.scissorXH = 0U;
	full.scissorYH = 0U;
	full.scissorXL = 0U;
	full.scissorYL = 0U;

	rvk2::RenderWorkPacket clipped = full;
	clipped.scissorXH = 1U;
	clipped.scissorXL = 2U;
	clipped.scissorYH = 1U;
	clipped.scissorYL = 3U;

	const rvk2::ExecutorOutput outFull =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{full}, batches);
	const rvk2::ExecutorOutput outClipped =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{clipped}, batches);

	expectEq(outFull.summary.colorWriteCount, 25ULL, "full fill-rect coverage write count mismatch");
	expectEq(outClipped.summary.colorWriteCount, 6ULL, "scissored fill-rect coverage write count mismatch");
	expectTrue(
		outClipped.summary.presentHash != outFull.summary.presentHash,
		"scissor coverage change should alter present hash");
}

void testCopyPhaseDestinationBypassConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket backgroundDark = makeFillWork(60ULL, 0x00900000U, 0x101010FFU);
	rvk2::RenderWorkPacket backgroundBright = backgroundDark;
	backgroundBright.fillColor = 0xE0E0E0FFU;

	rvk2::RenderWorkPacket copyRect = makeTexRectWork(false);
	copyRect.sourcePacketId = 61ULL;
	copyRect.colorImageAddress = backgroundDark.colorImageAddress;
	copyRect.phase = static_cast<u8>(rvk2::RenderPhase::kCopy);
	copyRect.cycleType = 2U;
	copyRect.rectLRY = 4U;
	copyRect.blendParams = 0x00004080U;

	rvk2::RenderWorkPacket cycle1Rect = copyRect;
	cycle1Rect.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	cycle1Rect.cycleType = 0U;

	const std::vector<rvk2::SubmissionBatchPacket> twoWorkBatch{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput copyDarkOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{backgroundDark, copyRect},
		twoWorkBatch);
	const rvk2::ExecutorOutput copyBrightOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{backgroundBright, copyRect},
		twoWorkBatch);

	expectTrue(copyDarkOut.summary.colorWriteCount > 0ULL, "copy-phase destination bypass scene should write pixels");
	expectEq(
		copyDarkOut.summary.presentHash,
		copyBrightOut.summary.presentHash,
		"copy phase should ignore destination-dependent blend/combiner effects");

	const rvk2::ExecutorOutput cycle1DarkOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{backgroundDark, cycle1Rect},
		twoWorkBatch);
	const rvk2::ExecutorOutput cycle1BrightOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{backgroundBright, cycle1Rect},
		twoWorkBatch);
	expectTrue(
		cycle1DarkOut.summary.presentHash != cycle1BrightOut.summary.presentHash,
		"cycle1 path should remain destination-sensitive for this blend setup");
}

void testCycle2PhaseDistinctConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket cycle1Work = makeTriangleWork();
	cycle1Work.textured = true;
	cycle1Work.triangleTextureEnable = true;
	cycle1Work.triangleShadeEnable = true;
	cycle1Work.triangleTexS = 0x01000000;
	cycle1Work.triangleTexT = 0x00800000;
	cycle1Work.triangleTexW = 0x00100000;
	cycle1Work.triangleTexDSDX = 0x00004000;
	cycle1Work.triangleTexDTDX = 0x00002000;
	cycle1Work.triangleTexDWDX = 0x00001000;
	cycle1Work.triangleTexDSDY = 0x00001000;
	cycle1Work.triangleTexDTDY = 0x00000800;
	cycle1Work.triangleTexDWDY = 0x00000400;
	cycle1Work.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	cycle1Work.cycleType = 0U;
	cycle1Work.blendParams = 0x000000FFU;

	rvk2::RenderWorkPacket cycle2Work = cycle1Work;
	cycle2Work.phase = static_cast<u8>(rvk2::RenderPhase::kCycle2);
	cycle2Work.cycleType = 1U;

	const std::vector<rvk2::SubmissionBatchPacket> batches{makeSingleBatch()};
	const rvk2::ExecutorOutput cycle1Out =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{cycle1Work}, batches);
	const rvk2::ExecutorOutput cycle2Out =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{cycle2Work}, batches);

	expectTrue(cycle1Out.summary.colorWriteCount > 0ULL, "cycle phase conformance scene should write pixels");
	expectEq(
		cycle1Out.summary.colorWriteCount,
		cycle2Out.summary.colorWriteCount,
		"cycle1/cycle2 should preserve covered pixel count");
	expectTrue(
		cycle1Out.summary.presentHash != cycle2Out.summary.presentHash,
		"cycle2 phase should produce output distinct from cycle1");
}

void testFillPhaseIgnoresBlendCombinerConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket fillA = makeFillWork(70ULL, 0x00A00000U, 0x00FF00FFU);
	fillA.combineMux = 0x123456789ABCDEF0ULL;
	fillA.blendParams = 0x01020304U;

	rvk2::RenderWorkPacket fillB = fillA;
	fillB.combineMux = 0x0FEDCBA987654321ULL;
	fillB.blendParams = 0x80818283U;

	const std::vector<rvk2::SubmissionBatchPacket> batches{makeSingleBatch()};
	const rvk2::ExecutorOutput outA =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{fillA}, batches);
	const rvk2::ExecutorOutput outB =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{fillB}, batches);

	expectEq(outA.summary.colorWriteCount, outB.summary.colorWriteCount, "fill phase write count should be stable");
	expectEq(
		outA.summary.presentHash,
		outB.summary.presentHash,
		"fill phase should ignore blend/combiner state for fill rect");
}

void testTexRectFlipConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket normal = makeTexRectWork(false);
	rvk2::RenderWorkPacket flipped = makeTexRectWork(true);
	const std::vector<rvk2::SubmissionBatchPacket> batches{makeSingleBatch()};

	const rvk2::ExecutorOutput outNormal =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{normal}, batches);
	const rvk2::ExecutorOutput outFlipped =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{flipped}, batches);

	expectEq(
		outNormal.summary.colorWriteCount,
		outFlipped.summary.colorWriteCount,
		"texrect flip should preserve covered pixel count");
	expectTrue(
		outNormal.summary.presentHash != outFlipped.summary.presentHash,
		"texrect flip should alter sampled output hash");
}

void testTexRectStateSensitivityConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket base = makeTexRectWork(false);
	base.sourcePacketId = 110ULL;
	base.colorImageAddress = 0x00E00000U;
	base.colorImageWidth = 8U;
	base.rectULX = 0U;
	base.rectULY = 0U;
	base.rectLRX = 5U;
	base.rectLRY = 5U;
	base.textureImageAddress = 0x00543210U;
	base.textureImageFormat = 2U;
	base.textureImageSize = 2U;
	base.textureImageWidth = 64U;
	base.tileFormat = 2U;
	base.tileSize = 2U;
	base.tileLine = 0x18U;
	base.tileTmem = 0x60U;
	base.tmemLoadKind = static_cast<u8>(rvk2::TmemLoadKind::kTile);
	base.tmemLoadTile = 1U;
	base.tmemLoadULS = 0x0020U;
	base.tmemLoadULT = 0x0010U;
	base.tmemLoadLRS = 0x00E0U;
	base.tmemLoadLRT = 0x00D0U;
	base.tmemLoadDXT = 0x0040U;

	const std::vector<rvk2::SubmissionBatchPacket> batches{makeSingleBatch()};
	const rvk2::ExecutorOutput baseOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{base}, batches);
	expectTrue(baseOut.summary.colorWriteCount > 0ULL, "texrect state sensitivity base scene should write pixels");

	rvk2::RenderWorkPacket tileVariant = base;
	tileVariant.tileMasks = 3U;
	tileVariant.tileMaskt = 4U;
	tileVariant.tileShifts = 2U;
	tileVariant.tileShiftt = 1U;
	tileVariant.tileCms = 1U;
	tileVariant.tileCmt = 3U;
	const rvk2::ExecutorOutput tileOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{tileVariant}, batches);
	expectEq(
		tileOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"tile descriptor transition should preserve texrect write count");
	expectTrue(
		tileOut.summary.presentHash != baseOut.summary.presentHash,
		"tile descriptor transition should alter texrect output hash");

	rvk2::RenderWorkPacket boundsVariant = base;
	boundsVariant.tileULS = 0x0040U;
	boundsVariant.tileULT = 0x0020U;
	boundsVariant.tileLRS = 0x00A0U;
	boundsVariant.tileLRT = 0x0080U;
	const rvk2::ExecutorOutput boundsOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{boundsVariant}, batches);
	expectEq(
		boundsOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"tile bounds transition should preserve texrect write count");
	expectTrue(
		boundsOut.summary.presentHash != baseOut.summary.presentHash,
		"tile bounds transition should alter texrect output hash");

	rvk2::RenderWorkPacket textureVariant = base;
	textureVariant.textureImageAddress ^= 0x00011100U;
	textureVariant.textureImageFormat ^= 0x3U;
	textureVariant.textureImageSize ^= 0x1U;
	textureVariant.textureImageWidth += 19U;
	const rvk2::ExecutorOutput textureOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{textureVariant}, batches);
	expectEq(
		textureOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"texture image transition should preserve texrect write count");
	expectTrue(
		textureOut.summary.presentHash != baseOut.summary.presentHash,
		"texture image transition should alter texrect output hash");

	rvk2::RenderWorkPacket loadVariant = base;
	loadVariant.tmemLoadKind = static_cast<u8>(rvk2::TmemLoadKind::kBlock);
	loadVariant.tmemLoadTile = 5U;
	loadVariant.tmemLoadULS = 0x0100U;
	loadVariant.tmemLoadULT = 0x0200U;
	loadVariant.tmemLoadLRS = 0x0300U;
	loadVariant.tmemLoadLRT = 0x0400U;
	loadVariant.tmemLoadDXT = 0x00C0U;
	const rvk2::ExecutorOutput loadOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{loadVariant}, batches);
	expectEq(
		loadOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"tmem load transition should preserve texrect write count");
	expectTrue(
		loadOut.summary.presentHash != baseOut.summary.presentHash,
		"tmem load transition should alter texrect output hash");
}

void testRenderStateInputSensitivityConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket base = makeTexRectWork(false);
	base.sourcePacketId = 120ULL;
	base.colorImageAddress = 0x00F00000U;
	base.colorImageWidth = 8U;
	base.rectULX = 0U;
	base.rectULY = 0U;
	base.rectLRX = 5U;
	base.rectLRY = 5U;
	base.otherModes = 0x123456789ABCDEF0ULL;
	base.primColor = 0xC06020FFU;
	base.envColor = 0x204080FFU;
	base.blendColor = 0xA0A0E0C0U;
	base.fogColor = 0x103050FFU;
	base.keyState = 0x0F0E0D0C0B0A0908ULL;
	base.convertState = 0x01030507090B0D0FULL;

	const std::vector<rvk2::SubmissionBatchPacket> batches{makeSingleBatch()};
	const rvk2::ExecutorOutput baseOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{base}, batches);
	expectTrue(baseOut.summary.colorWriteCount > 0ULL, "render state sensitivity base scene should write pixels");

	rvk2::RenderWorkPacket colorStateVariant = base;
	colorStateVariant.sourcePacketId = 121ULL;
	colorStateVariant.primColor ^= 0x00FF0000U;
	colorStateVariant.envColor ^= 0x0000FF00U;
	colorStateVariant.blendColor ^= 0x000000FFU;
	colorStateVariant.fogColor ^= 0xFF000000U;
	const rvk2::ExecutorOutput colorStateOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{colorStateVariant}, batches);
	expectEq(
		colorStateOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"color-state transition should preserve covered pixel count");
	expectTrue(
		colorStateOut.summary.presentHash != baseOut.summary.presentHash,
		"color-state transition should alter output hash");

	rvk2::RenderWorkPacket modeVariant = base;
	modeVariant.sourcePacketId = 122ULL;
	modeVariant.otherModes ^= 0x00FF00FF00FF00FFULL;
	const rvk2::ExecutorOutput modeOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{modeVariant}, batches);
	expectEq(
		modeOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"other-modes transition should preserve covered pixel count");
	expectTrue(
		modeOut.summary.presentHash != baseOut.summary.presentHash,
		"other-modes transition should alter output hash");

	rvk2::RenderWorkPacket keyConvertVariant = base;
	keyConvertVariant.sourcePacketId = 123ULL;
	keyConvertVariant.keyState ^= 0x55AA55AA55AA55AAULL;
	keyConvertVariant.convertState ^= 0x0F0F0F0F0F0F0F0FULL;
	const rvk2::ExecutorOutput keyConvertOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{keyConvertVariant}, batches);
	expectEq(
		keyConvertOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"key/convert transition should preserve covered pixel count");
	expectTrue(
		keyConvertOut.summary.presentHash != baseOut.summary.presentHash,
		"key/convert transition should alter output hash");
}

void testRenderTargetIsolationConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket firstTarget = makeFillWork(50ULL, 0x00700000U, 0x102030FFU);
	rvk2::RenderWorkPacket secondTarget = makeFillWork(51ULL, 0x00800000U, 0xA0B0C0FFU);

	rvk2::RenderWorkPacket firstTargetChanged = firstTarget;
	firstTargetChanged.fillColor = 0xFFEEDDFFU;

	const std::vector<rvk2::SubmissionBatchPacket> twoWorkBatch{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput baseOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{firstTarget, secondTarget},
		twoWorkBatch);
	const rvk2::ExecutorOutput changedOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{firstTargetChanged, secondTarget},
		twoWorkBatch);

	expectEq(baseOut.summary.surfaceCount, 2ULL, "render target isolation should allocate two surfaces");
	expectEq(
		baseOut.summary.presentHash,
		changedOut.summary.presentHash,
		"changes on non-presented render target should not affect present hash");
}

void testVIFilterModeConformance()
{
	const u32 colorAddress = 0x00AB0000U;
	constexpr u16 colorWidth = 4U;
	std::vector<rvk2::RenderWorkPacket> workPackets;
	workPackets.reserve(16U);
	static constexpr u32 pattern[16] = {
		0x707070FFU, 0x808080FFU, 0x707070FFU, 0x808080FFU,
		0x808080FFU, 0x707070FFU, 0x808080FFU, 0x707070FFU,
		0x707070FFU, 0x808080FFU, 0x707070FFU, 0x808080FFU,
		0x808080FFU, 0x707070FFU, 0x808080FFU, 0x707070FFU
	};
	for (u32 y = 0U; y < 4U; ++y) {
		for (u32 x = 0U; x < 4U; ++x) {
			const u32 idx = y * 4U + x;
			workPackets.push_back(
				makePixelFillWork(
					200ULL + static_cast<u64>(idx),
					colorAddress,
					colorWidth,
					x,
					y,
					pattern[idx]));
		}
	}
	const std::vector<rvk2::SubmissionBatchPacket> batches{
		makeBatchForWorkCount(static_cast<u32>(workPackets.size()))
	};

	rvk2::ExecutorConfig viConfig =
		makeVIExecutorConfig(2U | (3U << 8U), colorAddress, 4U, 4U, 8U);

	rvk2::Executor replicateExecutor(viConfig);
	const rvk2::ExecutorOutput replicateOut =
		replicateExecutor.executeWithOutput(workPackets, batches);
	expectTrue(
		!replicateOut.presentFrame.pixels.empty(),
		"VI filter conformance replicate scene should produce present pixels");

	rvk2::ExecutorConfig deditherConfig = viConfig;
	deditherConfig.viStatus |= 0x010000U;
	rvk2::Executor deditherExecutor(deditherConfig);
	const rvk2::ExecutorOutput deditherOut =
		deditherExecutor.executeWithOutput(workPackets, batches);
	expectEq(
		deditherOut.summary.colorWriteCount,
		replicateOut.summary.colorWriteCount,
		"VI dedither should preserve covered pixel count");
	expectTrue(
		deditherOut.summary.presentHash != replicateOut.summary.presentHash,
		"VI dedither should alter present hash in compatible AA mode");

	rvk2::ExecutorConfig aaNeededConfig = viConfig;
	aaNeededConfig.viStatus = 2U | (1U << 8U);
	rvk2::Executor aaNeededExecutor(aaNeededConfig);
	const rvk2::ExecutorOutput aaNeededNoDeditherOut =
		aaNeededExecutor.executeWithOutput(workPackets, batches);

	rvk2::ExecutorConfig aaNeededDeditherConfig = aaNeededConfig;
	aaNeededDeditherConfig.viStatus |= 0x010000U;
	rvk2::Executor aaNeededDeditherExecutor(aaNeededDeditherConfig);
	const rvk2::ExecutorOutput aaNeededWithDeditherOut =
		aaNeededDeditherExecutor.executeWithOutput(workPackets, batches);
	expectEq(
		aaNeededWithDeditherOut.summary.presentHash,
		aaNeededNoDeditherOut.summary.presentHash,
		"VI dedither should be inactive in AA-needed mode");
	expectTrue(
		presentFramesEqual(aaNeededWithDeditherOut, aaNeededNoDeditherOut),
		"VI dedither should not alter present frame in AA-needed mode");
}

void testVIFailSafeConformance()
{
	const rvk2::RenderWorkPacket fillWork =
		makePixelFillWork(250ULL, 0x00AC0000U, 2U, 0U, 0U, 0x203040FFU);
	const std::vector<rvk2::RenderWorkPacket> workPackets{fillWork};
	const std::vector<rvk2::SubmissionBatchPacket> batches{makeSingleBatch()};

	rvk2::ExecutorConfig validConfig =
		makeVIExecutorConfig(3U | (3U << 8U), fillWork.colorImageAddress, 2U, 2U, 4U);
	rvk2::Executor validExecutor(validConfig);
	const rvk2::ExecutorOutput validOut =
		validExecutor.executeWithOutput(workPackets, batches);
	expectTrue(
		validOut.presentFrame.width > 0U && validOut.presentFrame.height > 0U,
		"VI fail-safe baseline should produce non-empty present frame");

	rvk2::ExecutorConfig reservedTypeConfig = validConfig;
	reservedTypeConfig.viStatus = 1U | (3U << 8U);
	rvk2::Executor reservedTypeExecutor(reservedTypeConfig);
	const rvk2::ExecutorOutput reservedTypeOut =
		reservedTypeExecutor.executeWithOutput(workPackets, batches);
	expectEq(
		reservedTypeOut.presentFrame.width,
		0U,
		"VI reserved type should blank present width");
	expectEq(
		reservedTypeOut.presentFrame.height,
		0U,
		"VI reserved type should blank present height");

	rvk2::ExecutorConfig zeroWidthConfig = validConfig;
	zeroWidthConfig.viWidth = 0U;
	rvk2::Executor zeroWidthExecutor(zeroWidthConfig);
	const rvk2::ExecutorOutput zeroWidthOut =
		zeroWidthExecutor.executeWithOutput(workPackets, batches);
	expectEq(
		zeroWidthOut.presentFrame.width,
		0U,
		"VI zero width should blank present width");
	expectEq(
		zeroWidthOut.presentFrame.height,
		0U,
		"VI zero width should blank present height");

	rvk2::ExecutorConfig invalidStride16Config = validConfig;
	invalidStride16Config.viStatus = 2U | (3U << 8U);
	invalidStride16Config.viWidth = 2U;
	rvk2::Executor invalidStride16Executor(invalidStride16Config);
	const rvk2::ExecutorOutput invalidStride16Out =
		invalidStride16Executor.executeWithOutput(workPackets, batches);
	expectEq(
		invalidStride16Out.presentFrame.width,
		0U,
		"VI invalid 16bpp stride should blank present width");
	expectEq(
		invalidStride16Out.presentFrame.height,
		0U,
		"VI invalid 16bpp stride should blank present height");

	rvk2::ExecutorConfig invalidStride32Config = validConfig;
	invalidStride32Config.viStatus = 3U | (3U << 8U);
	invalidStride32Config.viWidth = 1U;
	rvk2::Executor invalidStride32Executor(invalidStride32Config);
	const rvk2::ExecutorOutput invalidStride32Out =
		invalidStride32Executor.executeWithOutput(workPackets, batches);
	expectEq(
		invalidStride32Out.presentFrame.width,
		0U,
		"VI invalid 32bpp stride should blank present width");
	expectEq(
		invalidStride32Out.presentFrame.height,
		0U,
		"VI invalid 32bpp stride should blank present height");
}

void testVIPixelAdvanceConformance()
{
	const u32 colorAddress = 0x00AD0000U;
	constexpr u16 colorWidth = 4U;
	std::vector<rvk2::RenderWorkPacket> workPackets;
	workPackets.reserve(8U);
	static constexpr u32 pattern[8] = {
		0x101010FFU, 0x202020FFU, 0x303030FFU, 0x404040FFU,
		0x505050FFU, 0x606060FFU, 0x707070FFU, 0x808080FFU
	};
	for (u32 y = 0U; y < 2U; ++y) {
		for (u32 x = 0U; x < 4U; ++x) {
			const u32 idx = y * 4U + x;
			workPackets.push_back(
				makePixelFillWork(
					300ULL + static_cast<u64>(idx),
					colorAddress,
					colorWidth,
					x,
					y,
					pattern[idx]));
		}
	}

	const std::vector<rvk2::SubmissionBatchPacket> batches{
		makeBatchForWorkCount(static_cast<u32>(workPackets.size()))
	};
	const rvk2::ExecutorConfig baseConfig =
		makeVIExecutorConfig(3U | (3U << 8U), colorAddress, 4U, 2U, 4U);
	rvk2::Executor baseExecutor(baseConfig);
	const rvk2::ExecutorOutput baseOut =
		baseExecutor.executeWithOutput(workPackets, batches);
	expectTrue(
		!baseOut.presentFrame.pixels.empty(),
		"VI pixel-advance base scene should produce present pixels");

	rvk2::ExecutorConfig shiftedConfig = baseConfig;
	shiftedConfig.viStatus = (3U | (3U << 8U)) | (4U << 12U);
	rvk2::Executor shiftedExecutor(shiftedConfig);
	const rvk2::ExecutorOutput shiftedOut =
		shiftedExecutor.executeWithOutput(workPackets, batches);
	expectEq(
		shiftedOut.presentFrame.width,
		baseOut.presentFrame.width,
		"VI pixel-advance should preserve present width");
	expectEq(
		shiftedOut.presentFrame.height,
		baseOut.presentFrame.height,
		"VI pixel-advance should preserve present height");
	expectTrue(
		shiftedOut.summary.presentHash != baseOut.summary.presentHash,
		"VI pixel-advance should alter present hash");
	expectEq(
		shiftedOut.presentFrame.pixels[0],
		baseOut.presentFrame.pixels[1],
		"VI pixel-advance should shift first visible sample");

	rvk2::ExecutorConfig overflowConfig = baseConfig;
	overflowConfig.viStatus = (3U | (3U << 8U)) | (12U << 12U);
	rvk2::Executor overflowExecutor(overflowConfig);
	const rvk2::ExecutorOutput overflowOut =
		overflowExecutor.executeWithOutput(workPackets, batches);
	expectTrue(
		overflowOut.summary.presentHash != baseOut.summary.presentHash,
		"VI pixel-advance overflow should alter present hash");
	expectEq(
		overflowOut.presentFrame.pixels[1],
		0x00000000U,
		"VI pixel-advance overflow should clip out-of-range sample to black");
}

} // namespace

int main()
{
	testCyclePhaseConformance();
	testBlendSensitivityConformance();
	testBlendDestinationDependencyConformance();
	testCombinerMuxConformance();
	testDepthOrderingConformance();
	testDepthPhaseParticipationConformance();
	testPrimitiveDepthSourceConformance();
	testDepthCompareUpdateModeConformance();
	testDepthSurfaceAliasIsolationConformance();
	testCoverageBlendFlagConformance();
	testCoverageModeFlagConformance();
	testAlphaCompareConformance();
	testMixedStateBatchSegmentationConformance();
	testMixedStateRapidTransitionMatrixConformance();
	testCoverageScissorConformance();
	testCopyPhaseDestinationBypassConformance();
	testCycle2PhaseDistinctConformance();
	testFillPhaseIgnoresBlendCombinerConformance();
	testTexRectFlipConformance();
	testTexRectStateSensitivityConformance();
	testRenderStateInputSensitivityConformance();
	testRenderTargetIsolationConformance();
	testVIFilterModeConformance();
	testVIFailSafeConformance();
	testVIPixelAdvanceConformance();

	if (g_failures == 0) {
		std::printf("rvk2 conformance tests: PASS\n");
		return 0;
	}

	std::fprintf(stderr, "rvk2 conformance tests: FAIL (%d failure(s))\n", g_failures);
	return 1;
}
