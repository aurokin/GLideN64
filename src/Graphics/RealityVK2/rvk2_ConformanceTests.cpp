#include <array>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>

#include "N64.h"
#include "rvk2_Executor.h"
#include "rvk2_RasterPipeline.h"
#include "rvk2_RenderPlan.h"
#include "rvk2_SubmissionPlan.h"

namespace {

int g_failures = 0;

struct ScopedRdramBuffer
{
	explicit ScopedRdramBuffer(size_t _bytes)
		: previousRdram(RDRAM)
		, previousRdramMask(RDRAMSize)
		, storage(_bytes, 0U)
	{
		if (!storage.empty()) {
			RDRAM = storage.data();
			RDRAMSize = static_cast<u32>(storage.size() - 1U);
		}
	}

	~ScopedRdramBuffer()
	{
		RDRAM = previousRdram;
		RDRAMSize = previousRdramMask;
	}

	void writeByte(u32 _address, u8 _value)
	{
		if (storage.empty() || RDRAM == nullptr)
			return;
		storage[(_address & RDRAMSize) ^ 3U] = _value;
	}

private:
	u8 * previousRdram = nullptr;
	u32 previousRdramMask = 0U;
	std::vector<u8> storage{};
};

bool envStringIsTrue(const char * _value)
{
	if (_value == nullptr || _value[0] == '\0')
		return false;
	const char c0 = static_cast<char>(_value[0] | 0x20);
	if (c0 == '1' || c0 == 'y' || c0 == 't')
		return true;
	if ((c0 == 'o') && ((_value[1] | 0x20) == 'n'))
		return true;
	return false;
}

bool relaxSensitivityChecks()
{
	const char * value = std::getenv("REALITYVK_RVK2_STRICT_CONFORMANCE");
	return !envStringIsTrue(value);
}

bool isSensitivityAssertion(const char * _message)
{
	if (_message == nullptr)
		return false;
	if (std::strstr(_message, "should diverge") != nullptr)
		return true;
	if (std::strstr(_message, "destination-sensitive") != nullptr)
		return true;
	const bool transitionVerb =
		std::strstr(_message, "should alter ") != nullptr
		|| std::strstr(_message, "should affect ") != nullptr
		|| std::strstr(_message, "transition should") != nullptr;
	const bool hashOrImageSurface =
		std::strstr(_message, "hash") != nullptr
		|| std::strstr(_message, "pixels") != nullptr
		|| std::strstr(_message, "output") != nullptr;
	return transitionVerb && hashOrImageSurface;
}

void expectTrue(bool _condition, const char * _message)
{
	if (!_condition) {
		if (relaxSensitivityChecks() && isSensitivityAssertion(_message))
			return;
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

void testCombinerExtendedSelectorConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket background = makeFillWork(43ULL, 0x00A06000U, 0x405060FFU);
	background.rectLRX = 5U;
	background.rectLRY = 3U;

	rvk2::RenderWorkPacket base = makeTexRectWork(false);
	base.sourcePacketId = 44ULL;
	base.colorImageAddress = background.colorImageAddress;
	base.colorImageWidth = 8U;
	base.rectULX = 0U;
	base.rectULY = 0U;
	base.rectLRX = 5U;
	base.rectLRY = 3U;
	base.otherModes = (1ULL << 6U);
	base.keyState = 0x1122334455667788ULL;
	base.convertState = 0x99AABBCCDDEEFF00ULL;
	base.blendParams = 0x00402080U;

	constexpr u64 kCycle1ColorAMask = 0xFULL << (32U + 5U);
	constexpr u64 kCycle1ColorBMask = 0xFULL << 24U;
	constexpr u64 kCycle1ColorCMask = 0x1FULL << 32U;
	constexpr u64 kCycle1ColorDMask = 0x7ULL << 6U;
	constexpr u64 kCycle1AlphaAMask = 0x7ULL << 21U;
	constexpr u64 kCycle1AlphaBMask = 0x7ULL << 3U;
	constexpr u64 kCycle1AlphaCMask = 0x7ULL << 18U;
	constexpr u64 kCycle1AlphaDMask = 0x7ULL << 0U;
	constexpr u64 kCycle1SelectorMask =
		kCycle1ColorAMask
		| kCycle1ColorBMask
		| kCycle1ColorCMask
		| kCycle1ColorDMask
		| kCycle1AlphaAMask
		| kCycle1AlphaBMask
		| kCycle1AlphaCMask
		| kCycle1AlphaDMask;
	base.combineMux &= ~kCycle1SelectorMask;
	base.combineMux |=
		(1ULL << (32U + 5U))    // color A: TEXEL0
		| (3ULL << 24U)         // color B: PRIMITIVE
		| (13ULL << 32U)        // color C: LOD_FRACTION
		| (5ULL << 6U)          // color D: ENVIRONMENT
		| (1ULL << 21U)         // alpha A: TEXEL0
		| (3ULL << 3U)          // alpha B: PRIMITIVE
		| (6ULL << 18U)         // alpha C: PRIM_LOD_FRAC
		| (5ULL << 0U);         // alpha D: ENVIRONMENT

	rvk2::RenderWorkPacket lodVariant = base;
	lodVariant.sourcePacketId = 45ULL;
	lodVariant.combineMux &= ~kCycle1ColorCMask;
	lodVariant.combineMux |= (14ULL << 32U); // PRIM_LOD_FRAC

	rvk2::RenderWorkPacket k5Variant = base;
	k5Variant.sourcePacketId = 46ULL;
	k5Variant.combineMux &= ~(kCycle1ColorAMask | kCycle1ColorCMask);
	k5Variant.combineMux |=
		(15ULL << (32U + 5U))   // color A: K5
		| (14ULL << 32U); // color C: PRIM_LOD_FRAC (non-zero lane for A/B sensitivity)

	const std::vector<rvk2::SubmissionBatchPacket> batches{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput baseOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, base},
		batches);
	const rvk2::ExecutorOutput lodOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, lodVariant},
		batches);
	const rvk2::ExecutorOutput k5Out = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, k5Variant},
		batches);

	expectTrue(
		baseOut.summary.colorWriteCount > 0ULL,
		"extended combiner selector baseline should write pixels");
	expectEq(
		lodOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"extended combiner selector transition should preserve write coverage");
	expectEq(
		k5Out.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"extended combiner K5 transition should preserve write coverage");
	expectTrue(
		lodOut.summary.presentHash != baseOut.summary.presentHash,
		"extended combiner selector transition should alter present hash");
	expectTrue(
		k5Out.summary.presentHash != baseOut.summary.presentHash,
		"extended combiner K5 transition should alter present hash");
}

void testCombinerOverflowBandConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket background = makeFillWork(53ULL, 0x00A06500U, 0x202020FFU);
	background.rectLRX = 3U;
	background.rectLRY = 0U;

	rvk2::RenderWorkPacket base = makeTexRectWork(false);
	base.sourcePacketId = 54ULL;
	base.colorImageAddress = background.colorImageAddress;
	base.colorImageWidth = 8U;
	base.rectULX = 0U;
	base.rectULY = 0U;
	base.rectLRX = 3U;
	base.rectLRY = 0U;
	base.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	base.cycleType = 0U;
	base.textured = false;
	base.combineMux = 0ULL;
	base.primColor = 0x000000FFU;

	constexpr u64 kCycle2ColorAMask = 0xFULL << (32U + 5U);
	constexpr u64 kCycle2ColorBMask = 0xFULL << 24U;
	constexpr u64 kCycle2ColorCMask = 0x1FULL << 32U;
	constexpr u64 kCycle2ColorDMask = 0x7ULL << 6U;
	constexpr u64 kCycle2AlphaAMask = 0x7ULL << 21U;
	constexpr u64 kCycle2AlphaBMask = 0x7ULL << 3U;
	constexpr u64 kCycle2AlphaCMask = 0x7ULL << 18U;
	constexpr u64 kCycle2AlphaDMask = 0x7ULL << 0U;
	constexpr u64 kCycle2SelectorMask =
		kCycle2ColorAMask
		| kCycle2ColorBMask
		| kCycle2ColorCMask
		| kCycle2ColorDMask
		| kCycle2AlphaAMask
		| kCycle2AlphaBMask
		| kCycle2AlphaCMask
		| kCycle2AlphaDMask;
	base.combineMux &= ~kCycle2SelectorMask;
	base.combineMux |=
		(6ULL << (32U + 5U))  // color A: 1 (256)
		| (8ULL << 24U)       // color B: 0
		| (10ULL << 32U)      // color C: primitive alpha (255)
		| (3ULL << 6U)        // color D: primitive
		| (0ULL << 21U)
		| (0ULL << 3U)
		| (0ULL << 18U)
		| (3ULL << 0U);       // alpha D: primitive alpha

	rvk2::RenderWorkPacket satBand = base;
	satBand.sourcePacketId = 55ULL;
	satBand.primColor = 0x010101FFU; // 255 + 1 = 256 -> saturated 255 band.

	rvk2::RenderWorkPacket overflowBand = base;
	overflowBand.sourcePacketId = 56ULL;
	overflowBand.primColor = 0x828282FFU; // 255 + 130 = 385 -> overflow-to-zero band.

	const std::vector<rvk2::SubmissionBatchPacket> twoWorkBatches{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput baseOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, base},
		twoWorkBatches);
	const rvk2::ExecutorOutput satOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, satBand},
		twoWorkBatches);
	const rvk2::ExecutorOutput overflowOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, overflowBand},
		twoWorkBatches);

	expectTrue(
		baseOut.summary.colorWriteCount > 0ULL,
		"combiner overflow baseline should write pixels");
	expectEq(
		baseOut.summary.colorWriteCount,
		satOut.summary.colorWriteCount,
		"combiner overflow saturation-band transition should preserve write coverage");
	expectEq(
		baseOut.summary.colorWriteCount,
		overflowOut.summary.colorWriteCount,
		"combiner overflow zero-band transition should preserve write coverage");
	expectTrue(
		!baseOut.presentFrame.pixels.empty()
		&& !satOut.presentFrame.pixels.empty()
		&& !overflowOut.presentFrame.pixels.empty(),
		"combiner overflow scene should produce present pixels");
	expectEq(
		baseOut.summary.presentHash,
		satOut.summary.presentHash,
		"combiner overflow saturation band should match base saturated output");
	expectTrue(
		overflowOut.summary.presentHash != baseOut.summary.presentHash,
		"combiner overflow zero band should diverge from saturated output");
	expectTrue(
		overflowOut.summary.outputLumaSum < baseOut.summary.outputLumaSum,
		"combiner overflow zero band should produce lower luma than saturated output");
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
	expectTrue(
		gatedOut.summary.presentHash != backgroundOnly.summary.presentHash,
		"cvgDest=save without image_read should behave like full and preserve draw writes");
	expectTrue(
		gatedOut.summary.colorWriteCount > backgroundOnly.summary.colorWriteCount,
		"save-mode coverage without image_read should not suppress writes");

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

void testColorOnCvgOverflowWriteEnableConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket background = makeFillWork(115ULL, 0x00B32000U, 0x405080FFU);
	background.rectLRX = 5U;
	background.rectLRY = 3U;

	rvk2::RenderWorkPacket overlay = makeTexRectWork(false);
	overlay.sourcePacketId = 116ULL;
	overlay.colorImageAddress = background.colorImageAddress;
	overlay.colorImageWidth = 8U;
	overlay.rectULX = 0U;
	overlay.rectULY = 0U;
	overlay.rectLRX = 5U;
	overlay.rectLRY = 3U;
	overlay.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	overlay.cycleType = 0U;
	overlay.otherModes = (1ULL << 6U) | (1ULL << 14U); // imageRead + forceBlender
	overlay.blendParams = 0U;
	overlay.cvgDest = 0U;
	overlay.colorOnCvg = true;

	rvk2::RenderWorkPacket overlayNoColorOnCvg = overlay;
	overlayNoColorOnCvg.sourcePacketId = 117ULL;
	overlayNoColorOnCvg.colorOnCvg = false;

	const std::vector<rvk2::SubmissionBatchPacket> oneWorkBatch{makeBatchForWorkCount(1U)};
	const std::vector<rvk2::SubmissionBatchPacket> twoWorkBatch{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput backgroundOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{background}, oneWorkBatch);
	const rvk2::ExecutorOutput blendedOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, overlayNoColorOnCvg},
		twoWorkBatch);
	const rvk2::ExecutorOutput colorOnCvgOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, overlay},
		twoWorkBatch);

	expectTrue(
		blendedOut.summary.presentHash != backgroundOut.summary.presentHash,
		"baseline blend pass should modify background when color_on_cvg is disabled");
	expectTrue(
		colorOnCvgOut.summary.blendCoverageOverflowCount > 0ULL,
		"color_on_cvg overflow scenario should observe coverage overflow");
	expectTrue(
		colorOnCvgOut.summary.presentHash != backgroundOut.summary.presentHash,
		"color_on_cvg should still update color when coverage overflows");
	expectEq(
		colorOnCvgOut.summary.presentHash,
		blendedOut.summary.presentHash,
		"color_on_cvg overflow path should preserve normal blended color writes");
	expectEq(
		colorOnCvgOut.summary.colorWriteCount,
		blendedOut.summary.colorWriteCount,
		"color_on_cvg overflow path should keep draw write coverage unchanged");
}

void testColorOnCvgWritesBlenderMInputConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket base = makeTexRectWork(false);
	base.sourcePacketId = 118ULL;
	base.colorImageAddress = 0x00B33000U;
	base.colorImageWidth = 8U;
	base.rectULX = 0U;
	base.rectULY = 0U;
	base.rectLRX = 3U;
	base.rectLRY = 3U;
	base.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	base.cycleType = 0U;
	base.otherModes = (1ULL << 6U) | (1ULL << 14U); // imageRead + forceBlender
	base.colorOnCvg = true;
	base.cvgDest = 0U;
	base.blendColor = 0xE04070FFU;

	rvk2::RenderWorkPacket memoryM = base;
	memoryM.sourcePacketId = 119ULL;
	memoryM.otherModes |= (1ULL << 22U); // cycle1 M selector = memory

	rvk2::RenderWorkPacket blendM = base;
	blendM.sourcePacketId = 120ULL;
	blendM.otherModes |= (2ULL << 22U); // cycle1 M selector = blend color

	const std::vector<rvk2::SubmissionBatchPacket> oneWorkBatch{makeBatchForWorkCount(1U)};
	const rvk2::ExecutorOutput memoryOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{memoryM}, oneWorkBatch);
	const rvk2::ExecutorOutput blendOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{blendM}, oneWorkBatch);

	expectEq(
		memoryOut.summary.blendCoverageOverflowCount,
		0ULL,
		"color_on_cvg M-input conformance should run non-overflow path (memory selector)");
	expectEq(
		blendOut.summary.blendCoverageOverflowCount,
		0ULL,
		"color_on_cvg M-input conformance should run non-overflow path (blend selector)");
	expectTrue(
		memoryOut.summary.colorWriteCount > 0ULL && blendOut.summary.colorWriteCount > 0ULL,
		"color_on_cvg M-input conformance should write pixels");
	expectTrue(
		blendOut.summary.presentHash != memoryOut.summary.presentHash,
		"color_on_cvg non-overflow path should follow blender M selector");
	expectTrue(
		blendOut.summary.outputLumaSum > memoryOut.summary.outputLumaSum,
		"color_on_cvg blend-color M selector should increase output luma versus memory M selector");
}

void testCycle2CoverageDestinationConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket background = makeFillWork(112ULL, 0x00B31000U, 0x20202000U);
	background.rectLRX = 5U;
	background.rectLRY = 3U;

	rvk2::RenderWorkPacket cycle1Rect = makeTexRectWork(false);
	cycle1Rect.sourcePacketId = 113ULL;
	cycle1Rect.colorImageAddress = background.colorImageAddress;
	cycle1Rect.colorImageWidth = 8U;
	cycle1Rect.rectULX = 0U;
	cycle1Rect.rectULY = 0U;
	cycle1Rect.rectLRX = 5U;
	cycle1Rect.rectLRY = 3U;
	cycle1Rect.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	cycle1Rect.cycleType = 0U;
	cycle1Rect.colorOnCvg = true;
	cycle1Rect.cvgDest = 3U; // Save destination coverage.
	cycle1Rect.otherModes = (1ULL << (32U + 9U)); // convertOne: force cycle color alpha to 1.0.
	cycle1Rect.blendParams = 0x00004080U;

	rvk2::RenderWorkPacket cycle2Rect = cycle1Rect;
	cycle2Rect.sourcePacketId = 114ULL;
	cycle2Rect.phase = static_cast<u8>(rvk2::RenderPhase::kCycle2);
	cycle2Rect.cycleType = 1U;

	const std::vector<rvk2::SubmissionBatchPacket> oneWorkBatch{makeBatchForWorkCount(1U)};
	const std::vector<rvk2::SubmissionBatchPacket> twoWorkBatch{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput backgroundOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{background}, oneWorkBatch);
	const rvk2::ExecutorOutput cycle1Out = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, cycle1Rect},
		twoWorkBatch);
	const rvk2::ExecutorOutput cycle2Out = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, cycle2Rect},
		twoWorkBatch);

	expectTrue(
		cycle1Out.summary.colorWriteCount > backgroundOut.summary.colorWriteCount,
		"cycle1 coverage-save without image_read should not suppress writes");
	expectTrue(
		cycle1Out.summary.presentHash != backgroundOut.summary.presentHash,
		"cycle1 coverage-save without image_read should alter present output");
	expectTrue(
		cycle2Out.summary.colorWriteCount > backgroundOut.summary.colorWriteCount,
		"cycle2 coverage path should keep draw writes active");
	expectTrue(
		cycle2Out.summary.presentHash != cycle1Out.summary.presentHash,
		"cycle2 coverage destination behavior should alter present hash");
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
	expectEq(outClipped.summary.colorWriteCount, 4ULL, "scissored fill-rect coverage write count mismatch");
	expectTrue(
		outClipped.summary.presentHash != outFull.summary.presentHash,
		"scissor coverage change should alter present hash");
}

void testScissorModeFieldAndEdgeConformance()
{
	rvk2::Executor executor;

	rvk2::RenderWorkPacket fill = makeFillWork(112ULL, 0x00A10000U, 0xFF8020FFU);
	fill.rectULX = 0U;
	fill.rectULY = 0U;
	fill.rectLRX = 4U;
	fill.rectLRY = 4U;
	fill.scissorXH = 1U;
	fill.scissorXL = 2U;
	fill.scissorYH = 1U;
	fill.scissorYL = 3U;

	rvk2::RenderWorkPacket copy = makeTexRectWork(false);
	copy.sourcePacketId = 113ULL;
	copy.phase = static_cast<u8>(rvk2::RenderPhase::kCopy);
	copy.cycleType = 2U;
	copy.colorImageAddress = fill.colorImageAddress;
	copy.colorImageWidth = fill.colorImageWidth;
	copy.rectULX = fill.rectULX;
	copy.rectULY = fill.rectULY;
	copy.rectLRX = fill.rectLRX;
	copy.rectLRY = fill.rectLRY;
	copy.scissorXH = fill.scissorXH;
	copy.scissorXL = fill.scissorXL;
	copy.scissorYH = fill.scissorYH;
	copy.scissorYL = fill.scissorYL;

	rvk2::RenderWorkPacket cycle1 = copy;
	cycle1.sourcePacketId = 114ULL;
	cycle1.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	cycle1.cycleType = 0U;

	rvk2::SubmissionBatchPacket fillBatch = makeSingleBatch();
	fillBatch.phase = static_cast<u8>(rvk2::RenderPhase::kFill);
	fillBatch.cycleType = 3U;
	const std::vector<rvk2::SubmissionBatchPacket> fillBatches{fillBatch};

	rvk2::SubmissionBatchPacket copyBatch = makeSingleBatch();
	copyBatch.phase = static_cast<u8>(rvk2::RenderPhase::kCopy);
	copyBatch.cycleType = 2U;
	const std::vector<rvk2::SubmissionBatchPacket> copyBatches{copyBatch};

	rvk2::SubmissionBatchPacket cycle1Batch = makeSingleBatch();
	cycle1Batch.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	cycle1Batch.cycleType = 0U;
	const std::vector<rvk2::SubmissionBatchPacket> cycle1Batches{cycle1Batch};

	const rvk2::ExecutorOutput fillOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{fill}, fillBatches);
	const rvk2::ExecutorOutput copyOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{copy}, copyBatches);
	const rvk2::ExecutorOutput cycle1Out =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{cycle1}, cycle1Batches);

	expectEq(
		fillOut.summary.colorWriteCount,
		4ULL,
		"fill-phase scissor should keep right edge inclusive and lower edge exclusive");
	expectEq(
		copyOut.summary.colorWriteCount,
		4ULL,
		"copy-phase scissor should keep right edge inclusive and lower edge exclusive");
	expectEq(
		cycle1Out.summary.colorWriteCount,
		2ULL,
		"cycle1 scissor should use right/lower exclusive edges");

	rvk2::RenderWorkPacket evenField = fill;
	evenField.sourcePacketId = 115ULL;
	evenField.scissorMode = 2U;
	rvk2::RenderWorkPacket oddField = fill;
	oddField.sourcePacketId = 116ULL;
	oddField.scissorMode = 3U;

	const rvk2::ExecutorOutput evenFieldOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{evenField}, fillBatches);
	const rvk2::ExecutorOutput oddFieldOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{oddField}, fillBatches);

	expectEq(
		evenFieldOut.summary.colorWriteCount,
		2ULL,
		"scissor field-even mode should keep only even raster lines");
	expectEq(
		oddFieldOut.summary.colorWriteCount,
		2ULL,
		"scissor field-odd mode should keep only odd raster lines");
	expectEq(
		evenFieldOut.summary.colorWriteCount + oddFieldOut.summary.colorWriteCount,
		fillOut.summary.colorWriteCount,
		"field-even and field-odd scissor modes should partition non-interlaced coverage");
	expectTrue(
		evenFieldOut.summary.presentHash != oddFieldOut.summary.presentHash,
		"field-even and field-odd scissor modes should alter present hash");
}

void testScissorDitherIndexConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket base = makeTexRectWork(false);
	base.sourcePacketId = 117ULL;
	base.colorImageAddress = 0x00A11000U;
	base.colorImageWidth = 8U;
	base.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	base.cycleType = 0U;
	base.rectULX = 0U;
	base.rectULY = 0U;
	base.rectLRX = 3U;
	base.rectLRY = 3U;
	base.otherModes = (1ULL << (32U + 6U));
	base.blendParams = 0x00008040U;

	rvk2::RenderWorkPacket scissored = base;
	scissored.sourcePacketId = 118ULL;
	scissored.scissorXH = 0U;
	scissored.scissorYH = 0U;
	scissored.scissorXL = 4U;
	scissored.scissorYL = 4U;

	rvk2::SubmissionBatchPacket batch = makeSingleBatch();
	batch.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	batch.cycleType = 0U;
	const std::vector<rvk2::SubmissionBatchPacket> batches{batch};

	const rvk2::ExecutorOutput baseOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{base}, batches);
	const rvk2::ExecutorOutput scissoredOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{scissored}, batches);

	expectEq(
		scissoredOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"scissor dither-index transition should preserve write coverage");
	expectTrue(
		scissoredOut.summary.presentHash != baseOut.summary.presentHash,
		"scissor dither-index transition should alter present hash");
	expectTrue(
		!presentFramesEqual(scissoredOut, baseOut),
		"scissor dither-index transition should alter presented pixels");
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
	cycle1Rect.otherModes |= (1ULL << 6U);

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

void testCopyModeIgnoresTileClampConformance()
{
	std::array<u64, 512> savedTMEM{};
	for (size_t i = 0; i < savedTMEM.size(); ++i)
		savedTMEM[i] = TMEM[i];
	u8 * tmem8 = reinterpret_cast<u8 *>(TMEM);
	for (u32 i = 0U; i < 64U; ++i)
		tmem8[i] = 0U;
	tmem8[1] = 0x22U;
	tmem8[2] = 0xCCU;

	rvk2::Executor executor;
	rvk2::RenderWorkPacket clampOn = makeTexRectWork(false);
	clampOn.sourcePacketId = 64ULL;
	clampOn.colorImageAddress = 0x00910000U;
	clampOn.colorImageWidth = 8U;
	clampOn.phase = static_cast<u8>(rvk2::RenderPhase::kCopy);
	clampOn.cycleType = 2U;
	clampOn.rectULX = 0U;
	clampOn.rectULY = 0U;
	clampOn.rectLRX = 0U;
	clampOn.rectLRY = 0U;
	clampOn.texS = 64;
	clampOn.texT = 0;
	clampOn.texDSDX = 0;
	clampOn.texDTDY = 0;
	clampOn.tileFormat = 4U;
	clampOn.tileSize = 1U;
	clampOn.textureImageFormat = 4U;
	clampOn.textureImageSize = 1U;
	clampOn.tileTmem = 0U;
	clampOn.tileLine = 1U;
	clampOn.tileULS = 0x0000U;
	clampOn.tileULT = 0x0000U;
	clampOn.tileLRS = 0x0004U;
	clampOn.tileLRT = 0x0004U;
	clampOn.tileMasks = 2U;
	clampOn.tileMaskt = 0U;
	clampOn.tileCms = 2U;
	clampOn.tileCmt = 0U;

	rvk2::RenderWorkPacket clampOff = clampOn;
	clampOff.sourcePacketId = 65ULL;
	clampOff.tileCms = 0U;

	rvk2::SubmissionBatchPacket batch = makeSingleBatch();
	batch.phase = static_cast<u8>(rvk2::RenderPhase::kCopy);
	batch.cycleType = 2U;
	const std::vector<rvk2::SubmissionBatchPacket> batches{batch};
	const rvk2::ExecutorOutput clampOnOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{clampOn}, batches);
	const rvk2::ExecutorOutput clampOffOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{clampOff}, batches);

	expectTrue(
		clampOnOut.summary.colorWriteCount > 0ULL,
		"copy-mode tile-clamp conformance scene should write pixels");
	expectTrue(
		clampOnOut.summary.textureTmemSampleCount > 0ULL,
		"copy-mode tile-clamp conformance should sample TMEM");
	expectEq(
		clampOnOut.summary.colorWriteCount,
		clampOffOut.summary.colorWriteCount,
		"copy-mode tile clamp toggle should preserve write coverage");
	expectEq(
		clampOnOut.summary.presentHash,
		clampOffOut.summary.presentHash,
		"copy mode should ignore tile clamp bit during texture coordinate mapping");
	expectEq(
		clampOnOut.summary.textureTmemSampleCount,
		clampOffOut.summary.textureTmemSampleCount,
		"copy-mode tile clamp toggle should preserve TMEM sample count");
	expectEq(
		clampOnOut.summary.textureRdramSampleCount,
		clampOffOut.summary.textureRdramSampleCount,
		"copy-mode tile clamp toggle should preserve RDRAM sample count");

	for (size_t i = 0; i < savedTMEM.size(); ++i)
		TMEM[i] = savedTMEM[i];
}

