#include "rvk2_TraceOutput.h"

#include <cstdio>

#include "Log.h"
#include "rvk2_Runtime.h"
#include "rvk2_TraceConfig.h"
#include "rvk2_Validation.h"

namespace {

void appendTraceSummaryRecord(
	const rvk2::FrameTraceRecord & _trace,
	const rvk2::RDPStateSnapshot & _snapshot,
	const rvk2::ValidationResult & _validation,
	u32 _microcodeType)
{
	const char * path = rvk2::getTraceOutputPath();
	if (path == nullptr || path[0] == '\0')
		return;

	FILE * file = std::fopen(path, "a");
	if (file == nullptr)
		return;

	std::fprintf(
		file,
		"%llu\t%llu\t%llu\t%llu\t%llu\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n",
		static_cast<unsigned long long>(_trace.frameId),
		static_cast<unsigned long long>(_trace.commandCount),
		static_cast<unsigned long long>(_trace.lastPacketId),
		static_cast<unsigned long long>(_trace.commandHash),
		static_cast<unsigned long long>(_trace.stateHash),
		static_cast<u32>(_snapshot.cycleType),
		static_cast<u32>(_snapshot.colorImageFormat),
		static_cast<u32>(_snapshot.colorImageSize),
		static_cast<u32>(_snapshot.colorImageWidth),
		static_cast<u32>(_snapshot.colorImageAddress),
		static_cast<u32>(_snapshot.depthImageAddress),
		static_cast<u32>(_snapshot.changedMask),
		_validation.ok ? 1U : 0U,
		static_cast<u32>(_validation.warningCount),
		static_cast<u32>(_validation.errorCount),
		_microcodeType);
	std::fclose(file);
}

void appendPacketTraceDump(
	const rvk2::CommandStream & _stream,
	const std::vector<rvk2::DrawSemanticPacket> & _semantics,
	const std::vector<rvk2::RasterOpPacket> & _rasterOps,
	const std::vector<rvk2::RenderWorkPacket> & _renderWork,
	const std::vector<rvk2::SubmissionBatchPacket> & _submissionBatches,
	const rvk2::FrameTraceRecord & _trace,
	u32 _microcodeType)
{
	const char * path = rvk2::getPacketTraceOutputPath();
	if (path == nullptr || path[0] == '\0')
		return;

	FILE * file = std::fopen(path, "a");
	if (file == nullptr)
		return;

		std::fprintf(
			file,
			"F\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%u\t%llu\t%u\t%u\t%llu\t%u\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%u\t%u\t%u\t%u\t%u\n",
		static_cast<unsigned long long>(_trace.frameId),
		static_cast<unsigned long long>(_trace.commandCount),
		static_cast<unsigned long long>(_trace.drawSemanticCount),
		static_cast<unsigned long long>(_trace.rasterOpCount),
		static_cast<unsigned long long>(_trace.lastPacketId),
		static_cast<unsigned long long>(_trace.commandHash),
		static_cast<unsigned long long>(_trace.stateHash),
		static_cast<unsigned long long>(_trace.drawSemanticHash),
		static_cast<unsigned long long>(_trace.rasterOpHash),
		static_cast<u32>(_trace.unknownRdpOpcodeCount),
		static_cast<unsigned long long>(_trace.firstUnknownRdpPacketId),
		static_cast<u32>(_trace.firstUnknownRdpOpcode),
		static_cast<u32>(_trace.truncatedPayloadCount),
		static_cast<unsigned long long>(_trace.firstTruncatedPayloadPacketId),
		static_cast<u32>(_trace.firstTruncatedPayloadOpcode),
		static_cast<unsigned long long>(_trace.renderWorkCount),
		static_cast<unsigned long long>(_trace.renderWorkHash),
		static_cast<unsigned long long>(_trace.submissionBatchCount),
		static_cast<unsigned long long>(_trace.submissionBatchHash),
		static_cast<unsigned long long>(_trace.executorWorkCount),
		static_cast<unsigned long long>(_trace.executorBatchCount),
		static_cast<unsigned long long>(_trace.executorColorWriteCount),
		static_cast<unsigned long long>(_trace.executorSurfaceCount),
		static_cast<unsigned long long>(_trace.executorPresentHash),
		static_cast<u32>(_trace.executorPresentWidth),
		static_cast<u32>(_trace.executorPresentHeight),
		static_cast<u32>(_trace.executorPresentAspectX),
		static_cast<u32>(_trace.executorPresentAspectY),
		_microcodeType);

	const auto & packets = _stream.commands();
	for (const rvk2::CommandPacket & packet : packets) {
		const u32 payloadWordCount =
			static_cast<u32>(
				packet.payloadWordCount <= static_cast<u8>(rvk2::kMaxCommandPayloadWords)
					? packet.payloadWordCount
					: static_cast<u8>(rvk2::kMaxCommandPayloadWords));
		std::fprintf(
			file,
			"P\t%llu\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%llu\t%u\t%u",
			static_cast<unsigned long long>(packet.id),
			static_cast<u32>(packet.domain),
			static_cast<u32>(packet.opcode),
			static_cast<u32>(packet.flags),
			static_cast<u32>(packet.w0),
			static_cast<u32>(packet.w1),
			static_cast<u32>(packet.provenance.taskId),
			static_cast<u32>(packet.provenance.dlistAddress),
			static_cast<u32>(packet.provenance.microcode),
			static_cast<u32>(packet.extraWordCount),
			static_cast<u32>(packet.w2),
			static_cast<u32>(packet.w3),
			static_cast<u32>(packet.w4),
			static_cast<u32>(packet.w5),
			static_cast<u32>(packet.w6),
			static_cast<u32>(packet.w7),
			static_cast<u32>(packet.fullWordCount),
			static_cast<unsigned long long>(packet.tailHash),
			_microcodeType,
			payloadWordCount);
		for (u32 payloadIndex = 0U; payloadIndex < payloadWordCount; ++payloadIndex)
			std::fprintf(file, "\t%u", static_cast<u32>(packet.payloadWords[payloadIndex]));
		std::fprintf(file, "\n");
		}

	for (const rvk2::DrawSemanticPacket & semantic : _semantics) {
		auto emitU64 = [&](u64 value) {
			std::fprintf(file, "\t%llu", static_cast<unsigned long long>(value));
		};
		auto emitU32 = [&](u32 value) {
			std::fprintf(file, "\t%u", value);
		};
		std::fprintf(file, "S");
		emitU64(semantic.sourcePacketId);
		emitU32(static_cast<u32>(semantic.sourceOpcode));
		emitU32(static_cast<u32>(semantic.drawType));
		emitU32(static_cast<u32>(semantic.tile));
		emitU32(semantic.texRectFlip ? 1U : 0U);
		emitU32(static_cast<u32>(semantic.cycleType));
		emitU64(semantic.combineMux);
		emitU32(static_cast<u32>(semantic.blendMux1));
		emitU32(static_cast<u32>(semantic.blendMux2));
		emitU32(static_cast<u32>(semantic.blendParams));
		emitU32(static_cast<u32>(semantic.rectULX));
		emitU32(static_cast<u32>(semantic.rectULY));
		emitU32(static_cast<u32>(semantic.rectLRX));
		emitU32(static_cast<u32>(semantic.rectLRY));
		emitU32(static_cast<u32>(static_cast<u16>(semantic.texS)));
		emitU32(static_cast<u32>(static_cast<u16>(semantic.texT)));
		emitU32(static_cast<u32>(static_cast<u16>(semantic.texDSDX)));
		emitU32(static_cast<u32>(static_cast<u16>(semantic.texDTDY)));
		emitU32(semantic.triangleLMajor ? 1U : 0U);
		emitU32(static_cast<u32>(semantic.triangleLevel));
		emitU32(static_cast<u32>(semantic.triangleYL));
		emitU32(static_cast<u32>(semantic.triangleYM));
		emitU32(static_cast<u32>(semantic.triangleYH));
		emitU32(static_cast<u32>(semantic.triangleXL));
		emitU32(static_cast<u32>(semantic.triangleXH));
		emitU32(static_cast<u32>(semantic.triangleXM));
		emitU32(static_cast<u32>(semantic.triangleDxLDY));
		emitU32(static_cast<u32>(semantic.triangleDxHDY));
		emitU32(static_cast<u32>(semantic.triangleDxMDY));
		emitU32(semantic.triangleShadeEnable ? 1U : 0U);
		emitU32(semantic.triangleTextureEnable ? 1U : 0U);
		emitU32(semantic.triangleZBufferEnable ? 1U : 0U);
		emitU32(static_cast<u32>(semantic.triangleShadeR));
		emitU32(static_cast<u32>(semantic.triangleShadeG));
		emitU32(static_cast<u32>(semantic.triangleShadeB));
		emitU32(static_cast<u32>(semantic.triangleShadeA));
		emitU32(static_cast<u32>(semantic.triangleShadeDRDX));
		emitU32(static_cast<u32>(semantic.triangleShadeDGDX));
		emitU32(static_cast<u32>(semantic.triangleShadeDBDX));
		emitU32(static_cast<u32>(semantic.triangleShadeDADX));
		emitU32(static_cast<u32>(semantic.triangleShadeDRDE));
		emitU32(static_cast<u32>(semantic.triangleShadeDGDE));
		emitU32(static_cast<u32>(semantic.triangleShadeDBDE));
		emitU32(static_cast<u32>(semantic.triangleShadeDADE));
		emitU32(static_cast<u32>(semantic.triangleShadeDRDY));
		emitU32(static_cast<u32>(semantic.triangleShadeDGDY));
		emitU32(static_cast<u32>(semantic.triangleShadeDBDY));
		emitU32(static_cast<u32>(semantic.triangleShadeDADY));
		emitU32(static_cast<u32>(semantic.triangleTexS));
		emitU32(static_cast<u32>(semantic.triangleTexT));
		emitU32(static_cast<u32>(semantic.triangleTexW));
		emitU32(static_cast<u32>(semantic.triangleTexDSDX));
		emitU32(static_cast<u32>(semantic.triangleTexDTDX));
		emitU32(static_cast<u32>(semantic.triangleTexDWDX));
		emitU32(static_cast<u32>(semantic.triangleTexDSDE));
		emitU32(static_cast<u32>(semantic.triangleTexDTDE));
		emitU32(static_cast<u32>(semantic.triangleTexDWDE));
		emitU32(static_cast<u32>(semantic.triangleTexDSDY));
		emitU32(static_cast<u32>(semantic.triangleTexDTDY));
		emitU32(static_cast<u32>(semantic.triangleTexDWDY));
		emitU32(static_cast<u32>(semantic.triangleZ));
		emitU32(static_cast<u32>(semantic.triangleDZDX));
		emitU32(static_cast<u32>(semantic.triangleDZDE));
		emitU32(static_cast<u32>(semantic.triangleDZDY));
		emitU32(semantic.textured ? 1U : 0U);
		emitU32(semantic.depthTest ? 1U : 0U);
		emitU32(static_cast<u32>(semantic.syncEpoch));
		emitU64(semantic.loadSyncPacketId);
		emitU64(semantic.pipeSyncPacketId);
		emitU64(semantic.tileSyncPacketId);
		emitU64(semantic.fullSyncPacketId);
		std::fprintf(file, "\n");
		}

	for (const rvk2::RasterOpPacket & op : _rasterOps) {
		auto emitU64 = [&](u64 value) {
			std::fprintf(file, "\t%llu", static_cast<unsigned long long>(value));
		};
		auto emitU32 = [&](u32 value) {
			std::fprintf(file, "\t%u", value);
		};
		std::fprintf(file, "R");
		emitU64(op.sourcePacketId);
		emitU32(static_cast<u32>(op.sourceOpcode));
		emitU32(static_cast<u32>(op.opKind));
		emitU32(static_cast<u32>(op.cycleType));
		emitU32(static_cast<u32>(op.tile));
		emitU32(op.texRectFlip ? 1U : 0U);
		emitU32(op.textured ? 1U : 0U);
		emitU32(op.depthTest ? 1U : 0U);
		emitU32(static_cast<u32>(op.rectULX));
		emitU32(static_cast<u32>(op.rectULY));
		emitU32(static_cast<u32>(op.rectLRX));
		emitU32(static_cast<u32>(op.rectLRY));
		emitU32(static_cast<u32>(static_cast<u16>(op.texS)));
		emitU32(static_cast<u32>(static_cast<u16>(op.texT)));
		emitU32(static_cast<u32>(static_cast<u16>(op.texDSDX)));
		emitU32(static_cast<u32>(static_cast<u16>(op.texDTDY)));
		emitU32(op.triangleLMajor ? 1U : 0U);
		emitU32(static_cast<u32>(op.triangleLevel));
		emitU32(static_cast<u32>(op.triangleYL));
		emitU32(static_cast<u32>(op.triangleYM));
		emitU32(static_cast<u32>(op.triangleYH));
		emitU32(static_cast<u32>(op.triangleXL));
		emitU32(static_cast<u32>(op.triangleXH));
		emitU32(static_cast<u32>(op.triangleXM));
		emitU32(static_cast<u32>(op.triangleDxLDY));
		emitU32(static_cast<u32>(op.triangleDxHDY));
		emitU32(static_cast<u32>(op.triangleDxMDY));
		emitU32(op.triangleShadeEnable ? 1U : 0U);
		emitU32(op.triangleTextureEnable ? 1U : 0U);
		emitU32(op.triangleZBufferEnable ? 1U : 0U);
		emitU32(static_cast<u32>(op.triangleShadeR));
		emitU32(static_cast<u32>(op.triangleShadeG));
		emitU32(static_cast<u32>(op.triangleShadeB));
		emitU32(static_cast<u32>(op.triangleShadeA));
		emitU32(static_cast<u32>(op.triangleShadeDRDX));
		emitU32(static_cast<u32>(op.triangleShadeDGDX));
		emitU32(static_cast<u32>(op.triangleShadeDBDX));
		emitU32(static_cast<u32>(op.triangleShadeDADX));
		emitU32(static_cast<u32>(op.triangleShadeDRDE));
		emitU32(static_cast<u32>(op.triangleShadeDGDE));
		emitU32(static_cast<u32>(op.triangleShadeDBDE));
		emitU32(static_cast<u32>(op.triangleShadeDADE));
		emitU32(static_cast<u32>(op.triangleShadeDRDY));
		emitU32(static_cast<u32>(op.triangleShadeDGDY));
		emitU32(static_cast<u32>(op.triangleShadeDBDY));
		emitU32(static_cast<u32>(op.triangleShadeDADY));
		emitU32(static_cast<u32>(op.triangleTexS));
		emitU32(static_cast<u32>(op.triangleTexT));
		emitU32(static_cast<u32>(op.triangleTexW));
		emitU32(static_cast<u32>(op.triangleTexDSDX));
		emitU32(static_cast<u32>(op.triangleTexDTDX));
		emitU32(static_cast<u32>(op.triangleTexDWDX));
		emitU32(static_cast<u32>(op.triangleTexDSDE));
		emitU32(static_cast<u32>(op.triangleTexDTDE));
		emitU32(static_cast<u32>(op.triangleTexDWDE));
		emitU32(static_cast<u32>(op.triangleTexDSDY));
		emitU32(static_cast<u32>(op.triangleTexDTDY));
		emitU32(static_cast<u32>(op.triangleTexDWDY));
		emitU32(static_cast<u32>(op.triangleZ));
		emitU32(static_cast<u32>(op.triangleDZDX));
		emitU32(static_cast<u32>(op.triangleDZDE));
		emitU32(static_cast<u32>(op.triangleDZDY));
		emitU64(op.combineMux);
		emitU32(static_cast<u32>(op.blendParams));
		emitU32(static_cast<u32>(op.fillColor));
		emitU32(static_cast<u32>(op.syncEpoch));
		emitU64(op.loadSyncPacketId);
		emitU64(op.pipeSyncPacketId);
		emitU64(op.tileSyncPacketId);
		emitU64(op.fullSyncPacketId);
		std::fprintf(file, "\n");
	}

	for (const rvk2::RenderWorkPacket & work : _renderWork) {
		auto emitU64 = [&](u64 value) {
			std::fprintf(file, "\t%llu", static_cast<unsigned long long>(value));
		};
		auto emitU32 = [&](u32 value) {
			std::fprintf(file, "\t%u", value);
		};
		std::fprintf(file, "W");
		emitU64(work.sourcePacketId);
		emitU32(static_cast<u32>(work.sourceOpcode));
		emitU32(static_cast<u32>(work.opKind));
		emitU32(static_cast<u32>(work.phase));
		emitU32(static_cast<u32>(work.cycleType));
		emitU32(static_cast<u32>(work.barrierMask));
		emitU32(static_cast<u32>(work.tile));
		emitU32(work.texRectFlip ? 1U : 0U);
		emitU32(work.textured ? 1U : 0U);
		emitU32(work.depthTest ? 1U : 0U);
		emitU32(static_cast<u32>(work.rectULX));
		emitU32(static_cast<u32>(work.rectULY));
		emitU32(static_cast<u32>(work.rectLRX));
		emitU32(static_cast<u32>(work.rectLRY));
		emitU32(static_cast<u32>(static_cast<u16>(work.texS)));
		emitU32(static_cast<u32>(static_cast<u16>(work.texT)));
		emitU32(static_cast<u32>(static_cast<u16>(work.texDSDX)));
		emitU32(static_cast<u32>(static_cast<u16>(work.texDTDY)));
		emitU32(work.triangleLMajor ? 1U : 0U);
		emitU32(static_cast<u32>(work.triangleLevel));
		emitU32(static_cast<u32>(work.triangleYL));
		emitU32(static_cast<u32>(work.triangleYM));
		emitU32(static_cast<u32>(work.triangleYH));
		emitU32(static_cast<u32>(work.triangleXL));
		emitU32(static_cast<u32>(work.triangleXH));
		emitU32(static_cast<u32>(work.triangleXM));
		emitU32(static_cast<u32>(work.triangleDxLDY));
		emitU32(static_cast<u32>(work.triangleDxHDY));
		emitU32(static_cast<u32>(work.triangleDxMDY));
		emitU32(work.triangleShadeEnable ? 1U : 0U);
		emitU32(work.triangleTextureEnable ? 1U : 0U);
		emitU32(work.triangleZBufferEnable ? 1U : 0U);
		emitU32(static_cast<u32>(work.triangleShadeR));
		emitU32(static_cast<u32>(work.triangleShadeG));
		emitU32(static_cast<u32>(work.triangleShadeB));
		emitU32(static_cast<u32>(work.triangleShadeA));
		emitU32(static_cast<u32>(work.triangleShadeDRDX));
		emitU32(static_cast<u32>(work.triangleShadeDGDX));
		emitU32(static_cast<u32>(work.triangleShadeDBDX));
		emitU32(static_cast<u32>(work.triangleShadeDADX));
		emitU32(static_cast<u32>(work.triangleShadeDRDE));
		emitU32(static_cast<u32>(work.triangleShadeDGDE));
		emitU32(static_cast<u32>(work.triangleShadeDBDE));
		emitU32(static_cast<u32>(work.triangleShadeDADE));
		emitU32(static_cast<u32>(work.triangleShadeDRDY));
		emitU32(static_cast<u32>(work.triangleShadeDGDY));
		emitU32(static_cast<u32>(work.triangleShadeDBDY));
		emitU32(static_cast<u32>(work.triangleShadeDADY));
		emitU32(static_cast<u32>(work.triangleTexS));
		emitU32(static_cast<u32>(work.triangleTexT));
		emitU32(static_cast<u32>(work.triangleTexW));
		emitU32(static_cast<u32>(work.triangleTexDSDX));
		emitU32(static_cast<u32>(work.triangleTexDTDX));
		emitU32(static_cast<u32>(work.triangleTexDWDX));
		emitU32(static_cast<u32>(work.triangleTexDSDE));
		emitU32(static_cast<u32>(work.triangleTexDTDE));
		emitU32(static_cast<u32>(work.triangleTexDWDE));
		emitU32(static_cast<u32>(work.triangleTexDSDY));
		emitU32(static_cast<u32>(work.triangleTexDTDY));
		emitU32(static_cast<u32>(work.triangleTexDWDY));
		emitU32(static_cast<u32>(work.triangleZ));
		emitU32(static_cast<u32>(work.triangleDZDX));
		emitU32(static_cast<u32>(work.triangleDZDE));
		emitU32(static_cast<u32>(work.triangleDZDY));
		emitU32(static_cast<u32>(work.colorImageFormat));
		emitU32(static_cast<u32>(work.colorImageSize));
		emitU32(static_cast<u32>(work.colorImageWidth));
		emitU32(static_cast<u32>(work.colorImageAddress));
		emitU32(static_cast<u32>(work.depthImageAddress));
		emitU32(static_cast<u32>(work.scissorMode));
		emitU32(static_cast<u32>(work.scissorXH));
		emitU32(static_cast<u32>(work.scissorYH));
		emitU32(static_cast<u32>(work.scissorXL));
		emitU32(static_cast<u32>(work.scissorYL));
		emitU32(static_cast<u32>(work.textureImageFormat));
		emitU32(static_cast<u32>(work.textureImageSize));
		emitU32(static_cast<u32>(work.textureImageWidth));
		emitU32(static_cast<u32>(work.textureImageAddress));
		emitU32(static_cast<u32>(work.tileFormat));
		emitU32(static_cast<u32>(work.tileSize));
		emitU32(static_cast<u32>(work.tileLine));
		emitU32(static_cast<u32>(work.tileTmem));
		emitU32(static_cast<u32>(work.tilePalette));
		emitU32(static_cast<u32>(work.tileCmt));
		emitU32(static_cast<u32>(work.tileCms));
		emitU32(static_cast<u32>(work.tileMaskt));
		emitU32(static_cast<u32>(work.tileMasks));
		emitU32(static_cast<u32>(work.tileShiftt));
		emitU32(static_cast<u32>(work.tileShifts));
		emitU32(static_cast<u32>(work.tileULS));
		emitU32(static_cast<u32>(work.tileULT));
		emitU32(static_cast<u32>(work.tileLRS));
		emitU32(static_cast<u32>(work.tileLRT));
		emitU32(work.tile1Valid ? 1U : 0U);
		emitU32(static_cast<u32>(work.tile1Index));
		emitU32(static_cast<u32>(work.tile1Format));
		emitU32(static_cast<u32>(work.tile1Size));
		emitU32(static_cast<u32>(work.tile1Line));
		emitU32(static_cast<u32>(work.tile1Tmem));
		emitU32(static_cast<u32>(work.tile1Palette));
		emitU32(static_cast<u32>(work.tile1Cmt));
		emitU32(static_cast<u32>(work.tile1Cms));
		emitU32(static_cast<u32>(work.tile1Maskt));
		emitU32(static_cast<u32>(work.tile1Masks));
		emitU32(static_cast<u32>(work.tile1Shiftt));
		emitU32(static_cast<u32>(work.tile1Shifts));
		emitU32(static_cast<u32>(work.tile1ULS));
		emitU32(static_cast<u32>(work.tile1ULT));
		emitU32(static_cast<u32>(work.tile1LRS));
		emitU32(static_cast<u32>(work.tile1LRT));
		emitU32(static_cast<u32>(work.tmemLoadKind));
		emitU32(static_cast<u32>(work.tmemLoadTile));
		emitU32(static_cast<u32>(work.tmemLoadULS));
		emitU32(static_cast<u32>(work.tmemLoadULT));
		emitU32(static_cast<u32>(work.tmemLoadLRS));
		emitU32(static_cast<u32>(work.tmemLoadLRT));
		emitU32(static_cast<u32>(work.tmemLoadDXT));
		emitU64(work.combineMux);
		emitU32(static_cast<u32>(work.blendParams));
		emitU32(static_cast<u32>(work.fillColor));
		emitU32(static_cast<u32>(work.syncEpoch));
		emitU64(work.loadSyncPacketId);
		emitU64(work.pipeSyncPacketId);
		emitU64(work.tileSyncPacketId);
		emitU64(work.fullSyncPacketId);
		emitU32(work.depthCompareEnable ? 1U : 0U);
		emitU32(work.depthUpdateEnable ? 1U : 0U);
		emitU32(static_cast<u32>(work.alphaCompare));
		emitU32(static_cast<u32>(work.cvgDest));
		emitU32(static_cast<u32>(work.blendMask));
		emitU32(work.cvgXAlpha ? 1U : 0U);
		emitU32(work.alphaCvgSel ? 1U : 0U);
		emitU32(work.colorOnCvg ? 1U : 0U);
		emitU32(work.forceBlender ? 1U : 0U);
		emitU32(static_cast<u32>(work.depthSource));
		emitU32(static_cast<u32>(work.primDepthZ));
		emitU32(static_cast<u32>(work.primDepthDelta));
		emitU64(work.otherModes);
		emitU32(static_cast<u32>(work.primColor));
		emitU32(static_cast<u32>(work.envColor));
		emitU32(static_cast<u32>(work.blendColor));
		emitU32(static_cast<u32>(work.fogColor));
		emitU32(static_cast<u32>(work.primColorMinLevel));
		emitU32(static_cast<u32>(work.primColorLodFrac));
		emitU32(static_cast<u32>(static_cast<u16>(work.convertK4)));
		emitU32(static_cast<u32>(static_cast<u16>(work.convertK5)));
		emitU32(static_cast<u32>(work.keyCenterR));
		emitU32(static_cast<u32>(work.keyScaleR));
		emitU32(static_cast<u32>(work.keyCenterG));
		emitU32(static_cast<u32>(work.keyScaleG));
		emitU32(static_cast<u32>(work.keyCenterB));
		emitU32(static_cast<u32>(work.keyScaleB));
		emitU64(work.keyState);
		emitU64(work.convertState);
		std::fprintf(file, "\n");
	}

	for (const rvk2::SubmissionBatchPacket & batch : _submissionBatches) {
		std::fprintf(
			file,
			"B\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%llu\t%llu\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n",
			static_cast<u32>(batch.batchIndex),
			static_cast<u32>(batch.splitReason),
			static_cast<u32>(batch.splitBarrierMask),
			static_cast<u32>(batch.phase),
			static_cast<u32>(batch.cycleType),
			static_cast<u32>(batch.firstWorkIndex),
			static_cast<u32>(batch.lastWorkIndex),
			static_cast<u32>(batch.workCount),
			static_cast<u32>(batch.barrierMaskUnion),
			static_cast<u32>(batch.texturedWorkCount),
			static_cast<u32>(batch.depthTestWorkCount),
			static_cast<unsigned long long>(batch.firstSourcePacketId),
			static_cast<unsigned long long>(batch.lastSourcePacketId),
			static_cast<u32>(batch.colorImageFormat),
			static_cast<u32>(batch.colorImageSize),
			static_cast<u32>(batch.colorImageWidth),
			static_cast<u32>(batch.colorImageAddress),
			static_cast<u32>(batch.depthImageAddress),
			static_cast<u32>(batch.scissorMode),
			static_cast<u32>(batch.scissorXH),
			static_cast<u32>(batch.scissorYH),
			static_cast<u32>(batch.scissorXL),
			static_cast<u32>(batch.scissorYL));
	}
	std::fclose(file);
}

void maybeLogTraceSummary(
	const rvk2::FrameTraceRecord & _trace,
	const rvk2::RDPStateSnapshot & _snapshot,
	const rvk2::ValidationResult & _validation)
{
	if (_validation.ok && !rvk2::shouldLogTraceSummary())
		return;

	LOG(
		LOG_WARNING,
		"rvk2 trace: frame=%llu commands=%llu cycle=%u changed=0x%x ok=%u warn=%u err=%u",
		static_cast<unsigned long long>(_trace.frameId),
		static_cast<unsigned long long>(_trace.commandCount),
		static_cast<u32>(_snapshot.cycleType),
		static_cast<u32>(_snapshot.changedMask),
		_validation.ok ? 1U : 0U,
		static_cast<u32>(_validation.warningCount),
		static_cast<u32>(_validation.errorCount));
}

} // namespace

namespace rvk2 {

void emitCapturedFrameTrace(u32 _microcodeType)
{
	const Runtime & traceRuntime = runtime();
	const FrameTraceRecord trace = traceRuntime.buildFrameTrace();
	const RDPStateSnapshot & snapshot = traceRuntime.rdpState().snapshot();
	Validation validator;
	const ValidationResult validation = validator.validateFrame(trace, snapshot);
	appendTraceSummaryRecord(trace, snapshot, validation, _microcodeType);
	appendPacketTraceDump(
		traceRuntime.commandStream(),
		traceRuntime.drawSemantics(),
		traceRuntime.rasterOps(),
		traceRuntime.renderPlan(),
		traceRuntime.submissionPlan(),
		trace,
		_microcodeType);
	maybeLogTraceSummary(trace, snapshot, validation);
}

} // namespace rvk2
