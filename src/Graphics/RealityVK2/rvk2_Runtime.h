#pragma once

#include <vector>

#include "rvk2_CommandStream.h"
#include "rvk2_DrawSemantic.h"
#include "rvk2_RasterPipeline.h"
#include "rvk2_RenderPlan.h"
#include "rvk2_RDPState.h"
#include "rvk2_RSPFrontend.h"
#include "rvk2_SubmissionPlan.h"
#include "rvk2_TMEMModel.h"

namespace rvk2 {

class Runtime
{
public:
	Runtime();

	void reset();
	void beginFrame(u64 _frameId);

	void submitRDPWord(
		u32 _dlistAddress,
		u32 _w0,
		u32 _w1,
		const CommandProvenance & _provenance = CommandProvenance{},
		u8 _extraWordCount = 0U,
		u32 _w2 = 0U,
		u32 _w3 = 0U,
		u32 _w4 = 0U,
		u32 _w5 = 0U,
		u32 _w6 = 0U,
		u32 _w7 = 0U,
		u16 _fullWordCount = 0U,
		u64 _tailHash = 1469598103934665603ULL,
		u8 _payloadWordCount = 0U,
		const u32 * _payloadWords = nullptr);
	void submitRSPWord(
		u32 _dlistAddress,
		u32 _w0,
		u32 _w1,
		const CommandProvenance & _provenance = CommandProvenance{},
		u8 _extraWordCount = 0U,
		u32 _w2 = 0U,
		u32 _w3 = 0U,
		u32 _w4 = 0U,
		u32 _w5 = 0U,
		u32 _w6 = 0U,
		u32 _w7 = 0U,
		u16 _fullWordCount = 0U,
		u64 _tailHash = 1469598103934665603ULL,
		u8 _payloadWordCount = 0U,
		const u32 * _payloadWords = nullptr);

	const CommandStream & commandStream() const;
	const std::vector<DrawSemanticPacket> & drawSemantics() const;
	const std::vector<RasterOpPacket> & rasterOps() const;
	const std::vector<RenderWorkPacket> & renderPlan() const;
	const std::vector<SubmissionBatchPacket> & submissionPlan() const;
	const RDPStateEngine & rdpState() const;
	const TMEMModel & tmemModel() const;

	FrameTraceRecord buildFrameTrace() const;

private:
	RSPFrontend m_rspFrontend;
	CommandStream m_commandStream;
	std::vector<DrawSemanticPacket> m_drawSemantics;
	std::vector<RasterOpPacket> m_rasterOps;
	std::vector<RenderWorkPacket> m_renderPlan;
	std::vector<SubmissionBatchPacket> m_submissionBatches;
	RenderPlanState m_renderPlanState{};
	RDPStateEngine m_rdpState;
	TMEMModel m_tmemModel;
	u32 m_unknownRdpOpcodeCount = 0U;
	PacketId m_firstUnknownRdpPacketId = 0ULL;
	u8 m_firstUnknownRdpOpcode = 0U;
	u32 m_truncatedPayloadCount = 0U;
	PacketId m_firstTruncatedPayloadPacketId = 0ULL;
	u8 m_firstTruncatedPayloadOpcode = 0U;
};

Runtime & runtime();

} // namespace rvk2