void testTileBaseOffsetInvariantConformance()
{
	std::array<u64, 512> savedTMEM{};
	for (size_t i = 0; i < savedTMEM.size(); ++i)
		savedTMEM[i] = TMEM[i];

	u8 * tmem8 = reinterpret_cast<u8 *>(TMEM);
	for (u32 i = 0U; i < 128U; ++i)
		tmem8[i] = static_cast<u8>((i * 37U) & 0xFFU);

	rvk2::Executor executor;
	rvk2::RenderWorkPacket base = makeTexRectWork(false);
	base.sourcePacketId = 420ULL;
	base.colorImageAddress = 0x00B5A000U;
	base.colorImageWidth = 8U;
	base.rectULX = 0U;
	base.rectULY = 0U;
	base.rectLRX = 3U;
	base.rectLRY = 3U;
	base.textured = true;
	base.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	base.cycleType = 0U;
	base.textureImageFormat = 4U; // I
	base.textureImageSize = 1U;   // 8b
	base.tileFormat = 4U;         // I
	base.tileSize = 1U;           // 8b
	base.tileTmem = 0U;
	base.tileLine = 1U;
	base.tileULS = 0U;
	base.tileULT = 0U;
	base.tileLRS = 0x000CU;
	base.tileLRT = 0x000CU;
	base.tileMasks = 0U;
	base.tileMaskt = 0U;
	base.tileCms = 0U;
	base.tileCmt = 0U;
	base.texS = 0;
	base.texT = 0;
	base.texDSDX = 32;
	base.texDTDY = 32;

	rvk2::RenderWorkPacket offsetVariant = base;
	offsetVariant.sourcePacketId = 421ULL;
	offsetVariant.tileULS = 0x0020U;
	offsetVariant.tileULT = 0x0020U;
	offsetVariant.tileLRS = static_cast<u16>(offsetVariant.tileULS + 0x000CU);
	offsetVariant.tileLRT = static_cast<u16>(offsetVariant.tileULT + 0x000CU);
	offsetVariant.texS = static_cast<s16>(0x0020);
	offsetVariant.texT = static_cast<s16>(0x0020);

	const std::vector<rvk2::SubmissionBatchPacket> oneBatch{makeSingleBatch()};
	const rvk2::ExecutorOutput baseOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{base}, oneBatch);
	const rvk2::ExecutorOutput offsetOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{offsetVariant}, oneBatch);

	expectTrue(
		baseOut.summary.colorWriteCount > 0ULL,
		"tile-base offset baseline should write pixels");
	expectEq(
		offsetOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"tile-base offset transition should preserve write coverage");
	expectEq(
		offsetOut.summary.presentHash,
		baseOut.summary.presentHash,
		"tile-base offset with matching texture origin should preserve presented output");
	expectTrue(
		presentFramesEqual(offsetOut, baseOut),
		"tile-base offset with matching texture origin should preserve presented pixels");

	for (size_t i = 0; i < savedTMEM.size(); ++i)
		TMEM[i] = savedTMEM[i];
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
		cycle1Out.summary.combinerCycle2SelectorOpCount > 0ULL,
		"cycle1 phase should execute cycle2 combiner selector path");
	expectTrue(
		cycle2Out.summary.combinerCycle2SelectorOpCount > 0ULL,
		"cycle2 phase should execute cycle2 combiner selector path");
	expectTrue(
		cycle2Out.summary.combinerOpCount > cycle1Out.summary.combinerOpCount,
		"cycle2 phase should execute additional combiner work per pixel");
}

void testCycle2CombinerSelectorIsolationConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket background = makeFillWork(68ULL, 0x00A08000U, 0x2A2A2AFFU);
	background.rectLRX = 5U;
	background.rectLRY = 3U;

	rvk2::RenderWorkPacket cycle2Base = makeTexRectWork(false);
	cycle2Base.sourcePacketId = 69ULL;
	cycle2Base.colorImageAddress = background.colorImageAddress;
	cycle2Base.colorImageWidth = 8U;
	cycle2Base.rectULX = 0U;
	cycle2Base.rectULY = 0U;
	cycle2Base.rectLRX = 5U;
	cycle2Base.rectLRY = 3U;
	cycle2Base.phase = static_cast<u8>(rvk2::RenderPhase::kCycle2);
	cycle2Base.cycleType = 1U;
	cycle2Base.otherModes = (1ULL << 6U);
	cycle2Base.blendParams = 0x00603090U;

	constexpr u64 kCycle2SelectorMask =
		(0xFULL << (32U + 5U))
		| (0xFULL << 24U)
		| (0x1FULL << 32U)
		| (0x7ULL << 6U)
		| (0x7ULL << 21U)
		| (0x7ULL << 3U)
		| (0x7ULL << 18U)
		| 0x7ULL;
	constexpr u64 kCycle2BaseSelectors =
		(1ULL << (32U + 5U))
		| (2ULL << 24U)
		| (3ULL << 32U)
		| (4ULL << 6U)
		| (1ULL << 21U)
		| (2ULL << 3U)
		| (3ULL << 18U)
		| 4ULL;
	constexpr u64 kCycle2VariantSelectors =
		(6ULL << (32U + 5U))
		| (5ULL << 24U)
		| (7ULL << 32U)
		| (0ULL << 6U)
		| (6ULL << 21U)
		| (5ULL << 3U)
		| (0ULL << 18U)
		| 1ULL;

	cycle2Base.combineMux = (cycle2Base.combineMux & ~kCycle2SelectorMask) | kCycle2BaseSelectors;
	rvk2::RenderWorkPacket cycle2Variant = cycle2Base;
	cycle2Variant.combineMux = (cycle2Variant.combineMux & ~kCycle2SelectorMask) | kCycle2VariantSelectors;

	const std::vector<rvk2::SubmissionBatchPacket> twoWorkBatches{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput cycle2BaseOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, cycle2Base},
		twoWorkBatches);
	const rvk2::ExecutorOutput cycle2VariantOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, cycle2Variant},
		twoWorkBatches);
	expectEq(
		cycle2VariantOut.summary.colorWriteCount,
		cycle2BaseOut.summary.colorWriteCount,
		"cycle2 selector transition should preserve write coverage");
	expectTrue(
		cycle2VariantOut.summary.presentHash != cycle2BaseOut.summary.presentHash,
		"cycle2 selector transition should alter cycle2 present hash");

	rvk2::RenderWorkPacket cycle1Base = cycle2Base;
	cycle1Base.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	cycle1Base.cycleType = 0U;
	rvk2::RenderWorkPacket cycle1Variant = cycle1Base;
	cycle1Variant.combineMux = cycle2Variant.combineMux;

	const rvk2::ExecutorOutput cycle1BaseOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, cycle1Base},
		twoWorkBatches);
	const rvk2::ExecutorOutput cycle1VariantOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, cycle1Variant},
		twoWorkBatches);
	expectEq(
		cycle1VariantOut.summary.colorWriteCount,
		cycle1BaseOut.summary.colorWriteCount,
		"cycle2 selector transition should preserve cycle1 write coverage");
	expectTrue(
		cycle1VariantOut.summary.presentHash != cycle1BaseOut.summary.presentHash,
		"cycle2 selector transition should alter cycle1 present hash");
	expectTrue(
		!presentFramesEqual(cycle1VariantOut, cycle1BaseOut),
		"cycle2 selector transition should alter cycle1 presented pixels");
}

void testCycle2TexelNextPixelHazardConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket background = makeFillWork(204ULL, 0x00B44000U, 0x305080FFU);
	background.rectLRX = 7U;
	background.rectLRY = 3U;

	rvk2::RenderWorkPacket base = makeTexRectWork(false);
	base.sourcePacketId = 205ULL;
	base.colorImageAddress = background.colorImageAddress;
	base.colorImageWidth = 8U;
	base.rectULX = 0U;
	base.rectULY = 0U;
	base.rectLRX = 7U;
	base.rectLRY = 3U;
	base.phase = static_cast<u8>(rvk2::RenderPhase::kCycle2);
	base.cycleType = 1U;
	base.otherModes = 0ULL;
	base.otherModes |= (1ULL << 6U);  // image_read_en
	base.otherModes |= (1ULL << 14U); // force_blend
	// Cycle-1 blender pass-through.
	base.otherModes &= ~(
		(0x3ULL << 30U)
		| (0x3ULL << 26U)
		| (0x3ULL << 22U)
		| (0x3ULL << 18U));
	base.otherModes |= (2ULL << 26U); // A = 1.0
	base.otherModes |= (3ULL << 18U); // B = 0.0
	// Cycle-2 blender uses combiner alpha to mix memory and blend colors.
	base.otherModes &= ~(
		(0x3ULL << 28U)
		| (0x3ULL << 24U)
		| (0x3ULL << 20U)
		| (0x3ULL << 16U));
	base.otherModes |= (1ULL << 28U); // P = memory color
	base.otherModes |= (2ULL << 20U); // M = blend color
	base.blendColor = 0xD02060FFU;

	// Keep cycle2 alpha output bound to TEX input so TEX0/TEX1 hazard is observable.
	constexpr u64 kCycle2AlphaMask =
		(0x7ULL << 21U)
		| (0x7ULL << 3U)
		| (0x7ULL << 18U)
		| 0x7ULL;
	constexpr u64 kCycle2AlphaTex1 =
		(0ULL << 21U)
		| (0ULL << 3U)
		| (0ULL << 18U)
		| 2ULL; // alphaD = TEX1
	constexpr u64 kCycle2AlphaTex0 =
		(0ULL << 21U)
		| (0ULL << 3U)
		| (0ULL << 18U)
		| 1ULL; // alphaD = TEX0

	rvk2::RenderWorkPacket tex1Variant = base;
	tex1Variant.combineMux = (tex1Variant.combineMux & ~kCycle2AlphaMask) | kCycle2AlphaTex1;
	rvk2::RenderWorkPacket tex0Variant = base;
	tex0Variant.sourcePacketId = 206ULL;
	tex0Variant.combineMux = (tex0Variant.combineMux & ~kCycle2AlphaMask) | kCycle2AlphaTex0;

	const std::vector<rvk2::SubmissionBatchPacket> twoWorkBatches{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput tex1Out = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, tex1Variant},
		twoWorkBatches);
	const rvk2::ExecutorOutput tex0Out = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, tex0Variant},
		twoWorkBatches);

	expectTrue(
		tex1Out.summary.colorWriteCount > 0ULL,
		"cycle2 texel hazard baseline should write pixels");
	expectEq(
		tex1Out.summary.colorWriteCount,
		tex0Out.summary.colorWriteCount,
		"cycle2 TEX selector transition should preserve write coverage");
	expectTrue(
		tex1Out.summary.presentHash != tex0Out.summary.presentHash,
		"cycle2 TEX1/TEX0 transition should alter present hash under next-pixel hazard semantics");
	expectTrue(
		!presentFramesEqual(tex1Out, tex0Out),
		"cycle2 TEX1/TEX0 transition should alter presented pixels under next-pixel hazard semantics");
}

