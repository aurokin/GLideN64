#include "rvk2_TraceOutput.h"

#include <cstdio>

#include "Log.h"
#include "rvk2_Runtime.h"
#include "rvk2_RuntimeSwitch.h"
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
		std::fprintf(
			file,
			"P\t%llu\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%llu\t%u\n",
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
			_microcodeType);
		}

	for (const rvk2::DrawSemanticPacket & semantic : _semantics) {
		std::fprintf(
			file,
			"S\t%llu\t%u\t%u\t%u\t%u\t%u\t%llu\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%llu\t%llu\t%llu\t%llu\n",
			static_cast<unsigned long long>(semantic.sourcePacketId),
			static_cast<u32>(semantic.sourceOpcode),
			static_cast<u32>(semantic.drawType),
			static_cast<u32>(semantic.tile),
			semantic.texRectFlip ? 1U : 0U,
			static_cast<u32>(semantic.cycleType),
			static_cast<unsigned long long>(semantic.combineMux),
			static_cast<u32>(semantic.blendMux1),
			static_cast<u32>(semantic.blendMux2),
			static_cast<u32>(semantic.blendParams),
			static_cast<u32>(semantic.rectULX),
			static_cast<u32>(semantic.rectULY),
			static_cast<u32>(semantic.rectLRX),
			static_cast<u32>(semantic.rectLRY),
			static_cast<u32>(static_cast<u16>(semantic.texS)),
			static_cast<u32>(static_cast<u16>(semantic.texT)),
			static_cast<u32>(static_cast<u16>(semantic.texDSDX)),
			static_cast<u32>(static_cast<u16>(semantic.texDTDY)),
			semantic.triangleLMajor ? 1U : 0U,
			static_cast<u32>(semantic.triangleLevel),
			static_cast<u32>(semantic.triangleYL),
			static_cast<u32>(semantic.triangleYM),
			static_cast<u32>(semantic.triangleYH),
			static_cast<u32>(semantic.triangleXL),
			static_cast<u32>(semantic.triangleXH),
			static_cast<u32>(semantic.triangleXM),
			static_cast<u32>(semantic.triangleDxLDY),
			static_cast<u32>(semantic.triangleDxHDY),
			static_cast<u32>(semantic.triangleDxMDY),
			semantic.textured ? 1U : 0U,
			semantic.depthTest ? 1U : 0U,
			static_cast<u32>(semantic.syncEpoch),
			static_cast<unsigned long long>(semantic.loadSyncPacketId),
				static_cast<unsigned long long>(semantic.pipeSyncPacketId),
				static_cast<unsigned long long>(semantic.tileSyncPacketId),
				static_cast<unsigned long long>(semantic.fullSyncPacketId));
		}

	for (const rvk2::RasterOpPacket & op : _rasterOps) {
		std::fprintf(
			file,
			"R\t%llu\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%llu\t%u\t%u\t%u\t%llu\t%llu\t%llu\t%llu\n",
			static_cast<unsigned long long>(op.sourcePacketId),
			static_cast<u32>(op.sourceOpcode),
			static_cast<u32>(op.opKind),
			static_cast<u32>(op.cycleType),
			static_cast<u32>(op.tile),
			op.texRectFlip ? 1U : 0U,
			op.textured ? 1U : 0U,
			op.depthTest ? 1U : 0U,
			static_cast<u32>(op.rectULX),
			static_cast<u32>(op.rectULY),
			static_cast<u32>(op.rectLRX),
			static_cast<u32>(op.rectLRY),
			static_cast<u32>(static_cast<u16>(op.texS)),
			static_cast<u32>(static_cast<u16>(op.texT)),
			static_cast<u32>(static_cast<u16>(op.texDSDX)),
			static_cast<u32>(static_cast<u16>(op.texDTDY)),
			op.triangleLMajor ? 1U : 0U,
			static_cast<u32>(op.triangleLevel),
			static_cast<u32>(op.triangleYL),
			static_cast<u32>(op.triangleYM),
			static_cast<u32>(op.triangleYH),
			static_cast<u32>(op.triangleXL),
			static_cast<u32>(op.triangleXH),
			static_cast<u32>(op.triangleXM),
			static_cast<u32>(op.triangleDxLDY),
			static_cast<u32>(op.triangleDxHDY),
			static_cast<u32>(op.triangleDxMDY),
			static_cast<unsigned long long>(op.combineMux),
			static_cast<u32>(op.blendParams),
			static_cast<u32>(op.fillColor),
			static_cast<u32>(op.syncEpoch),
			static_cast<unsigned long long>(op.loadSyncPacketId),
			static_cast<unsigned long long>(op.pipeSyncPacketId),
			static_cast<unsigned long long>(op.tileSyncPacketId),
			static_cast<unsigned long long>(op.fullSyncPacketId));
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