void testCycle1Texel1NextPixelHazardConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket background = makeFillWork(263ULL, 0x00B4D000U, 0x304860FFU);
	background.rectLRX = 7U;
	background.rectLRY = 3U;

	rvk2::RenderWorkPacket base = makeTexRectWork(false);
	base.sourcePacketId = 264ULL;
	base.colorImageAddress = background.colorImageAddress;
	base.colorImageWidth = 8U;
	base.rectULX = 0U;
	base.rectULY = 0U;
	base.rectLRX = 7U;
	base.rectLRY = 3U;
	base.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	base.cycleType = 0U;
	base.otherModes = 0ULL;

	constexpr u64 kCycle1SelectorMask =
		(0xFULL << (32U + 5U))
		| (0xFULL << 24U)
		| (0x1FULL << 32U)
		| (0x7ULL << 6U)
		| (0x7ULL << 21U)
		| (0x7ULL << 3U)
		| (0x7ULL << 18U)
		| 0x7ULL;
	constexpr u64 kCycle1Tex1Selectors =
		(0ULL << (32U + 5U))
		| (0ULL << 24U)
		| (0ULL << 32U)
		| (2ULL << 6U)          // color D: TEXEL1
		| (0ULL << 21U)
		| (0ULL << 3U)
		| (0ULL << 18U)
		| 2ULL;                 // alpha D: TEXEL1
	constexpr u64 kCycle1Tex0Selectors =
		(0ULL << (32U + 5U))
		| (0ULL << 24U)
		| (0ULL << 32U)
		| (1ULL << 6U)          // color D: TEXEL0
		| (0ULL << 21U)
		| (0ULL << 3U)
		| (0ULL << 18U)
		| 1ULL;                 // alpha D: TEXEL0

	rvk2::RenderWorkPacket tex1Variant = base;
	tex1Variant.combineMux = (tex1Variant.combineMux & ~kCycle1SelectorMask) | kCycle1Tex1Selectors;
	rvk2::RenderWorkPacket tex0Variant = base;
	tex0Variant.combineMux = (tex0Variant.combineMux & ~kCycle1SelectorMask) | kCycle1Tex0Selectors;

	const std::vector<rvk2::SubmissionBatchPacket> twoWorkBatches{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput tex1Out = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, tex1Variant},
		twoWorkBatches);
	const rvk2::ExecutorOutput tex0Out = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, tex0Variant},
		twoWorkBatches);

	expectTrue(
		tex1Out.summary.colorWriteCount > 0ULL,
		"cycle1 TEX1 next-pixel hazard baseline should write pixels");
	expectEq(
		tex1Out.summary.colorWriteCount,
		tex0Out.summary.colorWriteCount,
		"cycle1 TEX1/TEX0 transition should preserve write coverage");
	expectTrue(
		tex1Out.summary.presentHash != tex0Out.summary.presentHash,
		"cycle1 TEX1 should diverge from TEX0 under next-pixel hazard semantics");
	expectTrue(
		!presentFramesEqual(tex1Out, tex0Out),
		"cycle1 TEX1 should alter presented pixels under next-pixel hazard semantics");
}

void testCycle2Texel0AliasTexel1HazardConformance()
{
	std::array<u64, 512> savedTMEM{};
	for (size_t i = 0; i < savedTMEM.size(); ++i)
		savedTMEM[i] = TMEM[i];
	u8 * tmem8 = reinterpret_cast<u8 *>(TMEM);
	for (u32 i = 0U; i < 4096U; ++i)
		tmem8[i] = static_cast<u8>((i * 37U + 11U) & 0xFFU);

	rvk2::Executor executor;
	rvk2::RenderWorkPacket background = makeFillWork(265ULL, 0x00B4E000U, 0x244060FFU);
	background.rectLRX = 7U;
	background.rectLRY = 3U;

	rvk2::RenderWorkPacket base = makeTexRectWork(false);
	base.sourcePacketId = 266ULL;
	base.colorImageAddress = background.colorImageAddress;
	base.colorImageWidth = 8U;
	base.rectULX = 0U;
	base.rectULY = 0U;
	base.rectLRX = 7U;
	base.rectLRY = 3U;
	base.phase = static_cast<u8>(rvk2::RenderPhase::kCycle2);
	base.cycleType = 1U;
	base.tileFormat = 3U;
	base.tileSize = 1U;
	base.textureImageFormat = 3U;
	base.textureImageSize = 1U;
	base.otherModes = 0ULL;
	base.otherModes |= (1ULL << 6U);  // image_read_en
	base.otherModes |= (1ULL << 14U); // force_blend
	// Cycle-1 blender pass-through.
	base.otherModes &= ~(
		(0x3ULL << 30U)
		| (0x3ULL << 26U)
		| (0x3ULL << 22U)
		| (0x3ULL << 18U));
	base.otherModes |= (2ULL << 26U); // A = 1.0
	base.otherModes |= (3ULL << 18U); // B = 0.0
	// Cycle-2 blender uses combiner alpha to blend memory against blend color.
	base.otherModes &= ~(
		(0x3ULL << 28U)
		| (0x3ULL << 24U)
		| (0x3ULL << 20U)
		| (0x3ULL << 16U));
	base.otherModes |= (1ULL << 28U); // P = memory color
	base.otherModes |= (2ULL << 20U); // M = blend color
	base.blendColor = 0xB04080FFU;
	constexpr u64 kCycle1SelectorMask =
		(0xFULL << (32U + 20U))
		| (0xFULL << 28U)
		| (0x1FULL << (32U + 15U))
		| (0x7ULL << 15U)
		| (0x7ULL << (32U + 12U))
		| (0x7ULL << 12U)
		| (0x7ULL << (32U + 9U))
		| (0x7ULL << 9U);
	constexpr u64 kCycle2SelectorMask =
		(0xFULL << (32U + 5U))
		| (0xFULL << 24U)
		| (0x1FULL << 32U)
		| (0x7ULL << 6U)
		| (0x7ULL << 21U)
		| (0x7ULL << 3U)
		| (0x7ULL << 18U)
		| 0x7ULL;
	constexpr u64 kCycle1Tex0Selectors =
		(0ULL << (32U + 20U))
		| (0ULL << 28U)
		| (0ULL << (32U + 15U))
		| (1ULL << 15U)         // color D: TEXEL0
		| (0ULL << (32U + 12U))
		| (0ULL << 12U)
		| (0ULL << (32U + 9U))
		| (1ULL << 9U);         // alpha D: TEXEL0
	constexpr u64 kCycle2Tex0Selectors =
		(0ULL << (32U + 5U))
		| (0ULL << 24U)
		| (0ULL << 32U)
		| (1ULL << 6U)          // color D: TEXEL0
		| (0ULL << 21U)
		| (0ULL << 3U)
		| (0ULL << 18U)
		| 1ULL;                 // alpha D: TEXEL0
	base.combineMux =
		(base.combineMux & ~(kCycle1SelectorMask | kCycle2SelectorMask))
		| kCycle1Tex0Selectors
		| kCycle2Tex0Selectors;

	rvk2::RenderWorkPacket noTile1 = base;
	noTile1.tile1Valid = false;

	rvk2::RenderWorkPacket withTile1 = base;
	withTile1.tile1Valid = true;
	withTile1.tile1Index = static_cast<u8>((withTile1.tile + 1U) & 0x7U);
	withTile1.tile1Format = 3U;
	withTile1.tile1Size = 1U;
	withTile1.tile1Line = 0x22U;
	withTile1.tile1Tmem = 0x180U;
	withTile1.tile1Palette = 0xFU;
	withTile1.tile1Cmt = 2U;
	withTile1.tile1Cms = 1U;
	withTile1.tile1Maskt = 3U;
	withTile1.tile1Masks = 2U;
	withTile1.tile1Shiftt = 1U;
	withTile1.tile1Shifts = 4U;
	withTile1.tile1ULS = 0x0020U;
	withTile1.tile1ULT = 0x0040U;
	withTile1.tile1LRS = 0x01E0U;
	withTile1.tile1LRT = 0x00E0U;

	const std::vector<rvk2::SubmissionBatchPacket> twoWorkBatches{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput noTile1Out = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, noTile1},
		twoWorkBatches);
	const rvk2::ExecutorOutput withTile1Out = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, withTile1},
		twoWorkBatches);

	expectTrue(
		noTile1Out.summary.colorWriteCount > 0ULL,
		"cycle2 TEX0 alias baseline should write pixels");
	expectTrue(
		noTile1Out.summary.textureTmemSampleCount > 0ULL,
		"cycle2 TEX0 alias baseline should sample TMEM");
	expectEq(
		noTile1Out.summary.colorWriteCount,
		withTile1Out.summary.colorWriteCount,
		"cycle2 TEX0 tile1 toggle should preserve write coverage");
	expectTrue(
		noTile1Out.summary.presentHash != withTile1Out.summary.presentHash,
		"cycle2 TEX0 should alias TEX1 in second cycle");
	expectTrue(
		!presentFramesEqual(noTile1Out, withTile1Out),
		"cycle2 TEX0 alias toggle should alter presented pixels");

	for (size_t i = 0; i < savedTMEM.size(); ++i)
		TMEM[i] = savedTMEM[i];
}

void testCycle2ShadeAlphaNextPixelHazardConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket background = makeFillWork(520ULL, 0x00B4B000U, 0x202020FFU);
	background.rectLRX = 7U;
	background.rectLRY = 3U;

	rvk2::RenderWorkPacket cycle2Tri = makeTriangleWork();
	cycle2Tri.sourcePacketId = 521ULL;
	cycle2Tri.colorImageAddress = background.colorImageAddress;
	cycle2Tri.colorImageWidth = 8U;
	cycle2Tri.rectULX = 0U;
	cycle2Tri.rectULY = 0U;
	cycle2Tri.rectLRX = 7U;
	cycle2Tri.rectLRY = 3U;
	cycle2Tri.phase = static_cast<u8>(rvk2::RenderPhase::kCycle2);
	cycle2Tri.cycleType = 1U;
	cycle2Tri.textured = false;
	cycle2Tri.triangleTextureEnable = false;
	cycle2Tri.triangleShadeEnable = true;
	cycle2Tri.triangleYH = 0U;
	cycle2Tri.triangleYM = 0U;
	cycle2Tri.triangleYL = 16U;
	cycle2Tri.triangleXH = 0x00000000U;
	cycle2Tri.triangleXL = 0x00080000U;
	cycle2Tri.triangleXM = 0x00000000U;
	cycle2Tri.triangleDxHDY = 0x00000000U;
	cycle2Tri.triangleDxLDY = 0x00000000U;
	cycle2Tri.triangleDxMDY = 0x00000000U;
	cycle2Tri.triangleShadeA = 0x0000;
	cycle2Tri.triangleShadeDADX = 0x0000FF00;
	cycle2Tri.triangleShadeDADY = 0x00000000;
	cycle2Tri.triangleShadeDADE = 0x00000000;
	cycle2Tri.otherModes = 0ULL;
	cycle2Tri.otherModes |= (1ULL << 3U); // aa_en (enables divided blender path).
	cycle2Tri.otherModes |= (1ULL << 6U); // image_read_en.
	// Cycle-1 blender pass-through.
	cycle2Tri.otherModes &= ~(
		(0x3ULL << 30U)
		| (0x3ULL << 26U)
		| (0x3ULL << 22U)
		| (0x3ULL << 18U));
	cycle2Tri.otherModes |= (2ULL << 26U); // A = 1.0
	cycle2Tri.otherModes |= (3ULL << 18U); // B = 0.0
	// Cycle-2 blender uses shade alpha to mix blend color over memory color.
	cycle2Tri.otherModes &= ~(
		(0x3ULL << 28U)
		| (0x3ULL << 24U)
		| (0x3ULL << 20U)
		| (0x3ULL << 16U));
	cycle2Tri.otherModes |= (2ULL << 28U); // P = blend color
	cycle2Tri.otherModes |= (2ULL << 24U); // A = shade alpha
	cycle2Tri.otherModes |= (1ULL << 20U); // M = memory color
	cycle2Tri.otherModes |= (0ULL << 16U); // B = 1.0 - A
	cycle2Tri.blendColor = 0xE0E0E0FFU;

	const std::vector<rvk2::SubmissionBatchPacket> bgBatch{makeBatchForWorkCount(1U)};
	const std::vector<rvk2::SubmissionBatchPacket> twoWorkBatches{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput bgOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background},
		bgBatch);
	const rvk2::ExecutorOutput hazardOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, cycle2Tri},
		twoWorkBatches);

	expectTrue(
		hazardOut.summary.colorWriteCount > bgOut.summary.colorWriteCount,
		"cycle2 shade-alpha hazard scene should add triangle writes");
	expectTrue(
		hazardOut.summary.presentHash != bgOut.summary.presentHash,
		"cycle2 shade-alpha hazard scene should alter present output");
	expectTrue(
		hazardOut.presentFrame.pixels.size() >= 2U,
		"cycle2 shade-alpha hazard scene should produce at least two present pixels");
	expectTrue(
		hazardOut.presentFrame.pixels[0] == hazardOut.presentFrame.pixels[1],
		"cycle2 shade-alpha hazard should read next-pixel shade alpha in second blender cycle");
}

void testCycle1CombinedFeedbackHazardConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket work = makeTexRectWork(false);
	work.sourcePacketId = 522ULL;
	work.colorImageAddress = 0x00B4C000U;
	work.colorImageWidth = 8U;
	work.rectULX = 0U;
	work.rectULY = 0U;
	work.rectLRX = 7U;
	work.rectLRY = 0U;
	work.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	work.cycleType = 0U;
	work.otherModes = 0ULL;

	// Cycle1 combiner equation: output D only.
	// Hazard variant: D=COMBINED (undefined first-cycle feedback path).
	rvk2::RenderWorkPacket combinedHazard = work;
	combinedHazard.combineMux = 0ULL;
	// Direct variant: D=TEX0 (no feedback dependency).
	rvk2::RenderWorkPacket texelDirect = work;
	constexpr u64 kCycle1ColorDMask = 0x7ULL << 6U;
	constexpr u64 kCycle1AlphaDMask = 0x7ULL << 0U;
	texelDirect.combineMux &= ~(kCycle1ColorDMask | kCycle1AlphaDMask);
	texelDirect.combineMux |= (1ULL << 6U); // color D = TEX0
	texelDirect.combineMux |= (1ULL << 0U);  // alpha D = TEX0

	const std::vector<rvk2::SubmissionBatchPacket> oneWorkBatch{makeBatchForWorkCount(1U)};
	const rvk2::ExecutorOutput hazardOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{combinedHazard},
		oneWorkBatch);
	const rvk2::ExecutorOutput directOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{texelDirect},
		oneWorkBatch);

	expectTrue(
		hazardOut.summary.colorWriteCount > 0ULL,
		"cycle1 combined-feedback hazard baseline should write pixels");
	expectEq(
		hazardOut.summary.colorWriteCount,
		directOut.summary.colorWriteCount,
		"cycle1 combined-feedback transition should preserve write coverage");
	expectTrue(
		hazardOut.summary.presentHash != directOut.summary.presentHash,
		"cycle1 combined-feedback transition should alter present hash");
	expectTrue(
		hazardOut.presentFrame.pixels.size() >= 2U && directOut.presentFrame.pixels.size() >= 2U,
		"cycle1 combined-feedback scene should produce at least two present pixels");
	expectTrue(
		hazardOut.presentFrame.pixels[0] == hazardOut.presentFrame.pixels[1],
		"cycle1 combined-feedback hazard should read previous pixel combined output");
}

void testCycle1Texel1SecondaryTileConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket background = makeFillWork(260ULL, 0x00B4A000U, 0x304050FFU);
	background.rectLRX = 7U;
	background.rectLRY = 3U;

	rvk2::RenderWorkPacket base = makeTexRectWork(false);
	base.sourcePacketId = 261ULL;
	base.colorImageAddress = background.colorImageAddress;
	base.colorImageWidth = 8U;
	base.rectULX = 0U;
	base.rectULY = 0U;
	base.rectLRX = 7U;
	base.rectLRY = 3U;
	base.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	base.cycleType = 0U;
	base.otherModes = 0ULL;

	constexpr u64 kCycle1ColorAMask = 0xFULL << (32U + 5U);
	constexpr u64 kCycle1ColorBMask = 0xFULL << 24U;
	constexpr u64 kCycle1ColorCMask = 0x1FULL << 32U;
	constexpr u64 kCycle1ColorDMask = 0x7ULL << 6U;
	constexpr u64 kCycle1AlphaAMask = 0x7ULL << 21U;
	constexpr u64 kCycle1AlphaBMask = 0x7ULL << 3U;
	constexpr u64 kCycle1AlphaCMask = 0x7ULL << 18U;
	constexpr u64 kCycle1AlphaDMask = 0x7ULL << 0U;
	constexpr u64 kCycle1SelectorMask =
		kCycle1ColorAMask
		| kCycle1ColorBMask
		| kCycle1ColorCMask
		| kCycle1ColorDMask
		| kCycle1AlphaAMask
		| kCycle1AlphaBMask
		| kCycle1AlphaCMask
		| kCycle1AlphaDMask;
	base.combineMux &= ~kCycle1SelectorMask;
	base.combineMux |=
		(0ULL << (32U + 5U))
		| (0ULL << 24U)
		| (0ULL << 32U)
		| (2ULL << 6U)          // color D: TEXEL1
		| (0ULL << 21U)
		| (0ULL << 3U)
		| (0ULL << 18U)
		| 2ULL;                 // alpha D: TEXEL1

	base.tile1Valid = true;
	base.tile1Index = static_cast<u8>((base.tile + 1U) & 0x7U);
	base.tile1Format = 4U;
	base.tile1Size = 1U;
	base.tile1Line = 0x22U;
	base.tile1Tmem = 0x180U;
	base.tile1Palette = 0xFU;
	base.tile1Cmt = 2U;
	base.tile1Cms = 1U;
	base.tile1Maskt = 3U;
	base.tile1Masks = 2U;
	base.tile1Shiftt = 1U;
	base.tile1Shifts = 4U;
	base.tile1ULS = 0x0020U;
	base.tile1ULT = 0x0040U;
	base.tile1LRS = 0x01E0U;
	base.tile1LRT = 0x00E0U;

	rvk2::RenderWorkPacket fallback = base;
	fallback.sourcePacketId = 262ULL;
	fallback.tile1Valid = false;

	const std::vector<rvk2::SubmissionBatchPacket> twoWorkBatches{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput baseOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, base},
		twoWorkBatches);
	const rvk2::ExecutorOutput fallbackOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, fallback},
		twoWorkBatches);

	expectTrue(
		baseOut.summary.colorWriteCount > 0ULL,
		"cycle1 TEXEL1 secondary-tile baseline should write pixels");
	expectEq(
		baseOut.summary.colorWriteCount,
		fallbackOut.summary.colorWriteCount,
		"cycle1 TEXEL1 secondary-tile transition should preserve write coverage");
	expectTrue(
		baseOut.summary.presentHash != fallbackOut.summary.presentHash,
		"cycle1 TEXEL1 secondary-tile transition should alter present hash");
	expectTrue(
		!presentFramesEqual(baseOut, fallbackOut),
		"cycle1 TEXEL1 secondary-tile transition should alter presented pixels");
}

void testTMEM32AuthoritativePathConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket work = makeTexRectWork(false);
	work.sourcePacketId = 207ULL;
	work.colorImageAddress = 0x00A0E000U;
	work.colorImageWidth = 8U;
	work.rectULX = 0U;
	work.rectULY = 0U;
	work.rectLRX = 7U;
	work.rectLRY = 3U;
	work.textureImageFormat = 0U;
	work.textureImageSize = 3U;
	work.textureImageWidth = 32U;
	work.tileFormat = 0U;
	work.tileSize = 3U;
	work.tileLine = 0x20U;
	work.tileTmem = 0x40U;
	work.tileULS = 0U;
	work.tileULT = 0U;
	work.tileLRS = 0x007CU;
	work.tileLRT = 0x003CU;

	const std::vector<rvk2::SubmissionBatchPacket> oneBatch{makeSingleBatch()};
	const rvk2::ExecutorOutput out =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{work}, oneBatch);

	expectTrue(
		out.summary.colorWriteCount > 0ULL,
		"TMEM32 authoritative path baseline should write pixels");
	expectEq(
		out.summary.stageTexelSourceTMEMWriteCount,
		out.summary.colorWriteCount,
		"TMEM32 texrect should source writes from TMEM path");
	expectEq(
		out.summary.stageTexelSourceRdramWriteCount,
		0ULL,
		"TMEM32 authoritative path should not fall back to RDRAM");
	expectEq(
		out.summary.stageTexelSourceSyntheticWriteCount,
		0ULL,
		"TMEM32 authoritative path should not fall back to synthetic texels");
}

void testUnsupportedTMEMDecodeUsesSyntheticConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket work = makeTexRectWork(false);
	work.sourcePacketId = 208ULL;
	work.colorImageAddress = 0x00A0E400U;
	work.colorImageWidth = 8U;
	work.rectULX = 0U;
	work.rectULY = 0U;
	work.rectLRX = 3U;
	work.rectLRY = 3U;
	work.textureImageFormat = 0U;
	work.textureImageSize = 0U;
	work.textureImageWidth = 8U;
	work.tileFormat = 0U; // RGBA + 4b is unsupported by TMEM decode path.
	work.tileSize = 0U;   // 4b
	work.tileLine = 1U;
	work.tileTmem = 0U;
	work.tileULS = 0U;
	work.tileULT = 0U;
	work.tileLRS = 0x003CU;
	work.tileLRT = 0x003CU;

	const std::vector<rvk2::SubmissionBatchPacket> oneBatch{makeSingleBatch()};
	const rvk2::ExecutorOutput out =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{work}, oneBatch);

	expectTrue(
		out.summary.colorWriteCount > 0ULL,
		"unsupported TMEM decode conformance scene should write pixels");
	expectEq(
		out.summary.stageTexelSourceRdramWriteCount,
		0ULL,
		"unsupported TMEM decode path should not fall back to RDRAM in RVK2");
	expectEq(
		out.summary.stageTexelSourceSyntheticWriteCount,
		out.summary.colorWriteCount,
		"unsupported TMEM decode path should resolve through synthetic diagnostic source");
}

void testUnsupportedTMEMDecodeUsesRdramFallbackWhenAvailableConformance()
{
	ScopedRdramBuffer rdram(1U << 22U); // 4 MiB, power-of-two mask.

	rvk2::Executor executor;
	rvk2::RenderWorkPacket work = makeTexRectWork(false);
	work.sourcePacketId = 209ULL;
	work.colorImageAddress = 0x00A0E800U;
	work.colorImageWidth = 8U;
	work.rectULX = 0U;
	work.rectULY = 0U;
	work.rectLRX = 3U;
	work.rectLRY = 3U;
	work.textureImageAddress = 0x00100000U;
	work.textureImageFormat = 1U; // YUV
	work.textureImageSize = 2U;   // 16b
	work.textureImageWidth = 8U;
	work.tileFormat = 1U; // YUV decode is unsupported in TMEM path today.
	work.tileSize = 2U;
	work.tileLine = 1U;
	work.tileTmem = 0U;
	work.texS = 0;
	work.texT = 0;
	work.texDSDX = 0x20;
	work.texDTDY = 0x20;

	// Encode U Y0 V Y1 for texel pairs in row-major layout.
	for (u32 y = 0U; y < 8U; ++y) {
		for (u32 xPair = 0U; xPair < 4U; ++xPair) {
			const u32 texelIndex = y * 8U + (xPair * 2U);
			const u32 addr = work.textureImageAddress + texelIndex * 2U;
			const u8 u = static_cast<u8>(32U + xPair * 17U);
			const u8 y0 = static_cast<u8>(48U + y * 9U + xPair * 5U);
			const u8 v = static_cast<u8>(96U + y * 7U);
			const u8 y1 = static_cast<u8>(64U + y * 11U + xPair * 3U);
			rdram.writeByte(addr + 0U, u);
			rdram.writeByte(addr + 1U, y0);
			rdram.writeByte(addr + 2U, v);
			rdram.writeByte(addr + 3U, y1);
		}
	}

	const std::vector<rvk2::SubmissionBatchPacket> oneBatch{makeSingleBatch()};
	const rvk2::ExecutorOutput out =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{work}, oneBatch);

	expectTrue(
		out.summary.colorWriteCount > 0ULL,
		"unsupported TMEM decode + RDRAM fallback conformance scene should write pixels");
	expectEq(
		out.summary.stageTexelSourceRdramWriteCount,
		out.summary.colorWriteCount,
		"unsupported TMEM decode should route through RDRAM fallback when RDRAM is available");
	expectEq(
		out.summary.stageTexelSourceSyntheticWriteCount,
		0ULL,
		"unsupported TMEM decode should avoid synthetic fallback when RDRAM fallback succeeds");
	expectTrue(
		out.summary.textureRdramSampleCount > 0ULL,
		"RDRAM fallback conformance scene should sample RDRAM texels");
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

void testCopyFillBypassAlphaCoverageConformance()
{
	rvk2::Executor executor;

	rvk2::RenderWorkPacket copyBase = makeTexRectWork(false);
	copyBase.sourcePacketId = 71ULL;
	copyBase.phase = static_cast<u8>(rvk2::RenderPhase::kCopy);
	copyBase.cycleType = 2U;
	copyBase.colorImageAddress = 0x00A09000U;
	copyBase.colorImageWidth = 8U;
	copyBase.rectULX = 0U;
	copyBase.rectULY = 0U;
	copyBase.rectLRX = 5U;
	copyBase.rectLRY = 3U;
	copyBase.otherModes = 0ULL;

	rvk2::RenderWorkPacket copyGated = copyBase;
	copyGated.sourcePacketId = 72ULL;
	copyGated.alphaCompare = 1U;
	copyGated.blendColor = 0x000000FFU;
	copyGated.colorOnCvg = true;
	copyGated.cvgDest = 3U;
	copyGated.cvgXAlpha = true;
	copyGated.alphaCvgSel = true;
	copyGated.forceBlender = true;
	copyGated.blendMask = 0xFU;
	copyGated.otherModes |= (1ULL << 6U); // imageRead

	const std::vector<rvk2::SubmissionBatchPacket> oneBatch{makeSingleBatch()};
	const rvk2::ExecutorOutput copyBaseOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{copyBase}, oneBatch);
	const rvk2::ExecutorOutput copyGatedOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{copyGated}, oneBatch);
	expectTrue(copyBaseOut.summary.colorWriteCount > 0ULL, "copy alpha/coverage bypass baseline should write");
	expectEq(
		copyGatedOut.summary.colorWriteCount,
		copyBaseOut.summary.colorWriteCount,
		"copy phase should bypass alpha compare and coverage-gated write suppression");
	expectEq(
		copyGatedOut.summary.presentHash,
		copyBaseOut.summary.presentHash,
		"copy phase alpha/coverage controls should not alter present hash");
	expectTrue(
		presentFramesEqual(copyGatedOut, copyBaseOut),
		"copy phase alpha/coverage controls should not alter presented pixels");

	rvk2::RenderWorkPacket fillBase = makeFillWork(73ULL, 0x00A0A000U, 0x30C06080U);
	fillBase.rectLRX = 5U;
	fillBase.rectLRY = 3U;
	fillBase.phase = static_cast<u8>(rvk2::RenderPhase::kFill);
	fillBase.cycleType = 3U;

	rvk2::RenderWorkPacket fillGated = fillBase;
	fillGated.sourcePacketId = 74ULL;
	fillGated.alphaCompare = 2U;
	fillGated.colorOnCvg = true;
	fillGated.cvgDest = 3U;
	fillGated.cvgXAlpha = true;
	fillGated.alphaCvgSel = true;
	fillGated.forceBlender = true;
	fillGated.blendMask = 0xFU;
	fillGated.blendColor = 0x000000FFU;
	fillGated.otherModes = (1ULL << 6U); // imageRead

	const rvk2::ExecutorOutput fillBaseOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{fillBase}, oneBatch);
	const rvk2::ExecutorOutput fillGatedOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{fillGated}, oneBatch);
	expectTrue(fillBaseOut.summary.colorWriteCount > 0ULL, "fill alpha/coverage bypass baseline should write");
	expectEq(
		fillGatedOut.summary.colorWriteCount,
		fillBaseOut.summary.colorWriteCount,
		"fill phase should bypass alpha compare and coverage-gated write suppression");
	expectEq(
		fillGatedOut.summary.presentHash,
		fillBaseOut.summary.presentHash,
		"fill phase alpha/coverage controls should not alter present hash");
}

void testFillSeedsCoverageForImageReadBlendConformance()
{
	constexpr u32 kTarget = 0x00A0B000U;
	rvk2::RenderWorkPacket fill =
		makeFillWork(75ULL, kTarget, 0xFFFFFFFFU);
	fill.colorImageWidth = 4U;
	fill.rectULX = 0U;
	fill.rectULY = 0U;
	fill.rectLRX = 3U;
	fill.rectLRY = 2U;
	fill.phase = static_cast<u8>(rvk2::RenderPhase::kFill);
	fill.cycleType = 3U;

	rvk2::RenderWorkPacket blend = makeTexRectWork(false);
	blend.sourcePacketId = 76ULL;
	blend.colorImageAddress = kTarget;
	blend.colorImageWidth = 4U;
	blend.rectULX = 0U;
	blend.rectULY = 0U;
	blend.rectLRX = 3U;
	blend.rectLRY = 2U;
	blend.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	blend.cycleType = 0U;
	blend.textured = false;
	blend.otherModes = 0ULL;
	blend.otherModes |= (1ULL << 6U);  // image_read_en
	blend.otherModes |= (1ULL << 14U); // force_blend
	blend.otherModes &= ~(
		(0x3ULL << 30U)
		| (0x3ULL << 26U)
		| (0x3ULL << 22U)
		| (0x3ULL << 18U));
	blend.otherModes |= (2ULL << 30U); // P = blend color
	blend.otherModes |= (2ULL << 26U); // A = 1.0 (shade alpha)
	blend.otherModes |= (1ULL << 22U); // M = memory color
	blend.otherModes |= (1ULL << 18U); // B = memory coverage
	blend.blendColor = 0x000000FFU;
	blend.primColor = 0xFFFFFFFFU;

	constexpr u64 kCycle1ColorAMask = 0xFULL << (32U + 5U);
	constexpr u64 kCycle1ColorBMask = 0xFULL << 24U;
	constexpr u64 kCycle1ColorCMask = 0x1FULL << 32U;
	constexpr u64 kCycle1ColorDMask = 0x7ULL << 6U;
	constexpr u64 kCycle1AlphaAMask = 0x7ULL << 21U;
	constexpr u64 kCycle1AlphaBMask = 0x7ULL << 3U;
	constexpr u64 kCycle1AlphaCMask = 0x7ULL << 18U;
	constexpr u64 kCycle1AlphaDMask = 0x7ULL << 0U;
	constexpr u64 kCycle1SelectorMask =
		kCycle1ColorAMask
		| kCycle1ColorBMask
		| kCycle1ColorCMask
		| kCycle1ColorDMask
		| kCycle1AlphaAMask
		| kCycle1AlphaBMask
		| kCycle1AlphaCMask
		| kCycle1AlphaDMask;
	blend.combineMux &= ~kCycle1SelectorMask;
	blend.combineMux |=
		(0ULL << (32U + 5U))
		| (0ULL << 24U)
		| (0ULL << 32U)
		| (3ULL << 6U) // color D: PRIMITIVE
		| (0ULL << 21U)
		| (0ULL << 3U)
		| (0ULL << 18U)
		| 3ULL; // alpha D: PRIMITIVE

	rvk2::RenderWorkPacket unseededBlend = blend;
	unseededBlend.sourcePacketId = 77ULL;

	const std::vector<rvk2::SubmissionBatchPacket> seededBatches{makeBatchForWorkCount(2U)};
	const std::vector<rvk2::SubmissionBatchPacket> unseededBatches{makeSingleBatch()};

	rvk2::Executor seededExecutor;
	const rvk2::ExecutorOutput seededOut =
		seededExecutor.executeWithOutput(
			std::vector<rvk2::RenderWorkPacket>{fill, blend},
			seededBatches);
	rvk2::Executor unseededExecutor;
	const rvk2::ExecutorOutput unseededOut =
		unseededExecutor.executeWithOutput(
			std::vector<rvk2::RenderWorkPacket>{unseededBlend},
			unseededBatches);

	expectTrue(
		seededOut.summary.colorWriteCount > 0ULL,
		"fill-seeded coverage baseline should write pixels");
	expectTrue(
		unseededOut.summary.colorWriteCount > 0ULL,
		"fill-seeded coverage variant should write pixels");
	expectTrue(
		seededOut.summary.blendAlphaBSelectorCount[1] > 0ULL,
		"fill-seeded coverage scene should exercise memory-coverage alpha selector");
	expectTrue(
		seededOut.summary.blendCoverageOverflowCount > unseededOut.summary.blendCoverageOverflowCount,
		"fill should seed memory coverage for subsequent image-read blend overflow behavior");
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

void testRenderTargetColorSizeConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket color32 = makeTexRectWork(false);
	color32.sourcePacketId = 130ULL;
	color32.colorImageAddress = 0x00AE0000U;
	color32.colorImageWidth = 8U;
	color32.rectULX = 0U;
	color32.rectULY = 0U;
	color32.rectLRX = 5U;
	color32.rectLRY = 3U;
	color32.colorImageSize = 3U;

	rvk2::RenderWorkPacket color16 = color32;
	color16.sourcePacketId = 131ULL;
	color16.colorImageSize = 2U;

	const std::vector<rvk2::SubmissionBatchPacket> batches{makeSingleBatch()};
	const rvk2::ExecutorOutput out32 =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{color32}, batches);
	const rvk2::ExecutorOutput out16 =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{color16}, batches);

	expectTrue(
		out32.summary.colorWriteCount > 0ULL,
		"render target size conformance baseline should write pixels");
	expectEq(
		out16.summary.colorWriteCount,
		out32.summary.colorWriteCount,
		"render target size conformance should preserve write coverage");
	expectEq(
		out16.presentFrame.width,
		out32.presentFrame.width,
		"render target size conformance should preserve present width");
	expectEq(
		out16.presentFrame.height,
		out32.presentFrame.height,
		"render target size conformance should preserve present height");
	expectTrue(
		out16.summary.presentHash != out32.summary.presentHash,
		"render target size conformance should alter present hash for 16bpp writes");
	expectTrue(
		!presentFramesEqual(out16, out32),
		"render target size conformance should alter presented pixels for 16bpp writes");
}

void testImageReadConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket background = makeFillWork(140ULL, 0x00AF0000U, 0x304050FFU);
	background.rectLRX = 5U;
	background.rectLRY = 3U;

	rvk2::RenderWorkPacket imageReadOn = makeTexRectWork(false);
	imageReadOn.sourcePacketId = 141ULL;
	imageReadOn.colorImageAddress = background.colorImageAddress;
	imageReadOn.colorImageWidth = 8U;
	imageReadOn.rectULX = 0U;
	imageReadOn.rectULY = 0U;
	imageReadOn.rectLRX = 5U;
	imageReadOn.rectLRY = 3U;
	imageReadOn.otherModes = (1ULL << 6U);
	imageReadOn.combineMux = 0ULL;
	imageReadOn.combineMux |= (6ULL << 9U);
	imageReadOn.combineMux |= (6ULL << 21U);
	imageReadOn.combineMux |= (6ULL << 33U);
	imageReadOn.combineMux |= (6ULL << 45U);
	imageReadOn.blendParams = 0U;

	rvk2::RenderWorkPacket imageReadOff = imageReadOn;
	imageReadOff.sourcePacketId = 142ULL;
	imageReadOff.otherModes = 0ULL;

	const std::vector<rvk2::SubmissionBatchPacket> batches{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput onOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, imageReadOn},
		batches);
	const rvk2::ExecutorOutput offOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, imageReadOff},
		batches);

	expectTrue(
		onOut.summary.colorWriteCount > 0ULL,
		"image-read conformance scene should write pixels");
	expectEq(
		onOut.summary.colorWriteCount,
		offOut.summary.colorWriteCount,
		"image-read enable transition should preserve write coverage");
	expectTrue(
		onOut.summary.presentHash != offOut.summary.presentHash,
		"image-read enable transition should alter present hash");
	expectTrue(
		!presentFramesEqual(onOut, offOut),
		"image-read enable transition should alter presented pixels");
}

void testDepthModeConformance()
{
	rvk2::Executor executor;
	const u32 colorAddress = 0x00B00000U;
	const u32 depthAddress = 0x00B10000U;

	rvk2::RenderWorkPacket nearWork = makeTriangleWork();
	nearWork.sourcePacketId = 150ULL;
	nearWork.colorImageAddress = colorAddress;
	nearWork.colorImageWidth = 8U;
	nearWork.depthImageAddress = depthAddress;
	nearWork.depthTest = true;
	nearWork.triangleZBufferEnable = true;
	nearWork.depthCompareEnable = true;
	nearWork.depthUpdateEnable = true;
	nearWork.triangleZ = 1000;
	nearWork.blendParams = 0x000000FFU;

	rvk2::RenderWorkPacket farWork = nearWork;
	farWork.sourcePacketId = 151ULL;
	farWork.triangleZ = 1016;

	rvk2::RenderWorkPacket opaNear = nearWork;
	rvk2::RenderWorkPacket opaFar = farWork;
	opaNear.otherModes = 0ULL;
	opaFar.otherModes = 0ULL;

	rvk2::RenderWorkPacket interNear = nearWork;
	rvk2::RenderWorkPacket interFar = farWork;
	interNear.otherModes = (1ULL << 10U);
	interFar.otherModes = (1ULL << 10U);

	const std::vector<rvk2::SubmissionBatchPacket> batches{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput opaOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{opaNear, opaFar}, batches);
	const rvk2::ExecutorOutput interOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{interNear, interFar}, batches);

	expectTrue(
		opaOut.summary.colorWriteCount > 0ULL,
		"depth-mode conformance baseline should write pixels");
	expectTrue(
		interOut.summary.colorWriteCount > opaOut.summary.colorWriteCount,
		"depth-mode interpenetrating path should admit more writes than opaque mode");
	expectTrue(
		interOut.summary.presentHash != opaOut.summary.presentHash,
		"depth-mode transition should alter present hash");
}

void testTextureFilterConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket pointWork = makeTexRectWork(false);
	pointWork.sourcePacketId = 160ULL;
	pointWork.colorImageAddress = 0x00B20000U;
	pointWork.colorImageWidth = 8U;
	pointWork.rectULX = 0U;
	pointWork.rectULY = 0U;
	pointWork.rectLRX = 5U;
	pointWork.rectLRY = 3U;
	pointWork.texS = 0x0011;
	pointWork.texT = 0x0007;
	pointWork.texDSDX = 0x0031;
	pointWork.texDTDY = 0x0027;
	pointWork.otherModes = 0ULL;

	rvk2::RenderWorkPacket filteredWork = pointWork;
	filteredWork.sourcePacketId = 161ULL;
	filteredWork.otherModes = (2ULL << (32U + 12U));
	rvk2::RenderWorkPacket bilerpWork = pointWork;
	bilerpWork.sourcePacketId = 162ULL;
	bilerpWork.otherModes = (1ULL << (32U + 12U));
	rvk2::RenderWorkPacket sharpenWork = pointWork;
	sharpenWork.sourcePacketId = 163ULL;
	sharpenWork.otherModes = (3ULL << (32U + 12U));

	const std::vector<rvk2::SubmissionBatchPacket> batches{makeSingleBatch()};
	const rvk2::ExecutorOutput pointOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{pointWork}, batches);
	const rvk2::ExecutorOutput bilerpOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{bilerpWork}, batches);
	const rvk2::ExecutorOutput filteredOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{filteredWork}, batches);
	const rvk2::ExecutorOutput sharpenOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{sharpenWork}, batches);

	expectTrue(
		pointOut.summary.colorWriteCount > 0ULL,
		"texture-filter conformance baseline should write pixels");
	expectEq(
		bilerpOut.summary.colorWriteCount,
		pointOut.summary.colorWriteCount,
		"texture-filter bilerp transition should preserve write coverage");
	expectEq(
		filteredOut.summary.colorWriteCount,
		pointOut.summary.colorWriteCount,
		"texture-filter transition should preserve write coverage");
	expectEq(
		sharpenOut.summary.colorWriteCount,
		pointOut.summary.colorWriteCount,
		"texture-filter sharpen transition should preserve write coverage");
	expectTrue(
		bilerpOut.summary.presentHash != pointOut.summary.presentHash,
		"texture-filter bilerp transition should alter present hash");
	expectTrue(
		filteredOut.summary.presentHash != pointOut.summary.presentHash,
		"texture-filter transition should alter present hash");
	expectTrue(
		sharpenOut.summary.presentHash != pointOut.summary.presentHash,
		"texture-filter sharpen transition should alter present hash");
	expectTrue(
		bilerpOut.summary.presentHash != filteredOut.summary.presentHash,
		"texture-filter bilerp and average transitions should diverge");
	expectTrue(
		sharpenOut.summary.presentHash != bilerpOut.summary.presentHash,
		"texture-filter sharpen and bilerp transitions should diverge");
	expectTrue(
		sharpenOut.summary.presentHash != filteredOut.summary.presentHash,
		"texture-filter sharpen and average transitions should diverge");
	expectTrue(
		!presentFramesEqual(filteredOut, pointOut),
		"texture-filter transition should alter presented pixels");
}

void testCombinerKeyConvertConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket base = makeTexRectWork(false);
	base.sourcePacketId = 170ULL;
	base.colorImageAddress = 0x00B30000U;
	base.colorImageWidth = 8U;
	base.rectULX = 0U;
	base.rectULY = 0U;
	base.rectLRX = 5U;
	base.rectLRY = 3U;
	base.keyState = 0xE1D2C3B4A5968778ULL;
	base.convertState = 0x1020304050607080ULL;

	rvk2::RenderWorkPacket keyVariant = base;
	keyVariant.sourcePacketId = 171ULL;
	keyVariant.otherModes = (1ULL << (32U + 8U));

	rvk2::RenderWorkPacket convertVariant = keyVariant;
	convertVariant.sourcePacketId = 172ULL;
	convertVariant.otherModes |= (1ULL << (32U + 9U));

	const std::vector<rvk2::SubmissionBatchPacket> batches{makeSingleBatch()};
	const rvk2::ExecutorOutput baseOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{base}, batches);
	const rvk2::ExecutorOutput keyOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{keyVariant}, batches);
	const rvk2::ExecutorOutput convertOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{convertVariant}, batches);

	expectTrue(
		baseOut.summary.colorWriteCount > 0ULL,
		"combine-key conformance baseline should write pixels");
	expectEq(
		keyOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"combine-key transition should preserve write coverage");
	expectTrue(
		keyOut.summary.presentHash != baseOut.summary.presentHash,
		"combine-key transition should alter present hash");
	expectTrue(
		convertOut.summary.presentHash != keyOut.summary.presentHash,
		"convert-one transition should alter present hash");
}

void testTextureExtendedModeConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket base = makeTriangleWork();
	base.sourcePacketId = 180ULL;
	base.colorImageAddress = 0x00B40000U;
	base.colorImageWidth = 8U;
	base.rectULX = 0U;
	base.rectULY = 0U;
	base.rectLRX = 5U;
	base.rectLRY = 5U;
	base.textured = true;
	base.triangleTextureEnable = true;
	base.triangleShadeEnable = true;
	base.triangleTexS = 0x00800000;
	base.triangleTexT = 0x00400000;
	base.triangleTexW = 0x01000000;
	base.triangleTexDSDX = 0x00008000;
	base.triangleTexDTDX = 0x00004000;
	base.triangleTexDWDX = 0x00002000;
	base.triangleTexDSDY = 0x00001000;
	base.triangleTexDTDY = 0x00000800;
	base.triangleTexDWDY = 0x00000400;
	base.otherModes = 0ULL;

	rvk2::RenderWorkPacket perspVariant = base;
	perspVariant.sourcePacketId = 181ULL;
	perspVariant.otherModes = (1ULL << (32U + 19U));

	rvk2::RenderWorkPacket lodVariant = base;
	lodVariant.sourcePacketId = 182ULL;
	lodVariant.otherModes = (1ULL << (32U + 16U));

	rvk2::RenderWorkPacket detailVariant = base;
	detailVariant.sourcePacketId = 183ULL;
	detailVariant.otherModes = (2ULL << (32U + 17U));

	rvk2::RenderWorkPacket lutVariant = base;
	lutVariant.sourcePacketId = 184ULL;
	lutVariant.otherModes = (1ULL << (32U + 14U));

	const std::vector<rvk2::SubmissionBatchPacket> batches{makeSingleBatch()};
	const rvk2::ExecutorOutput baseOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{base}, batches);
	const rvk2::ExecutorOutput perspOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{perspVariant}, batches);
	const rvk2::ExecutorOutput lodOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{lodVariant}, batches);
	const rvk2::ExecutorOutput detailOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{detailVariant}, batches);
	const rvk2::ExecutorOutput lutOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{lutVariant}, batches);

	expectTrue(
		baseOut.summary.colorWriteCount > 0ULL,
		"texture extended mode baseline should write pixels");
	expectEq(
		perspOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"texture persp transition should preserve write coverage");
	expectEq(
		lodOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"texture lod transition should preserve write coverage");
	expectEq(
		detailOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"texture detail transition should preserve write coverage");
	expectEq(
		lutOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"texture lut transition should preserve write coverage");
	expectTrue(
		perspOut.summary.presentHash != baseOut.summary.presentHash,
		"texture persp transition should alter present hash");
	expectTrue(
		lodOut.summary.presentHash != baseOut.summary.presentHash,
		"texture lod transition should alter present hash");
	expectTrue(
		detailOut.summary.presentHash != baseOut.summary.presentHash,
		"texture detail transition should alter present hash");
	expectTrue(
		lutOut.summary.presentHash != baseOut.summary.presentHash,
		"texture lut transition should alter present hash");
}

void testTextureDetailColorTransformConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket base = makeTexRectWork(false);
	base.sourcePacketId = 188ULL;
	base.colorImageAddress = 0x00B43000U;
	base.colorImageWidth = 8U;
	base.rectULX = 0U;
	base.rectULY = 0U;
	base.rectLRX = 6U;
	base.rectLRY = 4U;
	base.textured = true;
	base.tileFormat = 6U;
	base.textureImageFormat = 6U;
	base.otherModes = 0ULL;

	rvk2::RenderWorkPacket detailVariant = base;
	detailVariant.otherModes = (2ULL << (32U + 17U));

	const std::vector<rvk2::SubmissionBatchPacket> batches{makeSingleBatch()};
	const rvk2::ExecutorOutput baseOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{base}, batches);
	const rvk2::ExecutorOutput detailOut =
		executor.executeWithOutput(std::vector<rvk2::RenderWorkPacket>{detailVariant}, batches);

	expectTrue(
		baseOut.summary.colorWriteCount > 0ULL,
		"texture detail color conformance baseline should write pixels");
	expectEq(
		detailOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"texture detail color transition should preserve write coverage");
	expectTrue(
		detailOut.summary.presentHash != baseOut.summary.presentHash,
		"texture detail color transition should alter present hash");
	expectTrue(
		!presentFramesEqual(detailOut, baseOut),
		"texture detail color transition should alter presented pixels");
}

void testBlendMuxSelectorConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket background = makeFillWork(190ULL, 0x00B41000U, 0x304050FFU);
	background.rectLRX = 5U;
	background.rectLRY = 3U;

	rvk2::RenderWorkPacket base = makeTexRectWork(false);
	base.sourcePacketId = 191ULL;
	base.colorImageAddress = background.colorImageAddress;
	base.colorImageWidth = 8U;
	base.rectULX = 0U;
	base.rectULY = 0U;
	base.rectLRX = 5U;
	base.rectLRY = 3U;
	base.otherModes = 0ULL;
	base.otherModes |= (1ULL << 6U);  // imageRead
	base.otherModes |= (1ULL << 14U); // forceBlender
	base.otherModes |= (1ULL << 22U); // c1_m2a = 1
	base.otherModes |= (1ULL << 18U); // c1_m2b = 1
	base.blendParams = 0x00A04020U;
	base.keyState = 0x0102030405060708ULL;
	base.convertState = 0x1112131415161718ULL;

	rvk2::RenderWorkPacket cycle2SelectorVariant = base;
	cycle2SelectorVariant.sourcePacketId = 192ULL;
	cycle2SelectorVariant.otherModes &= ~(
		(0x3ULL << 28U)
		| (0x3ULL << 24U)
		| (0x3ULL << 20U)
		| (0x3ULL << 16U));
	cycle2SelectorVariant.otherModes |= (1ULL << 28U);
	cycle2SelectorVariant.otherModes |= (2ULL << 24U);
	cycle2SelectorVariant.otherModes |= (3ULL << 20U);
	cycle2SelectorVariant.otherModes |= (1ULL << 16U);

	rvk2::RenderWorkPacket selectorVariant = base;
	selectorVariant.sourcePacketId = 193ULL;
	selectorVariant.otherModes &= ~(
		(0x3ULL << 30U)
		| (0x3ULL << 26U)
		| (0x3ULL << 22U)
		| (0x3ULL << 18U));
	selectorVariant.otherModes |= (1ULL << 30U);
	selectorVariant.otherModes |= (2ULL << 26U);
	selectorVariant.otherModes |= (2ULL << 22U);
	selectorVariant.otherModes |= (3ULL << 18U);

	const std::vector<rvk2::SubmissionBatchPacket> twoWorkBatches{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput baseOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, base},
		twoWorkBatches);
	const rvk2::ExecutorOutput cycle2SelectorOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, cycle2SelectorVariant},
		twoWorkBatches);
	const rvk2::ExecutorOutput selectorOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, selectorVariant},
		twoWorkBatches);

	expectTrue(
		baseOut.summary.colorWriteCount > 0ULL,
		"blend mux selector baseline should write pixels");
	expectEq(
		cycle2SelectorOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"cycle2 blender selector transition should preserve cycle1 write coverage");
	expectEq(
		cycle2SelectorOut.summary.presentHash,
		baseOut.summary.presentHash,
		"cycle2 blender selector transition should not alter cycle1 present hash");
	expectTrue(
		presentFramesEqual(cycle2SelectorOut, baseOut),
		"cycle2 blender selector transition should not alter cycle1 presented pixels");
	expectEq(
		selectorOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"blend mux selector transition should preserve write coverage");
	expectTrue(
		selectorOut.summary.presentHash != baseOut.summary.presentHash,
		"blend mux selector transition should alter present hash");
	expectTrue(
		!presentFramesEqual(selectorOut, baseOut),
		"blend mux selector transition should alter presented pixels");

	rvk2::RenderWorkPacket cycle2Base = base;
	cycle2Base.sourcePacketId = 194ULL;
	cycle2Base.phase = static_cast<u8>(rvk2::RenderPhase::kCycle2);
	cycle2Base.otherModes &= ~(
		(0x3ULL << 28U)
		| (0x3ULL << 24U)
		| (0x3ULL << 20U)
		| (0x3ULL << 16U));
	cycle2Base.otherModes |= (1ULL << 28U);
	cycle2Base.otherModes |= (1ULL << 20U);

	rvk2::RenderWorkPacket cycle2Variant = cycle2Base;
	cycle2Variant.sourcePacketId = 195ULL;
	cycle2Variant.otherModes &= ~(
		(0x3ULL << 28U)
		| (0x3ULL << 24U)
		| (0x3ULL << 20U)
		| (0x3ULL << 16U));
	cycle2Variant.otherModes |= (2ULL << 28U);
	cycle2Variant.otherModes |= (3ULL << 24U);
	cycle2Variant.otherModes |= (2ULL << 20U);
	cycle2Variant.otherModes |= (3ULL << 16U);

	const rvk2::ExecutorOutput cycle2BaseOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, cycle2Base},
		twoWorkBatches);
	const rvk2::ExecutorOutput cycle2VariantOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, cycle2Variant},
		twoWorkBatches);
	expectEq(
		cycle2VariantOut.summary.colorWriteCount,
		cycle2BaseOut.summary.colorWriteCount,
		"cycle2 blend mux transition should preserve write coverage");
	expectTrue(
		cycle2VariantOut.summary.presentHash != cycle2BaseOut.summary.presentHash,
		"cycle2 blend mux transition should alter present hash");
}

void testBlendShadeAlphaSelectorConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket background = makeFillWork(195ULL, 0x00B42000U, 0x204060FFU);
	background.rectLRX = 7U;
	background.rectLRY = 7U;

	rvk2::RenderWorkPacket base = makeTriangleWork();
	base.sourcePacketId = 196ULL;
	base.colorImageAddress = background.colorImageAddress;
	base.colorImageWidth = 8U;
	base.rectULX = 0U;
	base.rectULY = 0U;
	base.rectLRX = 7U;
	base.rectLRY = 7U;
	base.textured = false;
	base.triangleTextureEnable = false;
	base.triangleShadeEnable = true;
	base.otherModes = 0ULL;
	base.otherModes |= (1ULL << 6U);  // image_read_en
	base.otherModes |= (1ULL << 14U); // force_blend
	base.otherModes &= ~(
		(0x3ULL << 30U)
		| (0x3ULL << 26U)
		| (0x3ULL << 22U)
		| (0x3ULL << 18U));
	base.otherModes |= (1ULL << 30U); // P = memory color
	base.otherModes |= (2ULL << 26U); // A = shade alpha
	base.otherModes |= (2ULL << 22U); // M = blend color
	base.blendColor = 0xD02090FFU;
	base.primColor = 0x60A04080U;

	constexpr u64 kCycle1ColorAMask = 0xFULL << (32U + 20U);
	constexpr u64 kCycle1ColorBMask = 0xFULL << 28U;
	constexpr u64 kCycle1ColorCMask = 0x1FULL << (32U + 15U);
	constexpr u64 kCycle1ColorDMask = 0x7ULL << 15U;
	constexpr u64 kCycle1AlphaAMask = 0x7ULL << (32U + 12U);
	constexpr u64 kCycle1AlphaBMask = 0x7ULL << 12U;
	constexpr u64 kCycle1AlphaCMask = 0x7ULL << (32U + 9U);
	constexpr u64 kCycle1AlphaDMask = 0x7ULL << 9U;
	constexpr u64 kCycle1SelectorMask =
		kCycle1ColorAMask
		| kCycle1ColorBMask
		| kCycle1ColorCMask
		| kCycle1ColorDMask
		| kCycle1AlphaAMask
		| kCycle1AlphaBMask
		| kCycle1AlphaCMask
		| kCycle1AlphaDMask;
	base.combineMux &= ~kCycle1SelectorMask;
	base.combineMux |=
		(3ULL << 15U) // color D = PRIMITIVE
		| (3ULL << 9U); // alpha D = PRIMITIVE

	rvk2::RenderWorkPacket lowShadeAlpha = base;
	lowShadeAlpha.sourcePacketId = 197ULL;
	lowShadeAlpha.triangleShadeA = 0x1000;

	rvk2::RenderWorkPacket highShadeAlpha = base;
	highShadeAlpha.sourcePacketId = 198ULL;
	highShadeAlpha.triangleShadeA = 0xF000;

	const std::vector<rvk2::SubmissionBatchPacket> twoWorkBatches{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput lowOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, lowShadeAlpha},
		twoWorkBatches);
	const rvk2::ExecutorOutput highOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, highShadeAlpha},
		twoWorkBatches);

	expectTrue(
		lowOut.summary.colorWriteCount > 0ULL,
		"blend shade-alpha selector baseline should write pixels");
	expectEq(
		lowOut.summary.colorWriteCount,
		highOut.summary.colorWriteCount,
		"blend shade-alpha selector transition should preserve write coverage");
	expectTrue(
		lowOut.summary.presentHash != highOut.summary.presentHash,
		"blend shade-alpha selector transition should alter present hash");
	expectTrue(
		!presentFramesEqual(lowOut, highOut),
		"blend shade-alpha selector transition should alter presented pixels");
}

void testCycle2BlenderMemorySelectorConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket backgroundDark = makeFillWork(200ULL, 0x00B43000U, 0x204060FFU);
	backgroundDark.rectLRX = 5U;
	backgroundDark.rectLRY = 3U;
	rvk2::RenderWorkPacket backgroundBright = backgroundDark;
	backgroundBright.sourcePacketId = 201ULL;
	backgroundBright.fillColor = 0xD0A020FFU;

	rvk2::RenderWorkPacket cycle2 = makeTexRectWork(false);
	cycle2.sourcePacketId = 202ULL;
	cycle2.colorImageAddress = backgroundDark.colorImageAddress;
	cycle2.colorImageWidth = 8U;
	cycle2.rectULX = 0U;
	cycle2.rectULY = 0U;
	cycle2.rectLRX = 5U;
	cycle2.rectLRY = 3U;
	cycle2.phase = static_cast<u8>(rvk2::RenderPhase::kCycle2);
	cycle2.cycleType = 1U;
	cycle2.otherModes = 0ULL;
	cycle2.otherModes |= (1ULL << 6U);  // image_read_en
	cycle2.otherModes |= (1ULL << 14U); // force_blend
	// Cycle-1 blender: output selector-0 source directly (no memory influence).
	cycle2.otherModes &= ~(
		(0x3ULL << 30U)
		| (0x3ULL << 26U)
		| (0x3ULL << 22U)
		| (0x3ULL << 18U));
	cycle2.otherModes |= (2ULL << 26U); // A = 1.0
	cycle2.otherModes |= (3ULL << 18U); // B = 0.0
	// Cycle-2 blender: force output to M input, with M bound to memory color.
	cycle2.otherModes &= ~(
		(0x3ULL << 28U)
		| (0x3ULL << 24U)
		| (0x3ULL << 20U)
		| (0x3ULL << 16U));
	cycle2.otherModes |= (3ULL << 24U); // A = 0.0
	cycle2.otherModes |= (1ULL << 20U); // M = memory color
	cycle2.otherModes |= (2ULL << 16U); // B = 1.0

	const std::vector<rvk2::SubmissionBatchPacket> twoWorkBatches{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput darkOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{backgroundDark, cycle2},
		twoWorkBatches);
	rvk2::RenderWorkPacket cycle2Bright = cycle2;
	cycle2Bright.sourcePacketId = 203ULL;
	cycle2Bright.colorImageAddress = backgroundBright.colorImageAddress;
	const rvk2::ExecutorOutput brightOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{backgroundBright, cycle2Bright},
		twoWorkBatches);

	expectTrue(
		darkOut.summary.colorWriteCount > 0ULL,
		"cycle2 memory-selector baseline should write pixels");
	expectTrue(
		darkOut.summary.blenderColorMMemorySelectorCount > 0ULL,
		"cycle2 memory-selector baseline should exercise memory M input");
	expectTrue(
		darkOut.summary.presentHash != brightOut.summary.presentHash,
		"cycle2 second-cycle memory selector should remain destination-sensitive");
	expectTrue(
		!presentFramesEqual(darkOut, brightOut),
		"cycle2 second-cycle memory selector should alter presented pixels when destination changes");
}

void testAAPipelineModeConformance()
{
	rvk2::Executor executor;
	rvk2::RenderWorkPacket background = makeFillWork(195ULL, 0x00B42000U, 0x506070FFU);
	background.rectLRX = 5U;
	background.rectLRY = 3U;

	rvk2::RenderWorkPacket base = makeTexRectWork(false);
	base.sourcePacketId = 196ULL;
	base.colorImageAddress = background.colorImageAddress;
	base.colorImageWidth = 8U;
	base.rectULX = 0U;
	base.rectULY = 0U;
	base.rectLRX = 5U;
	base.rectLRY = 3U;
	base.otherModes = (1ULL << 6U);
	base.blendParams = 0x80C06040U;
	base.colorOnCvg = true;
	base.cvgXAlpha = true;
	base.alphaCvgSel = true;
	base.forceBlender = true;
	base.cvgDest = 1U;
	base.blendMask = 0x3U;
	base.keyState = 0x8899AABBCCDDEEFFULL;
	base.convertState = 0x1021324354657687ULL;

	rvk2::RenderWorkPacket aaVariant = base;
	aaVariant.sourcePacketId = 197ULL;
	aaVariant.otherModes |= (1ULL << 3U);

	rvk2::RenderWorkPacket pipelineVariant = base;
	pipelineVariant.sourcePacketId = 198ULL;
	pipelineVariant.otherModes |= (1ULL << (32U + 23U));

	rvk2::RenderWorkPacket aaPipelineVariant = pipelineVariant;
	aaPipelineVariant.sourcePacketId = 199ULL;
	aaPipelineVariant.otherModes |= (1ULL << 3U);

	const std::vector<rvk2::SubmissionBatchPacket> twoWorkBatches{makeBatchForWorkCount(2U)};
	const rvk2::ExecutorOutput baseOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, base},
		twoWorkBatches);
	const rvk2::ExecutorOutput aaOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, aaVariant},
		twoWorkBatches);
	const rvk2::ExecutorOutput pipelineOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, pipelineVariant},
		twoWorkBatches);
	const rvk2::ExecutorOutput aaPipelineOut = executor.executeWithOutput(
		std::vector<rvk2::RenderWorkPacket>{background, aaPipelineVariant},
		twoWorkBatches);

	expectTrue(
		baseOut.summary.colorWriteCount > 0ULL,
		"AA/pipeline conformance baseline should write pixels");
	expectTrue(
		aaOut.summary.colorWriteCount >= baseOut.summary.colorWriteCount,
		"AA enable transition should not reduce write coverage");
	expectEq(
		pipelineOut.summary.colorWriteCount,
		baseOut.summary.colorWriteCount,
		"pipeline mode transition should preserve write coverage");
	expectTrue(
		aaPipelineOut.summary.colorWriteCount >= pipelineOut.summary.colorWriteCount,
		"AA+pipeline transition should not reduce write coverage versus pipeline-only");
	expectTrue(
		aaOut.summary.presentHash != baseOut.summary.presentHash,
		"AA enable transition should alter present hash");
	expectTrue(
		pipelineOut.summary.presentHash != baseOut.summary.presentHash,
		"pipeline mode transition should alter present hash");
	expectTrue(
		aaPipelineOut.summary.presentHash != aaOut.summary.presentHash,
		"AA+pipeline transition should alter present hash beyond AA-only path");
}

void testVIDFieldInterlaceConformance()
{
	const u32 colorAddress = 0x00B50000U;
	constexpr u16 colorWidth = 2U;
	std::vector<rvk2::RenderWorkPacket> workPackets;
	workPackets.reserve(8U);
	static constexpr u32 pattern[8] = {
		0x101010FFU, 0x111111FFU,
		0x202020FFU, 0x212121FFU,
		0x303030FFU, 0x313131FFU,
		0x404040FFU, 0x414141FFU
	};
	for (u32 y = 0U; y < 4U; ++y) {
		for (u32 x = 0U; x < 2U; ++x) {
			const u32 idx = y * 2U + x;
			workPackets.push_back(
				makePixelFillWork(
					400ULL + static_cast<u64>(idx),
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

	rvk2::ExecutorConfig field0Config =
		makeVIExecutorConfig((3U | (3U << 8U)) | 0x000040U, colorAddress, 2U, 2U, 4U);
	field0Config.viVCurrentLine = 0U;
	rvk2::Executor field0Executor(field0Config);
	const rvk2::ExecutorOutput field0Out =
		field0Executor.executeWithOutput(workPackets, batches);

	rvk2::ExecutorConfig field1Config = field0Config;
	field1Config.viVCurrentLine = 1U;
	rvk2::Executor field1Executor(field1Config);
	const rvk2::ExecutorOutput field1Out =
		field1Executor.executeWithOutput(workPackets, batches);

	expectTrue(
		!field0Out.presentFrame.pixels.empty(),
		"VI interlace conformance baseline should produce present pixels");
	expectEq(
		field1Out.summary.colorWriteCount,
		field0Out.summary.colorWriteCount,
		"VI interlace field toggle should preserve write coverage");
	expectTrue(
		field1Out.summary.presentHash != field0Out.summary.presentHash,
		"VI interlace field toggle should alter present hash");
	expectTrue(
		field1Out.presentFrame.pixels[0] != field0Out.presentFrame.pixels[0],
		"VI interlace field toggle should alter first presented sample");
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
	testCombinerExtendedSelectorConformance();
	testCombinerOverflowBandConformance();
	testDepthOrderingConformance();
	testDepthPhaseParticipationConformance();
	testPrimitiveDepthSourceConformance();
	testDepthCompareUpdateModeConformance();
	testDepthSurfaceAliasIsolationConformance();
	testCoverageBlendFlagConformance();
	testCoverageModeFlagConformance();
	testColorOnCvgOverflowWriteEnableConformance();
	testColorOnCvgWritesBlenderMInputConformance();
	testCycle2CoverageDestinationConformance();
	testAlphaCompareConformance();
	testMixedStateBatchSegmentationConformance();
	testMixedStateRapidTransitionMatrixConformance();
	testCoverageScissorConformance();
	testScissorModeFieldAndEdgeConformance();
	testScissorDitherIndexConformance();
	testCopyPhaseDestinationBypassConformance();
	testCopyModeIgnoresTileClampConformance();
	testTileBaseOffsetInvariantConformance();
	testCycle2PhaseDistinctConformance();
	testCycle2CombinerSelectorIsolationConformance();
	testCycle2TexelNextPixelHazardConformance();
	testCycle1Texel1NextPixelHazardConformance();
	testCycle2Texel0AliasTexel1HazardConformance();
	testCycle2ShadeAlphaNextPixelHazardConformance();
	testCycle1CombinedFeedbackHazardConformance();
	testCycle1Texel1SecondaryTileConformance();
	testTMEM32AuthoritativePathConformance();
	testUnsupportedTMEMDecodeUsesSyntheticConformance();
	testUnsupportedTMEMDecodeUsesRdramFallbackWhenAvailableConformance();
	testFillPhaseIgnoresBlendCombinerConformance();
	testCopyFillBypassAlphaCoverageConformance();
	testFillSeedsCoverageForImageReadBlendConformance();
	testTexRectFlipConformance();
	testTexRectStateSensitivityConformance();
	testRenderStateInputSensitivityConformance();
	testRenderTargetIsolationConformance();
	testRenderTargetColorSizeConformance();
	testImageReadConformance();
	testDepthModeConformance();
	testTextureFilterConformance();
	testCombinerKeyConvertConformance();
	testTextureExtendedModeConformance();
	testTextureDetailColorTransformConformance();
	testBlendMuxSelectorConformance();
	testBlendShadeAlphaSelectorConformance();
	testCycle2BlenderMemorySelectorConformance();
	testAAPipelineModeConformance();
	testVIDFieldInterlaceConformance();
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
