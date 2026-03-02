#include <cstdio>
#include <string>
#include <vector>

#include "rvk2_CommandStream.h"
#include "rvk2_Executor.h"
#include "rvk2_RDPState.h"
#include "rvk2_RenderPlan.h"
#include "rvk2_Runtime.h"
#include "rvk2_RSPFrontend.h"
#include "rvk2_SubmissionPlan.h"
#include "rvk2_SyntheticTriangle.h"
#include "rvk2_TMEMModel.h"
#include "rvk2_Types.h"
#include "rvk2_VIRenderer.h"

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

void testSchemaVersion()
{
	expectEq(rvk2::kSchemaVersion, 1U, "schema version must be 1");
	expectTrue(std::string(rvk2::kSchemaName) == "rvk2_schema_v1", "schema name must match freeze tag");
}

void testOpcodeDecodeIdentity()
{
	rvk2::RSPFrontend frontend;
	rvk2::CommandProvenance provenance{};
	provenance.taskId = 7U;
	provenance.microcode = 0x1234U;

	const rvk2::CommandPacket rdpPacket =
		frontend.ingestRDPCommand(0x00100000U, 0x3F10013FU, 0x00123400U, provenance);
	expectEq(rdpPacket.id, 1ULL, "RDP packet id should start at 1");
	expectEq(rdpPacket.domain, rvk2::CommandDomain::kRDP, "RDP packet domain mismatch");
	expectEq(rdpPacket.opcode, static_cast<u8>(0x3FU), "RDP packet opcode mismatch");
	expectEq(rdpPacket.provenance.taskId, 7U, "RDP packet taskId mismatch");
	expectEq(rdpPacket.provenance.microcode, static_cast<u16>(0x1234U), "RDP packet microcode mismatch");
	expectEq(rdpPacket.provenance.dlistAddress, 0x00100000U, "RDP packet dlist address mismatch");

	const rvk2::CommandPacket rspPacket =
		frontend.ingestRSPCommand(0x00100008U, 0xBD001122U, 0x33445566U, provenance);
	expectEq(rspPacket.id, 2ULL, "RSP packet id should increment");
	expectEq(rspPacket.domain, rvk2::CommandDomain::kRSP, "RSP packet domain mismatch");
	expectEq(rspPacket.opcode, static_cast<u8>(0xBDU), "RSP packet opcode mismatch");
	expectEq(rspPacket.provenance.dlistAddress, 0x00100008U, "RSP packet dlist address mismatch");
}

void testCommandHashStability()
{
	rvk2::CommandPacket packet{};
	packet.id = 1ULL;
	packet.domain = rvk2::CommandDomain::kRDP;
	packet.opcode = 0x3FU;
	packet.flags = 0U;
	packet.w0 = 0x3F10013FU;
	packet.w1 = 0x00123400U;
	packet.provenance.taskId = 1U;
	packet.provenance.dlistAddress = 0x1000U;
	packet.provenance.microcode = 20U;

	rvk2::CommandStream streamA;
	streamA.reset(1ULL);
	streamA.push(packet);
	expectEq(streamA.commandHash(), 0x2355B219CD5E7231ULL, "command hash stability constant mismatch");

	rvk2::CommandStream streamB;
	streamB.reset(1ULL);
	streamB.push(packet);
	expectEq(streamA.commandHash(), streamB.commandHash(), "equal command streams must hash equally");

	rvk2::CommandPacket packetChanged = packet;
	packetChanged.provenance.microcode = 21U;
	rvk2::CommandStream streamC;
	streamC.reset(1ULL);
	streamC.push(packetChanged);
	expectTrue(streamA.commandHash() != streamC.commandHash(), "hash should change when packet payload changes");

	rvk2::CommandPacket packetExtra = packet;
	packetExtra.extraWordCount = 2U;
	packetExtra.w2 = 0xDEADBEEFU;
	packetExtra.w3 = 0xCAFEBABEU;
	rvk2::CommandStream streamD;
	streamD.reset(1ULL);
	streamD.push(packetExtra);
	expectTrue(streamA.commandHash() != streamD.commandHash(), "hash should change when extra words are present");

	rvk2::CommandPacket packetTail = packetExtra;
	packetTail.fullWordCount = 8U;
	packetTail.tailHash = 0xAA55AA55AA55AA55ULL;
	rvk2::CommandStream streamE;
	streamE.reset(1ULL);
	streamE.push(packetTail);
	expectTrue(streamD.commandHash() != streamE.commandHash(), "hash should change when full-word metadata changes");
}

void testExtendedPayloadCaptureAndHash()
{
	const u32 payloadWords[10] = {
		0x10010001U,
		0x10020002U,
		0x10030003U,
		0x10040004U,
		0x10050005U,
		0x10060006U,
		0x10070007U,
		0x10080008U,
		0x10090009U,
		0x100A000AU
	};

	rvk2::Runtime runtime;
	runtime.beginFrame(23ULL);
	rvk2::CommandProvenance provenance{};
	provenance.taskId = 23U;
	provenance.microcode = 20U;
	runtime.submitRDPWord(
		0x00200000U,
		(0x0FU << 24) | (1U << 23) | (2U << 16) | 0x0123U,
		(0x0234U << 16) | 0x0034U,
		provenance,
		6U,
		0U,
		0U,
		0U,
		0U,
		0U,
		0U,
		12U,
		0xA5A5A5A5A5A5A5A5ULL,
		10U,
		payloadWords);

	const std::vector<rvk2::CommandPacket> & commands = runtime.commandStream().commands();
	expectEq(commands.size(), static_cast<size_t>(1U), "extended payload command count mismatch");
	const rvk2::CommandPacket & packet = commands[0];
	expectEq(packet.payloadWordCount, static_cast<u8>(10U), "extended payload word count mismatch");
	expectEq(packet.extraWordCount, static_cast<u8>(6U), "extended payload inline count mismatch");
	expectEq(packet.w2, payloadWords[0], "extended payload w2 mismatch");
	expectEq(packet.w3, payloadWords[1], "extended payload w3 mismatch");
	expectEq(packet.w4, payloadWords[2], "extended payload w4 mismatch");
	expectEq(packet.w5, payloadWords[3], "extended payload w5 mismatch");
	expectEq(packet.w6, payloadWords[4], "extended payload w6 mismatch");
	expectEq(packet.w7, payloadWords[5], "extended payload w7 mismatch");
	expectEq(packet.payloadWords[9], payloadWords[9], "extended payload tail word mismatch");

	const rvk2::FrameTraceRecord trace = runtime.buildFrameTrace();
	expectEq(trace.truncatedPayloadCount, 0U, "extended payload should not be marked truncated");

	rvk2::CommandStream streamA;
	streamA.reset(1ULL);
	streamA.push(packet);
	rvk2::CommandPacket packetChanged = packet;
	packetChanged.payloadWords[9] ^= 0x1U;
	rvk2::CommandStream streamB;
	streamB.reset(1ULL);
	streamB.push(packetChanged);
	expectTrue(
		streamA.commandHash() != streamB.commandHash(),
		"command hash must change when extended payload changes");
}

void testTriangleExtendedSemanticDecode()
{
	u32 payloadWords[42]{};
	for (u32 i = 0U; i < 42U; ++i)
		payloadWords[i] = 0x11000000U + (i * 0x00110011U);

	rvk2::Runtime runtime;
	runtime.beginFrame(31ULL);
	rvk2::CommandProvenance provenance{};
	provenance.taskId = 31U;
	provenance.microcode = 20U;
	runtime.submitRDPWord(
		0x00310000U,
		(0x0FU << 24) | (1U << 23) | (3U << 19) | (2U << 16) | 0x0145U,
		(0x0236U << 16) | 0x0012U,
		provenance,
		6U,
		0U,
		0U,
		0U,
		0U,
		0U,
		0U,
		44U,
		0x5AA55AA55AA55AA5ULL,
		42U,
		payloadWords);

	const std::vector<rvk2::DrawSemanticPacket> & semantics = runtime.drawSemantics();
	expectEq(semantics.size(), static_cast<size_t>(1U), "extended semantic draw count mismatch");
	const rvk2::DrawSemanticPacket & semantic = semantics[0];
	expectTrue(semantic.triangleShadeEnable, "triangle shade enable decode mismatch");
	expectTrue(semantic.triangleTextureEnable, "triangle texture enable decode mismatch");
	expectTrue(semantic.triangleZBufferEnable, "triangle zbuffer enable decode mismatch");

	auto decodePrimary = [](u32 _a, u32 _b) -> s32 {
		return static_cast<s32>((_a & 0xFFFF0000U) | ((_b >> 16U) & 0xFFFFU));
	};
	auto decodeSecondary = [](u32 _a, u32 _b) -> s32 {
		return static_cast<s32>(((_a << 16U) & 0xFFFF0000U) | (_b & 0xFFFFU));
	};

	// For 0x0F, expanded words [2..43] map directly from payload[0..41].
	expectEq(
		semantic.triangleShadeR,
		decodePrimary(payloadWords[6], payloadWords[10]),
		"triangle shade R decode mismatch");
	expectEq(
		semantic.triangleShadeG,
		decodeSecondary(payloadWords[6], payloadWords[10]),
		"triangle shade G decode mismatch");
	expectEq(
		semantic.triangleShadeDRDY,
		decodePrimary(payloadWords[16], payloadWords[20]),
		"triangle shade DRDY decode mismatch");
	expectEq(
		semantic.triangleShadeDADY,
		decodeSecondary(payloadWords[17], payloadWords[21]),
		"triangle shade DADY decode mismatch");

	expectEq(
		semantic.triangleTexS,
		decodePrimary(payloadWords[22], payloadWords[26]),
		"triangle texture S decode mismatch");
	expectEq(
		semantic.triangleTexT,
		decodeSecondary(payloadWords[22], payloadWords[26]),
		"triangle texture T decode mismatch");
	expectEq(
		semantic.triangleTexW,
		decodePrimary(payloadWords[23], payloadWords[27]),
		"triangle texture W decode mismatch");
	expectEq(
		semantic.triangleTexDSDY,
		decodePrimary(payloadWords[32], payloadWords[36]),
		"triangle texture DSDY decode mismatch");
	expectEq(
		semantic.triangleTexDWDY,
		decodePrimary(payloadWords[33], payloadWords[37]),
		"triangle texture DWDY decode mismatch");

	expectEq(
		semantic.triangleZ,
		static_cast<s32>(payloadWords[38]),
		"triangle Z decode mismatch");
	expectEq(
		semantic.triangleDZDX,
		static_cast<s32>(payloadWords[39]),
		"triangle DZDX decode mismatch");
	expectEq(
		semantic.triangleDZDE,
		static_cast<s32>(payloadWords[40]),
		"triangle DZDE decode mismatch");
	expectEq(
		semantic.triangleDZDY,
		static_cast<s32>(payloadWords[41]),
		"triangle DZDY decode mismatch");

	const std::vector<rvk2::RasterOpPacket> & rasterOps = runtime.rasterOps();
	expectEq(rasterOps.size(), static_cast<size_t>(1U), "extended semantic raster count mismatch");
	expectEq(rasterOps[0].triangleShadeR, semantic.triangleShadeR, "raster triangle shade R mismatch");
	expectEq(rasterOps[0].triangleTexS, semantic.triangleTexS, "raster triangle tex S mismatch");
	expectEq(rasterOps[0].triangleZ, semantic.triangleZ, "raster triangle Z mismatch");

	const std::vector<rvk2::RenderWorkPacket> & renderPlan = runtime.renderPlan();
	expectEq(renderPlan.size(), static_cast<size_t>(1U), "extended semantic render count mismatch");
	expectEq(renderPlan[0].triangleShadeDRDY, semantic.triangleShadeDRDY, "render triangle shade DRDY mismatch");
	expectEq(renderPlan[0].triangleTexDWDY, semantic.triangleTexDWDY, "render triangle tex DWDY mismatch");
	expectEq(renderPlan[0].triangleDZDE, semantic.triangleDZDE, "render triangle DZDE mismatch");
}

void testRDPStateTransitions()
{
	rvk2::RDPStateEngine engine;
	engine.reset();
	expectEq(engine.snapshot().changedMask, rvk2::rdp_state_changed::kNone, "reset changed mask must be zero");
	expectEq(engine.snapshot().lastPacketId, 0ULL, "reset last packet id must be zero");

	rvk2::CommandPacket nonRdp{};
	nonRdp.id = 99ULL;
	nonRdp.domain = rvk2::CommandDomain::kRSP;
	nonRdp.opcode = 0x3FU;
	nonRdp.w0 = 0x3F000000U;
	nonRdp.w1 = 0x12345678U;
	engine.apply(nonRdp);
	expectEq(engine.snapshot().lastPacketId, 0ULL, "non-RDP packet must not change state");

	rvk2::CommandPacket setOther{};
	setOther.id = 1ULL;
	setOther.domain = rvk2::CommandDomain::kRDP;
	setOther.opcode = 0x2FU;
	setOther.w0 = (0x2FU << 24) | (2U << 20) | 0x000345U;
	setOther.w1 = 0x89ABCDEFU;
	engine.apply(setOther);
	expectEq(engine.snapshot().cycleType, static_cast<u8>(2U), "SetOtherModes cycle type mismatch");
	expectTrue((engine.snapshot().changedMask & rvk2::rdp_state_changed::kOtherModes) != 0U, "SetOtherModes changed mask missing");

	rvk2::CommandPacket setCombine{};
	setCombine.id = 2ULL;
	setCombine.domain = rvk2::CommandDomain::kRDP;
	setCombine.opcode = 0x3CU;
	setCombine.w0 = (0x3CU << 24) | 0x00543210U;
	setCombine.w1 = 0x0FEDCBA9U;
	engine.apply(setCombine);
	expectEq(engine.snapshot().combineMux, (static_cast<u64>(0x00543210U) << 32) | 0x0FEDCBA9ULL, "SetCombineMode combine mux mismatch");
	expectTrue((engine.snapshot().changedMask & rvk2::rdp_state_changed::kCombine) != 0U, "SetCombineMode changed mask missing");

	rvk2::CommandPacket setScissor{};
	setScissor.id = 3ULL;
	setScissor.domain = rvk2::CommandDomain::kRDP;
	setScissor.opcode = 0x2DU;
	setScissor.w0 = (0x2DU << 24) | (0x123U << 12) | 0x456U;
	setScissor.w1 = (0x789U << 12) | 0xABCU;
	engine.apply(setScissor);
	expectEq(engine.snapshot().scissorXH, static_cast<u16>(0x123U), "SetScissor XH mismatch");
	expectEq(engine.snapshot().scissorYH, static_cast<u16>(0x456U), "SetScissor YH mismatch");
	expectEq(engine.snapshot().scissorXL, static_cast<u16>(0x789U), "SetScissor XL mismatch");
	expectEq(engine.snapshot().scissorYL, static_cast<u16>(0xABCU), "SetScissor YL mismatch");

	rvk2::CommandPacket setColor{};
	setColor.id = 4ULL;
	setColor.domain = rvk2::CommandDomain::kRDP;
	setColor.opcode = 0x3FU;
	setColor.w0 = (0x3FU << 24) | (5U << 21) | (2U << 19) | 0x2ABU;
	setColor.w1 = 0x00123456U;
	engine.apply(setColor);
	expectEq(engine.snapshot().colorImageFormat, static_cast<u8>(5U), "SetColorImage format mismatch");
	expectEq(engine.snapshot().colorImageSize, static_cast<u8>(2U), "SetColorImage size mismatch");
	expectEq(engine.snapshot().colorImageWidth, static_cast<u16>(0x2ACU), "SetColorImage width mismatch");
	expectEq(engine.snapshot().colorImageAddress, 0x00123456U, "SetColorImage address mismatch");

	rvk2::CommandPacket setDepth{};
	setDepth.id = 5ULL;
	setDepth.domain = rvk2::CommandDomain::kRDP;
	setDepth.opcode = 0x3EU;
	setDepth.w0 = (0x3EU << 24);
	setDepth.w1 = 0x00FEDCBAU;
	engine.apply(setDepth);
	expectEq(engine.snapshot().depthImageAddress, 0x00FEDCBAU, "SetDepthImage address mismatch");

	rvk2::CommandPacket loadSync{};
	loadSync.id = 6ULL;
	loadSync.domain = rvk2::CommandDomain::kRDP;
	loadSync.opcode = 0x26U;
	loadSync.w0 = (0x26U << 24);
	loadSync.w1 = 0U;
	engine.apply(loadSync);

	rvk2::CommandPacket pipeSync{};
	pipeSync.id = 7ULL;
	pipeSync.domain = rvk2::CommandDomain::kRDP;
	pipeSync.opcode = 0x27U;
	pipeSync.w0 = (0x27U << 24);
	pipeSync.w1 = 0U;
	engine.apply(pipeSync);

	rvk2::CommandPacket tileSync{};
	tileSync.id = 8ULL;
	tileSync.domain = rvk2::CommandDomain::kRDP;
	tileSync.opcode = 0x28U;
	tileSync.w0 = (0x28U << 24);
	tileSync.w1 = 0U;
	engine.apply(tileSync);

	rvk2::CommandPacket fullSync{};
	fullSync.id = 9ULL;
	fullSync.domain = rvk2::CommandDomain::kRDP;
	fullSync.opcode = 0x29U;
	fullSync.w0 = (0x29U << 24);
	fullSync.w1 = 0U;
	engine.apply(fullSync);

	expectEq(engine.snapshot().syncEpoch, 4U, "sync epoch mismatch");
	expectEq(engine.snapshot().loadSyncCount, 1U, "LoadSync count mismatch");
	expectEq(engine.snapshot().pipeSyncCount, 1U, "PipeSync count mismatch");
	expectEq(engine.snapshot().tileSyncCount, 1U, "TileSync count mismatch");
	expectEq(engine.snapshot().fullSyncCount, 1U, "FullSync count mismatch");
	expectEq(engine.snapshot().loadSyncPacketId, 6ULL, "LoadSync packet id mismatch");
	expectEq(engine.snapshot().pipeSyncPacketId, 7ULL, "PipeSync packet id mismatch");
	expectEq(engine.snapshot().tileSyncPacketId, 8ULL, "TileSync packet id mismatch");
	expectEq(engine.snapshot().fullSyncPacketId, 9ULL, "FullSync packet id mismatch");
	expectEq(engine.snapshot().lastPacketId, 9ULL, "last packet id mismatch after transition sequence");

	const u32 expectedMask =
		rvk2::rdp_state_changed::kOtherModes
		| rvk2::rdp_state_changed::kCombine
		| rvk2::rdp_state_changed::kScissor
		| rvk2::rdp_state_changed::kColorImage
		| rvk2::rdp_state_changed::kDepthImage
		| rvk2::rdp_state_changed::kLoadSync
		| rvk2::rdp_state_changed::kPipeSync
		| rvk2::rdp_state_changed::kTileSync
		| rvk2::rdp_state_changed::kFullSync;
	expectEq(engine.snapshot().changedMask, expectedMask, "changed mask mismatch after transition sequence");
}

void testTMEMStateTransitions()
{
	rvk2::TMEMModel model;
	model.reset();
	expectEq(model.snapshot().changedMask, rvk2::tmem_state_changed::kNone, "TMEM reset changed mask must be zero");
	expectEq(model.snapshot().lastPacketId, 0ULL, "TMEM reset last packet id must be zero");

	rvk2::CommandPacket nonRdp{};
	nonRdp.id = 77ULL;
	nonRdp.domain = rvk2::CommandDomain::kRSP;
	nonRdp.opcode = 0x3DU;
	nonRdp.w0 = 0x3D000000U;
	nonRdp.w1 = 0x11223344U;
	model.apply(nonRdp);
	expectEq(model.snapshot().lastPacketId, 0ULL, "TMEM non-RDP packet must not change state");

	rvk2::CommandPacket setTexture{};
	setTexture.id = 1ULL;
	setTexture.domain = rvk2::CommandDomain::kRDP;
	setTexture.opcode = 0x3DU;
	setTexture.w0 = (0x3DU << 24) | (2U << 21) | (3U << 19) | 0x0155U;
	setTexture.w1 = 0x00ABCDEFU;
	model.apply(setTexture);
	expectEq(model.snapshot().textureImage.format, static_cast<u8>(2U), "SetTextureImage format mismatch");
	expectEq(model.snapshot().textureImage.size, static_cast<u8>(3U), "SetTextureImage size mismatch");
	expectEq(model.snapshot().textureImage.width, static_cast<u16>(0x156U), "SetTextureImage width mismatch");
	expectEq(model.snapshot().textureImage.address, 0x00ABCDEFU, "SetTextureImage address mismatch");

	rvk2::CommandPacket setTile{};
	setTile.id = 2ULL;
	setTile.domain = rvk2::CommandDomain::kRDP;
	setTile.opcode = 0x35U;
	setTile.w0 = (0x35U << 24) | (1U << 21) | (2U << 19) | (0x12AU << 9) | 0x101U;
	setTile.w1 = (3U << 24) | (0xAU << 20) | (2U << 18) | (8U << 14) | (7U << 10) | (1U << 8) | (4U << 4) | 5U;
	model.apply(setTile);
	const rvk2::TileDescriptorState & tile3 = model.snapshot().tiles[3];
	expectEq(tile3.format, static_cast<u8>(1U), "SetTile format mismatch");
	expectEq(tile3.size, static_cast<u8>(2U), "SetTile size mismatch");
	expectEq(tile3.line, static_cast<u16>(0x12AU), "SetTile line mismatch");
	expectEq(tile3.tmem, static_cast<u16>(0x101U), "SetTile tmem mismatch");
	expectEq(tile3.palette, static_cast<u8>(0xAU), "SetTile palette mismatch");
	expectEq(tile3.cmt, static_cast<u8>(2U), "SetTile cmt mismatch");
	expectEq(tile3.cms, static_cast<u8>(1U), "SetTile cms mismatch");
	expectEq(tile3.maskt, static_cast<u8>(8U), "SetTile maskt mismatch");
	expectEq(tile3.masks, static_cast<u8>(4U), "SetTile masks mismatch");
	expectEq(tile3.shiftt, static_cast<u8>(7U), "SetTile shiftt mismatch");
	expectEq(tile3.shifts, static_cast<u8>(5U), "SetTile shifts mismatch");

	rvk2::CommandPacket setTileSize{};
	setTileSize.id = 3ULL;
	setTileSize.domain = rvk2::CommandDomain::kRDP;
	setTileSize.opcode = 0x32U;
	setTileSize.w0 = (0x32U << 24) | (0x155U << 12) | 0x222U;
	setTileSize.w1 = (3U << 24) | (0x2AAU << 12) | 0x377U;
	model.apply(setTileSize);
	expectEq(tile3.uls, static_cast<u16>(0x155U), "SetTileSize uls mismatch");
	expectEq(tile3.ult, static_cast<u16>(0x222U), "SetTileSize ult mismatch");
	expectEq(tile3.lrs, static_cast<u16>(0x2AAU), "SetTileSize lrs mismatch");
	expectEq(tile3.lrt, static_cast<u16>(0x377U), "SetTileSize lrt mismatch");

	rvk2::CommandPacket loadBlock{};
	loadBlock.id = 4ULL;
	loadBlock.domain = rvk2::CommandDomain::kRDP;
	loadBlock.opcode = 0x33U;
	loadBlock.w0 = (0x33U << 24) | (0x111U << 12) | 0x002U;
	loadBlock.w1 = (3U << 24) | (0x2C0U << 12) | 0x321U;
	model.apply(loadBlock);
	expectEq(model.snapshot().lastLoad.kind, rvk2::TmemLoadKind::kBlock, "LoadBlock kind mismatch");
	expectEq(model.snapshot().lastLoad.tile, static_cast<u8>(3U), "LoadBlock tile mismatch");
	expectEq(model.snapshot().lastLoad.uls, static_cast<u16>(0x111U), "LoadBlock uls mismatch");
	expectEq(model.snapshot().lastLoad.ult, static_cast<u16>(0x002U), "LoadBlock ult mismatch");
	expectEq(model.snapshot().lastLoad.lrs, static_cast<u16>(0x2C0U), "LoadBlock lrs mismatch");
	expectEq(model.snapshot().lastLoad.lrt, static_cast<u16>(0U), "LoadBlock lrt must be zero");
	expectEq(model.snapshot().lastLoad.dxt, static_cast<u16>(0x321U), "LoadBlock dxt mismatch");

	rvk2::CommandPacket loadTile{};
	loadTile.id = 5ULL;
	loadTile.domain = rvk2::CommandDomain::kRDP;
	loadTile.opcode = 0x34U;
	loadTile.w0 = (0x34U << 24) | (0x020U << 12) | 0x040U;
	loadTile.w1 = (2U << 24) | (0x080U << 12) | 0x0A0U;
	model.apply(loadTile);
	expectEq(model.snapshot().lastLoad.kind, rvk2::TmemLoadKind::kTile, "LoadTile kind mismatch");
	expectEq(model.snapshot().lastLoad.tile, static_cast<u8>(2U), "LoadTile tile mismatch");
	expectEq(model.snapshot().lastLoad.lrt, static_cast<u16>(0x0A0U), "LoadTile lrt mismatch");
	expectEq(model.snapshot().lastLoad.dxt, static_cast<u16>(0U), "LoadTile dxt must be zero");

	rvk2::CommandPacket loadTLUT{};
	loadTLUT.id = 6ULL;
	loadTLUT.domain = rvk2::CommandDomain::kRDP;
	loadTLUT.opcode = 0x30U;
	loadTLUT.w0 = (0x30U << 24) | (0x011U << 12) | 0x022U;
	loadTLUT.w1 = (5U << 24) | (0x033U << 12) | 0x044U;
	model.apply(loadTLUT);
	expectEq(model.snapshot().lastLoad.kind, rvk2::TmemLoadKind::kTLUT, "LoadTLUT kind mismatch");
	expectEq(model.snapshot().lastLoad.tile, static_cast<u8>(5U), "LoadTLUT tile mismatch");
	expectEq(model.snapshot().lastLoad.lrs, static_cast<u16>(0x033U), "LoadTLUT lrs mismatch");
	expectEq(model.snapshot().lastLoad.lrt, static_cast<u16>(0x044U), "LoadTLUT lrt mismatch");
	expectEq(model.snapshot().lastPacketId, 6ULL, "TMEM last packet id mismatch after transition sequence");

	const u32 expectedMask =
		rvk2::tmem_state_changed::kTextureImage
		| rvk2::tmem_state_changed::kTileDescriptor
		| rvk2::tmem_state_changed::kTileSize
		| rvk2::tmem_state_changed::kLoadTile
		| rvk2::tmem_state_changed::kLoadBlock
		| rvk2::tmem_state_changed::kLoadTLUT;
	expectEq(model.snapshot().changedMask, expectedMask, "TMEM changed mask mismatch after transition sequence");
}

void testRDPExtendedStateFields()
{
	rvk2::RDPStateEngine engine;
	engine.reset();

	const u32 mode0 =
		(0xBU << 0)
		| (2U << 4)
		| (1U << 6)
		| (1U << 8)
		| (1U << 9)
		| (1U << 11)
		| (2U << 12)
		| (1U << 14)
		| (1U << 16)
		| (2U << 17)
		| (1U << 19)
		| (3U << 20)
		| (1U << 22)
		| (1U << 23);
	const u32 mode1 =
		(2U << 0)
		| (1U << 2)
		| (1U << 3)
		| (1U << 4)
		| (1U << 5)
		| (1U << 7)
		| (2U << 8)
		| (3U << 10)
		| (1U << 12)
		| (1U << 14)
		| (1U << 15)
		| (1U << 16)
		| (2U << 18)
		| (3U << 20)
		| (2U << 24)
		| (1U << 26)
		| (3U << 28)
		| (2U << 30);

	rvk2::CommandPacket setOther{};
	setOther.id = 1ULL;
	setOther.domain = rvk2::CommandDomain::kRDP;
	setOther.opcode = 0x2FU;
	setOther.w0 = (0x2FU << 24) | mode0;
	setOther.w1 = mode1;
	engine.apply(setOther);
	expectEq(engine.snapshot().cycleType, static_cast<u8>(3U), "extended SetOtherModes cycle type mismatch");
	expectEq(engine.snapshot().otherModesDecoded.alphaCompare, static_cast<u8>(2U), "other modes alpha compare mismatch");
	expectEq(engine.snapshot().otherModesDecoded.cvgDest, static_cast<u8>(2U), "other modes cvgDest mismatch");
	expectEq(engine.snapshot().otherModesDecoded.blendMask, static_cast<u8>(0xBU), "other modes blend mask mismatch");
	expectEq(engine.snapshot().otherModesDecoded.depthMode, static_cast<u8>(3U), "other modes depthMode mismatch");
	expectTrue(engine.snapshot().otherModesDecoded.depthCompare, "other modes depth compare mismatch");
	expectTrue(engine.snapshot().otherModesDecoded.colorOnCvg, "other modes color-on-coverage mismatch");
	expectTrue(engine.snapshot().otherModesDecoded.cvgXAlpha, "other modes cvg x alpha mismatch");
	expectTrue(!engine.snapshot().otherModesDecoded.alphaCvgSel, "other modes alpha coverage select mismatch");
	expectTrue(engine.snapshot().otherModesDecoded.forceBlender, "other modes force blender mismatch");
	expectEq(engine.snapshot().otherModesDecoded.textureFilter, static_cast<u8>(2U), "other modes texture filter mismatch");
	expectEq(engine.snapshot().otherModesDecoded.textureLUT, static_cast<u8>(1U), "other modes texture LUT mismatch");
	expectTrue(engine.snapshot().otherModesDecoded.texturePersp, "other modes texture perspective mismatch");

	rvk2::CommandPacket setPrimColor{};
	setPrimColor.id = 2ULL;
	setPrimColor.domain = rvk2::CommandDomain::kRDP;
	setPrimColor.opcode = 0x3AU;
	setPrimColor.w0 = (0x3AU << 24) | (0x1FU << 8) | 0xABU;
	setPrimColor.w1 = 0x11223344U;
	engine.apply(setPrimColor);
	expectEq(engine.snapshot().primColorMinLevel, static_cast<u8>(0x1FU), "SetPrimColor min level mismatch");
	expectEq(engine.snapshot().primColorLodFrac, static_cast<u8>(0xABU), "SetPrimColor lod frac mismatch");
	expectEq(engine.snapshot().primColor.r, static_cast<u8>(0x11U), "SetPrimColor r mismatch");
	expectEq(engine.snapshot().primColor.g, static_cast<u8>(0x22U), "SetPrimColor g mismatch");
	expectEq(engine.snapshot().primColor.b, static_cast<u8>(0x33U), "SetPrimColor b mismatch");
	expectEq(engine.snapshot().primColor.a, static_cast<u8>(0x44U), "SetPrimColor a mismatch");

	rvk2::CommandPacket setEnvColor{};
	setEnvColor.id = 3ULL;
	setEnvColor.domain = rvk2::CommandDomain::kRDP;
	setEnvColor.opcode = 0x3BU;
	setEnvColor.w0 = (0x3BU << 24);
	setEnvColor.w1 = 0x55667788U;
	engine.apply(setEnvColor);
	expectEq(engine.snapshot().envColor.r, static_cast<u8>(0x55U), "SetEnvColor r mismatch");
	expectEq(engine.snapshot().envColor.g, static_cast<u8>(0x66U), "SetEnvColor g mismatch");
	expectEq(engine.snapshot().envColor.b, static_cast<u8>(0x77U), "SetEnvColor b mismatch");
	expectEq(engine.snapshot().envColor.a, static_cast<u8>(0x88U), "SetEnvColor a mismatch");

	rvk2::CommandPacket setBlendColor{};
	setBlendColor.id = 4ULL;
	setBlendColor.domain = rvk2::CommandDomain::kRDP;
	setBlendColor.opcode = 0x39U;
	setBlendColor.w0 = (0x39U << 24);
	setBlendColor.w1 = 0x99AABBCCU;
	engine.apply(setBlendColor);
	expectEq(engine.snapshot().blendColor.r, static_cast<u8>(0x99U), "SetBlendColor r mismatch");
	expectEq(engine.snapshot().blendColor.g, static_cast<u8>(0xAAU), "SetBlendColor g mismatch");
	expectEq(engine.snapshot().blendColor.b, static_cast<u8>(0xBBU), "SetBlendColor b mismatch");
	expectEq(engine.snapshot().blendColor.a, static_cast<u8>(0xCCU), "SetBlendColor a mismatch");

	rvk2::CommandPacket setFogColor{};
	setFogColor.id = 5ULL;
	setFogColor.domain = rvk2::CommandDomain::kRDP;
	setFogColor.opcode = 0x38U;
	setFogColor.w0 = (0x38U << 24);
	setFogColor.w1 = 0xDDEEFF11U;
	engine.apply(setFogColor);
	expectEq(engine.snapshot().fogColor.r, static_cast<u8>(0xDDU), "SetFogColor r mismatch");
	expectEq(engine.snapshot().fogColor.g, static_cast<u8>(0xEEU), "SetFogColor g mismatch");
	expectEq(engine.snapshot().fogColor.b, static_cast<u8>(0xFFU), "SetFogColor b mismatch");
	expectEq(engine.snapshot().fogColor.a, static_cast<u8>(0x11U), "SetFogColor a mismatch");

	rvk2::CommandPacket setFillColor{};
	setFillColor.id = 6ULL;
	setFillColor.domain = rvk2::CommandDomain::kRDP;
	setFillColor.opcode = 0x37U;
	setFillColor.w0 = (0x37U << 24);
	setFillColor.w1 = 0xA1B2C3D4U;
	engine.apply(setFillColor);
	expectEq(engine.snapshot().fillColor, 0xA1B2C3D4U, "SetFillColor value mismatch");

	rvk2::CommandPacket setPrimDepth{};
	setPrimDepth.id = 7ULL;
	setPrimDepth.domain = rvk2::CommandDomain::kRDP;
	setPrimDepth.opcode = 0x2EU;
	setPrimDepth.w0 = (0x2EU << 24);
	setPrimDepth.w1 = 0x1234FEDCU;
	engine.apply(setPrimDepth);
	expectEq(engine.snapshot().primDepthZ, static_cast<u16>(0x1234U), "SetPrimDepth z mismatch");
	expectEq(engine.snapshot().primDepthDelta, static_cast<u16>(0xFEDCU), "SetPrimDepth dz mismatch");

	const u32 k0Raw = 0x101U;
	const u32 k1Raw = 0x1FFU;
	const u32 k2Raw = 0x12AU;
	const u32 k3Raw = 0x055U;
	const u32 k4Raw = 0x111U;
	const u32 k5Raw = 0x022U;
	rvk2::CommandPacket setConvert{};
	setConvert.id = 8ULL;
	setConvert.domain = rvk2::CommandDomain::kRDP;
	setConvert.opcode = 0x2CU;
	setConvert.w0 = (0x2CU << 24) | (k0Raw << 13) | (k1Raw << 4) | (k2Raw >> 5);
	setConvert.w1 = ((k2Raw & 0x1FU) << 27) | (k3Raw << 18) | (k4Raw << 9) | k5Raw;
	engine.apply(setConvert);
	expectEq(engine.snapshot().convertK0, static_cast<s16>(-255), "SetConvert k0 mismatch");
	expectEq(engine.snapshot().convertK1, static_cast<s16>(-1), "SetConvert k1 mismatch");
	expectEq(engine.snapshot().convertK2, static_cast<s16>(-214), "SetConvert k2 mismatch");
	expectEq(engine.snapshot().convertK3, static_cast<s16>(0x55), "SetConvert k3 mismatch");
	expectEq(engine.snapshot().convertK4, static_cast<s16>(0x111), "SetConvert k4 mismatch");
	expectEq(engine.snapshot().convertK5, static_cast<s16>(0x22), "SetConvert k5 mismatch");

	rvk2::CommandPacket setKeyR{};
	setKeyR.id = 9ULL;
	setKeyR.domain = rvk2::CommandDomain::kRDP;
	setKeyR.opcode = 0x2BU;
	setKeyR.w0 = (0x2BU << 24);
	setKeyR.w1 = (0xABCU << 16) | (0x12U << 8) | 0x34U;
	engine.apply(setKeyR);
	expectEq(engine.snapshot().keyCenterR, static_cast<u8>(0x12U), "SetKeyR center mismatch");
	expectEq(engine.snapshot().keyScaleR, static_cast<u8>(0x34U), "SetKeyR scale mismatch");
	expectEq(engine.snapshot().keyWidthR, static_cast<u16>(0xABCU), "SetKeyR width mismatch");

	rvk2::CommandPacket setKeyGB{};
	setKeyGB.id = 10ULL;
	setKeyGB.domain = rvk2::CommandDomain::kRDP;
	setKeyGB.opcode = 0x2AU;
	setKeyGB.w0 = (0x2AU << 24) | (0x345U << 12) | 0x678U;
	setKeyGB.w1 = (0x56U << 24) | (0x78U << 16) | (0x9AU << 8) | 0xBCU;
	engine.apply(setKeyGB);
	expectEq(engine.snapshot().keyCenterG, static_cast<u8>(0x56U), "SetKeyGB center G mismatch");
	expectEq(engine.snapshot().keyScaleG, static_cast<u8>(0x78U), "SetKeyGB scale G mismatch");
	expectEq(engine.snapshot().keyWidthG, static_cast<u16>(0x345U), "SetKeyGB width G mismatch");
	expectEq(engine.snapshot().keyCenterB, static_cast<u8>(0x9AU), "SetKeyGB center B mismatch");
	expectEq(engine.snapshot().keyScaleB, static_cast<u8>(0xBCU), "SetKeyGB scale B mismatch");
	expectEq(engine.snapshot().keyWidthB, static_cast<u16>(0x678U), "SetKeyGB width B mismatch");

	rvk2::CommandPacket setScissor{};
	setScissor.id = 11ULL;
	setScissor.domain = rvk2::CommandDomain::kRDP;
	setScissor.opcode = 0x2DU;
	setScissor.w0 = (0x2DU << 24) | (0x111U << 12) | 0x222U;
	setScissor.w1 = (2U << 24) | (0x333U << 12) | 0x444U;
	engine.apply(setScissor);
	expectEq(engine.snapshot().scissorMode, static_cast<u8>(2U), "SetScissor mode mismatch");

	const u32 expectedMask =
		rvk2::rdp_state_changed::kOtherModes
		| rvk2::rdp_state_changed::kPrimColor
		| rvk2::rdp_state_changed::kEnvColor
		| rvk2::rdp_state_changed::kBlendColor
		| rvk2::rdp_state_changed::kFogColor
		| rvk2::rdp_state_changed::kFillColor
		| rvk2::rdp_state_changed::kPrimDepth
		| rvk2::rdp_state_changed::kConvert
		| rvk2::rdp_state_changed::kKeyR
		| rvk2::rdp_state_changed::kKeyGB
		| rvk2::rdp_state_changed::kScissor;
	expectEq(engine.snapshot().changedMask, expectedMask, "extended changed mask mismatch");
	expectEq(engine.snapshot().lastPacketId, 11ULL, "extended last packet id mismatch");
}

void testRuntimeDrawSemanticCapture()
{
	rvk2::Runtime runtime;
	runtime.beginFrame(17ULL);

	rvk2::CommandProvenance provenance{};
	provenance.taskId = 17U;
	provenance.microcode = 20U;

	runtime.submitRDPWord(
		0x00100000U,
		(0x2FU << 24) | (1U << 20) | 0x000111U,
		0x12345678U,
		provenance);
	runtime.submitRDPWord(
		0x00100008U,
		(0x3DU << 24) | (2U << 21) | (2U << 19) | 0x0080U,
		0x000A0000U,
		provenance);
	runtime.submitRDPWord(
		0x00100010U,
		(0x35U << 24) | (2U << 21) | (2U << 19) | (4U << 9) | 0x20U,
		(2U << 24),
		provenance);
	runtime.submitRDPWord(
		0x00100018U,
		(0x26U << 24),
		0U,
		provenance);

	runtime.submitRDPWord(
		0x00100020U,
		(0x0FU << 24) | (1U << 23) | (5U << 19) | (2U << 16) | 0x0234U,
		(0x345U << 16) | 0x0123U,
		provenance,
		6U,
		(0x001U << 16) | 0x2345U,
		(0x002U << 16) | 0x3456U,
		(0x003U << 16) | 0x4567U,
		(0x004U << 16) | 0x5678U,
		(0x005U << 16) | 0x6789U,
		(0x006U << 16) | 0x789AU,
		8U);
	runtime.submitRDPWord(
		0x00100028U,
		(0x36U << 24) | 0x00000011U,
		0x00000022U,
		provenance);

	const std::vector<rvk2::DrawSemanticPacket> & semantics = runtime.drawSemantics();
	expectEq(semantics.size(), static_cast<size_t>(2U), "Runtime draw semantic capture count mismatch");

	const rvk2::DrawSemanticPacket & tri = semantics[0];
	expectEq(tri.sourcePacketId, 5ULL, "Draw semantic tri packet id mismatch");
	expectEq(tri.sourceOpcode, static_cast<u8>(0x0FU), "Draw semantic tri opcode mismatch");
	expectEq(tri.drawType, static_cast<u8>(1U), "Draw semantic tri type mismatch");
	expectEq(tri.tile, static_cast<u8>(2U), "Draw semantic tri tile mismatch");
	expectEq(tri.cycleType, static_cast<u8>(1U), "Draw semantic tri cycle type mismatch");
	expectEq(tri.combineMux, runtime.rdpState().snapshot().combineMux, "Draw semantic tri combine mux mismatch");
	expectTrue(tri.triangleLMajor, "Draw semantic tri lmajor mismatch");
	expectEq(tri.triangleLevel, static_cast<u8>(5U), "Draw semantic tri level mismatch");
	expectEq(tri.triangleYL, static_cast<u16>(0x0234U), "Draw semantic tri YL mismatch");
	expectEq(tri.triangleYM, static_cast<u16>(0x0345U), "Draw semantic tri YM mismatch");
	expectEq(tri.triangleYH, static_cast<u16>(0x0123U), "Draw semantic tri YH mismatch");
	expectEq(tri.triangleXL, static_cast<s32>(0x00012345U), "Draw semantic tri XL mismatch");
	expectEq(tri.triangleXH, static_cast<s32>(0x00034567U), "Draw semantic tri XH mismatch");
	expectEq(tri.triangleXM, static_cast<s32>(0x00056789U), "Draw semantic tri XM mismatch");
	expectEq(tri.triangleDxLDY, static_cast<s32>(0x00023456U), "Draw semantic tri DxLDY mismatch");
	expectEq(tri.triangleDxHDY, static_cast<s32>(0x00045678U), "Draw semantic tri DxHDY mismatch");
	expectEq(tri.triangleDxMDY, static_cast<s32>(0x0006789AU), "Draw semantic tri DxMDY mismatch");
	expectTrue(tri.textured, "Draw semantic tri should be textured");
	expectTrue(tri.depthTest, "Draw semantic tri should be depth-tested");
	expectEq(tri.syncEpoch, 1U, "Draw semantic tri sync epoch mismatch");
	expectEq(tri.loadSyncPacketId, 4ULL, "Draw semantic tri load sync packet mismatch");

	const rvk2::DrawSemanticPacket & fill = semantics[1];
	expectEq(fill.sourcePacketId, 6ULL, "Draw semantic fill packet id mismatch");
	expectEq(fill.sourceOpcode, static_cast<u8>(0x36U), "Draw semantic fill opcode mismatch");
	expectEq(fill.drawType, static_cast<u8>(3U), "Draw semantic fill type mismatch");
	expectTrue(!fill.textured, "Draw semantic fill should not be textured");
	expectTrue(!fill.depthTest, "Draw semantic fill should not be depth-tested");
	expectEq(fill.syncEpoch, 1U, "Draw semantic fill sync epoch mismatch");
	expectEq(fill.rectULX, static_cast<u16>(0U), "Draw semantic fill ULX mismatch");
	expectEq(fill.rectULY, static_cast<u16>(0x22U), "Draw semantic fill ULY mismatch");
	expectEq(fill.rectLRX, static_cast<u16>(0U), "Draw semantic fill LRX mismatch");
	expectEq(fill.rectLRY, static_cast<u16>(0x11U), "Draw semantic fill LRY mismatch");

	const std::vector<rvk2::RasterOpPacket> & rasterOps = runtime.rasterOps();
	expectEq(rasterOps.size(), static_cast<size_t>(2U), "Runtime raster op capture count mismatch");
	expectEq(
		rasterOps[0].opKind,
		static_cast<u8>(rvk2::RasterOpKind::kTriangle),
		"Runtime raster op triangle kind mismatch");
	expectEq(
		rasterOps[1].opKind,
		static_cast<u8>(rvk2::RasterOpKind::kFillRect),
		"Runtime raster op fill kind mismatch");

	const std::vector<rvk2::RenderWorkPacket> & renderPlan = runtime.renderPlan();
	expectEq(renderPlan.size(), static_cast<size_t>(2U), "Runtime render plan count mismatch");
	expectEq(
		renderPlan[0].phase,
		static_cast<u8>(rvk2::RenderPhase::kCycle2),
		"Runtime render plan triangle phase mismatch");
	expectEq(
		renderPlan[1].phase,
		static_cast<u8>(rvk2::RenderPhase::kFill),
		"Runtime render plan fill phase mismatch");
	expectEq(renderPlan[0].triangleYL, tri.triangleYL, "Runtime render plan tri YL mismatch");
	expectEq(renderPlan[0].triangleYM, tri.triangleYM, "Runtime render plan tri YM mismatch");
	expectEq(renderPlan[0].triangleYH, tri.triangleYH, "Runtime render plan tri YH mismatch");
	expectEq(renderPlan[0].triangleXL, tri.triangleXL, "Runtime render plan tri XL mismatch");
	expectEq(renderPlan[0].triangleXH, tri.triangleXH, "Runtime render plan tri XH mismatch");
	expectEq(renderPlan[0].triangleXM, tri.triangleXM, "Runtime render plan tri XM mismatch");
	expectEq(renderPlan[0].triangleDxLDY, tri.triangleDxLDY, "Runtime render plan tri DxLDY mismatch");
	expectEq(renderPlan[0].triangleDxHDY, tri.triangleDxHDY, "Runtime render plan tri DxHDY mismatch");
	expectEq(renderPlan[0].triangleDxMDY, tri.triangleDxMDY, "Runtime render plan tri DxMDY mismatch");
	expectTrue(
		renderPlan[0].rectLRX >= renderPlan[0].rectULX,
		"Runtime render plan tri rect X ordering mismatch");
	expectTrue(
		renderPlan[0].rectLRY >= renderPlan[0].rectULY,
		"Runtime render plan tri rect Y ordering mismatch");
	expectEq(renderPlan[1].rectLRY, fill.rectLRY, "Runtime render plan fill LRY mismatch");
	expectEq(
		renderPlan[0].barrierMask,
		static_cast<u8>(rvk2::render_barrier::kLoadSync),
		"Runtime render plan first barrier mask mismatch");
	expectEq(
		renderPlan[1].barrierMask,
		static_cast<u8>(rvk2::render_barrier::kNone),
		"Runtime render plan second barrier mask mismatch");
	expectEq(renderPlan[0].loadSyncPacketId, 4ULL, "Runtime render plan sync packet mismatch");

	const std::vector<rvk2::SubmissionBatchPacket> & submitPlan = runtime.submissionPlan();
	expectEq(submitPlan.size(), static_cast<size_t>(2U), "Runtime submission plan count mismatch");
	expectEq(submitPlan[0].batchIndex, 0U, "Runtime submission plan first batch index mismatch");
	expectEq(submitPlan[0].splitReason, static_cast<u8>(rvk2::SubmissionSplitReason::kStart), "Runtime submission plan first split reason mismatch");
	expectEq(submitPlan[0].splitBarrierMask, static_cast<u8>(rvk2::render_barrier::kLoadSync), "Runtime submission plan first split barrier mismatch");
	expectEq(submitPlan[0].workCount, 1U, "Runtime submission plan first work count mismatch");
	expectEq(submitPlan[0].barrierMaskUnion, static_cast<u8>(rvk2::render_barrier::kLoadSync), "Runtime submission plan first barrier union mismatch");
	expectEq(submitPlan[0].texturedWorkCount, 1U, "Runtime submission plan first textured count mismatch");
	expectEq(submitPlan[0].depthTestWorkCount, 1U, "Runtime submission plan first depth count mismatch");
	expectEq(submitPlan[1].splitReason, static_cast<u8>(rvk2::SubmissionSplitReason::kPhaseChange), "Runtime submission plan second split reason mismatch");
	expectEq(submitPlan[1].splitBarrierMask, static_cast<u8>(rvk2::render_barrier::kNone), "Runtime submission plan second split barrier mismatch");
	expectEq(submitPlan[1].workCount, 1U, "Runtime submission plan second work count mismatch");
	expectEq(submitPlan[1].texturedWorkCount, 0U, "Runtime submission plan second textured count mismatch");
	expectEq(submitPlan[1].depthTestWorkCount, 0U, "Runtime submission plan second depth count mismatch");

	const rvk2::FrameTraceRecord traceA = runtime.buildFrameTrace();
	expectEq(traceA.drawSemanticCount, 2ULL, "Frame trace draw semantic count mismatch");
	expectEq(traceA.rasterOpCount, 2ULL, "Frame trace raster op count mismatch");
	expectEq(traceA.renderWorkCount, 2ULL, "Frame trace render plan count mismatch");
	expectEq(traceA.submissionBatchCount, 2ULL, "Frame trace submission batch count mismatch");
	expectEq(traceA.executorWorkCount, 2ULL, "Frame trace executor work count mismatch");
	expectEq(traceA.executorBatchCount, 2ULL, "Frame trace executor batch count mismatch");
	expectTrue(traceA.executorColorWriteCount > 0ULL, "Frame trace executor color write count should be positive");
	expectEq(traceA.executorPresentAspectX, static_cast<u8>(4U), "Frame trace executor aspect x mismatch");
	expectEq(traceA.executorPresentAspectY, static_cast<u8>(3U), "Frame trace executor aspect y mismatch");
	expectEq(traceA.unknownRdpOpcodeCount, 0U, "Frame trace unknown opcode count mismatch");
	expectEq(traceA.truncatedPayloadCount, 0U, "Frame trace truncated payload count mismatch");
	expectTrue(
		traceA.drawSemanticHash != 1469598103934665603ULL,
		"Frame trace draw semantic hash should change from offset basis");
	expectTrue(
		traceA.rasterOpHash != 1469598103934665603ULL,
		"Frame trace raster op hash should change from offset basis");
	expectTrue(
		traceA.renderWorkHash != 1469598103934665603ULL,
		"Frame trace render plan hash should change from offset basis");
	expectTrue(
		traceA.submissionBatchHash != 1469598103934665603ULL,
		"Frame trace submission batch hash should change from offset basis");
	expectTrue(
		traceA.executorPresentHash != 1469598103934665603ULL,
		"Frame trace executor present hash should change from offset basis");

	rvk2::Runtime runtimeSame;
	runtimeSame.beginFrame(17ULL);
	runtimeSame.submitRDPWord(0x00100000U, (0x2FU << 24) | (1U << 20) | 0x000111U, 0x12345678U, provenance);
	runtimeSame.submitRDPWord(0x00100008U, (0x3DU << 24) | (2U << 21) | (2U << 19) | 0x0080U, 0x000A0000U, provenance);
	runtimeSame.submitRDPWord(0x00100010U, (0x35U << 24) | (2U << 21) | (2U << 19) | (4U << 9) | 0x20U, (2U << 24), provenance);
	runtimeSame.submitRDPWord(0x00100018U, (0x26U << 24), 0U, provenance);
	runtimeSame.submitRDPWord(
		0x00100020U,
		(0x0FU << 24) | (1U << 23) | (5U << 19) | (2U << 16) | 0x0234U,
		(0x345U << 16) | 0x0123U,
		provenance,
		6U,
		(0x001U << 16) | 0x2345U,
		(0x002U << 16) | 0x3456U,
		(0x003U << 16) | 0x4567U,
		(0x004U << 16) | 0x5678U,
		(0x005U << 16) | 0x6789U,
		(0x006U << 16) | 0x789AU,
		8U);
	runtimeSame.submitRDPWord(0x00100028U, (0x36U << 24) | 0x00000011U, 0x00000022U, provenance);
	const rvk2::FrameTraceRecord traceSame = runtimeSame.buildFrameTrace();
	expectEq(traceA.drawSemanticHash, traceSame.drawSemanticHash, "Frame trace draw semantic hash must be deterministic");
	expectEq(traceA.rasterOpHash, traceSame.rasterOpHash, "Frame trace raster op hash must be deterministic");
	expectEq(traceA.renderWorkHash, traceSame.renderWorkHash, "Frame trace render plan hash must be deterministic");
	expectEq(traceA.submissionBatchHash, traceSame.submissionBatchHash, "Frame trace submission batch hash must be deterministic");
	expectEq(traceA.executorPresentHash, traceSame.executorPresentHash, "Frame trace executor present hash must be deterministic");

	rvk2::Runtime runtimeDifferent;
	runtimeDifferent.beginFrame(17ULL);
	runtimeDifferent.submitRDPWord(0x00100000U, (0x2FU << 24) | (1U << 20) | 0x000111U, 0x12345678U, provenance);
	runtimeDifferent.submitRDPWord(0x00100008U, (0x3DU << 24) | (2U << 21) | (2U << 19) | 0x0080U, 0x000A0000U, provenance);
	runtimeDifferent.submitRDPWord(0x00100010U, (0x35U << 24) | (2U << 21) | (2U << 19) | (4U << 9) | 0x20U, (2U << 24), provenance);
	runtimeDifferent.submitRDPWord(0x00100018U, (0x26U << 24), 0U, provenance);
	runtimeDifferent.submitRDPWord(
		0x00100020U,
		(0x0FU << 24) | (1U << 23) | (5U << 19) | (2U << 16) | 0x0234U,
		(0x345U << 16) | 0x0123U,
		provenance,
		6U,
		(0x001U << 16) | 0x2345U,
		(0x002U << 16) | 0x3456U,
		(0x003U << 16) | 0x4567U,
		(0x004U << 16) | 0x5678U,
		(0x005U << 16) | 0x6789U,
		(0x006U << 16) | 0x789AU,
		8U);
	const rvk2::FrameTraceRecord traceDifferent = runtimeDifferent.buildFrameTrace();
	expectTrue(
		traceA.drawSemanticHash != traceDifferent.drawSemanticHash,
		"Frame trace draw semantic hash should change when semantic stream changes");
	expectTrue(
		traceA.rasterOpHash != traceDifferent.rasterOpHash,
		"Frame trace raster op hash should change when raster stream changes");
	expectTrue(
		traceA.renderWorkHash != traceDifferent.renderWorkHash,
		"Frame trace render plan hash should change when render stream changes");
	expectTrue(
		traceA.submissionBatchHash != traceDifferent.submissionBatchHash,
		"Frame trace submission batch hash should change when submission stream changes");

	rvk2::Runtime runtimeUnknown;
	runtimeUnknown.beginFrame(17ULL);
	runtimeUnknown.submitRDPWord(0x00100000U, (0x31U << 24), 0U, provenance);
	const rvk2::FrameTraceRecord unknownTrace = runtimeUnknown.buildFrameTrace();
	expectEq(unknownTrace.unknownRdpOpcodeCount, 1U, "Unknown opcode count should increment");
	expectEq(unknownTrace.firstUnknownRdpPacketId, 1ULL, "Unknown opcode first packet id mismatch");
	expectEq(unknownTrace.firstUnknownRdpOpcode, static_cast<u8>(0x31U), "Unknown opcode value mismatch");
	expectEq(unknownTrace.truncatedPayloadCount, 0U, "Unknown trace truncated payload count mismatch");

	rvk2::Runtime runtimeTruncated;
	runtimeTruncated.beginFrame(17ULL);
	runtimeTruncated.submitRDPWord(
		0x00100000U,
		(0x0FU << 24),
		0U,
		provenance,
		2U,
		0x11111111U,
		0x22222222U,
		0U,
		0U,
		0U,
		0U,
		10U);
	const rvk2::FrameTraceRecord truncatedTrace = runtimeTruncated.buildFrameTrace();
	expectEq(truncatedTrace.truncatedPayloadCount, 1U, "Truncated payload count should increment");
	expectEq(
		truncatedTrace.firstTruncatedPayloadPacketId,
		1ULL,
		"Truncated payload first packet id mismatch");
	expectEq(
		truncatedTrace.firstTruncatedPayloadOpcode,
		static_cast<u8>(0x0FU),
		"Truncated payload opcode mismatch");
}

void testSyntheticTrianglePacking()
{
	SPVertex v0{};
	SPVertex v1{};
	SPVertex v2{};
	v0.x = 1.0f;
	v0.y = 1.0f;
	v1.x = 0.0f;
	v1.y = 2.0f;
	v2.x = 0.0f;
	v2.y = 0.0f;

	rvk2::synthetic_triangle::TriangleWords words{};
	const bool built = rvk2::synthetic_triangle::buildWords(
		v0,
		v1,
		v2,
		0x0FU,
		2U,
		5U,
		words);
	expectTrue(built, "Synthetic triangle words should build for non-degenerate triangle");
	expectEq(words.w0, 0x0FAA0008U, "Synthetic triangle w0 mismatch");
	expectEq(words.w1, 0x00040000U, "Synthetic triangle w1 mismatch");
	expectEq(words.w2, 0x00010000U, "Synthetic triangle w2 mismatch");
	expectEq(words.w3, 0x3FFFC000U, "Synthetic triangle w3 mismatch");
	expectEq(words.w4, 0x00000000U, "Synthetic triangle w4 mismatch");
	expectEq(words.w5, 0x00000000U, "Synthetic triangle w5 mismatch");
	expectEq(words.w6, 0x00000000U, "Synthetic triangle w6 mismatch");
	expectEq(words.w7, 0x00004000U, "Synthetic triangle w7 mismatch");

	rvk2::Runtime runtime;
	runtime.beginFrame(21ULL);
	rvk2::CommandProvenance provenance{};
	provenance.taskId = 21U;
	provenance.microcode = 20U;
	runtime.submitRDPWord(
		0x00300000U,
		words.w0,
		words.w1,
		provenance,
		6U,
		words.w2,
		words.w3,
		words.w4,
		words.w5,
		words.w6,
		words.w7,
		8U);

	const std::vector<rvk2::DrawSemanticPacket> & semantics = runtime.drawSemantics();
	expectEq(semantics.size(), static_cast<size_t>(1U), "Synthetic triangle semantic count mismatch");
	const rvk2::DrawSemanticPacket & semantic = semantics[0];
	expectEq(semantic.sourceOpcode, static_cast<u8>(0x0FU), "Synthetic triangle semantic opcode mismatch");
	expectEq(semantic.drawType, static_cast<u8>(1U), "Synthetic triangle semantic draw type mismatch");
	expectEq(semantic.tile, static_cast<u8>(2U), "Synthetic triangle semantic tile mismatch");
	expectTrue(semantic.triangleLMajor, "Synthetic triangle semantic lmajor mismatch");
	expectEq(semantic.triangleLevel, static_cast<u8>(5U), "Synthetic triangle semantic level mismatch");
	expectEq(semantic.triangleYL, static_cast<u16>(8U), "Synthetic triangle semantic YL mismatch");
	expectEq(semantic.triangleYM, static_cast<u16>(4U), "Synthetic triangle semantic YM mismatch");
	expectEq(semantic.triangleYH, static_cast<u16>(0U), "Synthetic triangle semantic YH mismatch");
	expectEq(semantic.triangleXL, static_cast<s32>(0x00010000U), "Synthetic triangle semantic XL mismatch");
	expectEq(semantic.triangleXH, static_cast<s32>(0x00000000U), "Synthetic triangle semantic XH mismatch");
	expectEq(semantic.triangleXM, static_cast<s32>(0x00000000U), "Synthetic triangle semantic XM mismatch");
	expectEq(semantic.triangleDxLDY, static_cast<s32>(0xFFFFC000U), "Synthetic triangle semantic DxLDY mismatch");
	expectEq(semantic.triangleDxHDY, static_cast<s32>(0x00000000U), "Synthetic triangle semantic DxHDY mismatch");
	expectEq(semantic.triangleDxMDY, static_cast<s32>(0x00004000U), "Synthetic triangle semantic DxMDY mismatch");
}

void testTexRectSemanticExtraction()
{
	rvk2::Runtime runtime;
	runtime.beginFrame(3ULL);
	rvk2::CommandProvenance provenance{};
	provenance.taskId = 3U;
	provenance.microcode = 20U;

	runtime.submitRDPWord(
		0x2000U,
		(0x2FU << 24) | (1U << 20),
		(1U << 4),
		provenance);
	runtime.submitRDPWord(
		0x2008U,
		(0x3DU << 24) | (2U << 21) | (2U << 19) | 0x0040U,
		0x00090000U,
		provenance);
	runtime.submitRDPWord(
		0x2010U,
		(0x35U << 24) | (2U << 21) | (2U << 19) | (3U << 9) | 0x10U,
		(4U << 24),
		provenance);
	runtime.submitRDPWord(
		0x2018U,
		(0x24U << 24) | (0x120U << 12) | 0x140U,
		(4U << 24) | (0x080U << 12) | 0x0A0U,
		provenance,
		2U,
		(0x1234U << 16) | 0x5678U,
		(0xFF10U << 16) | 0x0080U);

	const std::vector<rvk2::DrawSemanticPacket> & semantics = runtime.drawSemantics();
	expectEq(semantics.size(), static_cast<size_t>(1U), "TexRect semantic count mismatch");
	const rvk2::DrawSemanticPacket & texrect = semantics[0];
	expectEq(texrect.drawType, static_cast<u8>(2U), "TexRect semantic draw type mismatch");
	expectEq(texrect.sourceOpcode, static_cast<u8>(0x24U), "TexRect semantic opcode mismatch");
	expectEq(texrect.tile, static_cast<u8>(4U), "TexRect semantic tile mismatch");
	expectTrue(!texrect.texRectFlip, "TexRect semantic flip mismatch");
	expectEq(texrect.rectULX, static_cast<u16>(0x080U), "TexRect semantic ULX mismatch");
	expectEq(texrect.rectULY, static_cast<u16>(0x0A0U), "TexRect semantic ULY mismatch");
	expectEq(texrect.rectLRX, static_cast<u16>(0x120U), "TexRect semantic LRX mismatch");
	expectEq(texrect.rectLRY, static_cast<u16>(0x140U), "TexRect semantic LRY mismatch");
	expectEq(texrect.texS, static_cast<s16>(0x1234), "TexRect semantic S mismatch");
	expectEq(texrect.texT, static_cast<s16>(0x5678), "TexRect semantic T mismatch");
	expectEq(texrect.texDSDX, static_cast<s16>(0xFF10), "TexRect semantic DSDX mismatch");
	expectEq(texrect.texDTDY, static_cast<s16>(0x0080), "TexRect semantic DTDY mismatch");
}

void testVIRendererAspectScaling()
{
	std::vector<u32> pixels43{
		0x10203040U, 0x50607080U, 0x90A0B0C0U, 0xD0E0F000U,
		0x00112233U, 0x44556677U, 0x8899AABBU, 0xCCDDEEFFU,
		0x13579BDFU, 0x2468ACE0U, 0x0BADF00DU, 0xC001D00DU
	};

	rvk2::VIRendererConfig config43{};
	config43.aspectX = 4U;
	config43.aspectY = 3U;
	rvk2::VIRenderer renderer43(config43);
	rvk2::VIFrameInput input43{};
	input43.sourceWidth = 4U;
	input43.sourceHeight = 3U;
	input43.sourcePixels = &pixels43;
	const rvk2::VIFrameSummary summary43 = renderer43.present(input43);
	expectEq(summary43.presentWidth, 4U, "VIRenderer 4:3 width mismatch");
	expectEq(summary43.presentHeight, 3U, "VIRenderer 4:3 height mismatch");
	expectEq(summary43.contentX, 0U, "VIRenderer 4:3 contentX mismatch");
	expectEq(summary43.contentY, 0U, "VIRenderer 4:3 contentY mismatch");
	expectEq(summary43.contentWidth, 4U, "VIRenderer 4:3 content width mismatch");
	expectEq(summary43.contentHeight, 3U, "VIRenderer 4:3 content height mismatch");
	expectTrue(summary43.presentHash != 1469598103934665603ULL, "VIRenderer 4:3 hash should not be offset basis");

	rvk2::VIRendererConfig config169{};
	config169.aspectX = 16U;
	config169.aspectY = 9U;
	rvk2::VIRenderer renderer169(config169);
	const rvk2::VIFrameSummary summary169 = renderer169.present(input43);
	expectEq(summary169.presentWidth, 6U, "VIRenderer 16:9 width mismatch");
	expectEq(summary169.presentHeight, 3U, "VIRenderer 16:9 height mismatch");
	expectEq(summary169.contentX, 1U, "VIRenderer 16:9 contentX mismatch");
	expectEq(summary169.contentY, 0U, "VIRenderer 16:9 contentY mismatch");
	expectEq(summary169.contentWidth, 4U, "VIRenderer 16:9 content width mismatch");
	expectEq(summary169.contentHeight, 3U, "VIRenderer 16:9 content height mismatch");
	expectTrue(summary169.presentHash != summary43.presentHash, "VIRenderer 16:9 hash should differ from 4:3");

	std::vector<u32> pixels169(32U, 0xA0B0C0D0U);
	rvk2::VIFrameInput input169{};
	input169.sourceWidth = 8U;
	input169.sourceHeight = 4U;
	input169.sourcePixels = &pixels169;
	const rvk2::VIFrameSummary summaryCrop = renderer43.present(input169);
	expectEq(summaryCrop.presentWidth, 8U, "VIRenderer 16:9->4:3 width mismatch");
	expectEq(summaryCrop.presentHeight, 6U, "VIRenderer 16:9->4:3 height mismatch");
	expectEq(summaryCrop.contentX, 0U, "VIRenderer 16:9->4:3 contentX mismatch");
	expectEq(summaryCrop.contentY, 1U, "VIRenderer 16:9->4:3 contentY mismatch");
	expectEq(summaryCrop.contentWidth, 8U, "VIRenderer 16:9->4:3 content width mismatch");
	expectEq(summaryCrop.contentHeight, 4U, "VIRenderer 16:9->4:3 content height mismatch");

	std::vector<u32> registerPixels{
		0x000000FFU, 0x010101FFU, 0x020202FFU, 0x030303FFU,
		0x101010FFU, 0x111111FFU, 0x121212FFU, 0x131313FFU,
		0x202020FFU, 0x212121FFU, 0x222222FFU, 0x232323FFU,
		0x303030FFU, 0x313131FFU, 0x323232FFU, 0x333333FFU
	};

	rvk2::VIRendererConfig configSquare{};
	configSquare.aspectX = 1U;
	configSquare.aspectY = 1U;
	rvk2::VIRenderer rendererSquare(configSquare);
	rvk2::VIFrameInput registerInput{};
	registerInput.sourceWidth = 4U;
	registerInput.sourceHeight = 4U;
	registerInput.sourcePixels = &registerPixels;
	registerInput.registers.valid = true;
	registerInput.registers.status = 3U;
	registerInput.registers.width = 4U;
	registerInput.registers.vSync = 525U;
	registerInput.registers.hStart = (0U << 16U) | 2U;
	registerInput.registers.vStart = (0U << 16U) | 4U;
	registerInput.registers.xScale = 2048U;
	registerInput.registers.yScale = 2048U;
	std::vector<u32> sampledPixels;
	const rvk2::VIFrameSummary registerSummary =
		rendererSquare.present(registerInput, &sampledPixels);
	expectEq(registerSummary.presentWidth, 2U, "VIRenderer register width mismatch");
	expectEq(registerSummary.presentHeight, 2U, "VIRenderer register height mismatch");
	expectEq(registerSummary.contentWidth, 2U, "VIRenderer register content width mismatch");
	expectEq(registerSummary.contentHeight, 2U, "VIRenderer register content height mismatch");
	expectEq(sampledPixels.size(), static_cast<size_t>(4U), "VIRenderer register sampled pixel count mismatch");
	expectEq(sampledPixels[0], registerPixels[0], "VIRenderer register sample (0,0) mismatch");
	expectEq(sampledPixels[1], registerPixels[2], "VIRenderer register sample (1,0) mismatch");
	expectEq(sampledPixels[2], registerPixels[8], "VIRenderer register sample (0,1) mismatch");
	expectEq(sampledPixels[3], registerPixels[10], "VIRenderer register sample (1,1) mismatch");

	registerInput.registers.status = 3U;
	std::vector<u32> noGammaPixels;
	const rvk2::VIFrameSummary noGammaSummary =
		rendererSquare.present(registerInput, &noGammaPixels);
	registerInput.registers.status = 3U | 0x000008U;
	std::vector<u32> gammaPixels;
	const rvk2::VIFrameSummary gammaSummary =
		rendererSquare.present(registerInput, &gammaPixels);
	expectTrue(
		noGammaSummary.presentHash != gammaSummary.presentHash,
		"VIRenderer gamma flag should alter present hash");
	bool gammaPixelsDiffer = false;
	if (noGammaPixels.size() == gammaPixels.size()) {
		for (size_t i = 0; i < noGammaPixels.size(); ++i) {
			if (noGammaPixels[i] != gammaPixels[i]) {
				gammaPixelsDiffer = true;
				break;
			}
		}
	}
	expectTrue(
		gammaPixelsDiffer,
		"VIRenderer gamma flag should alter sampled pixel values");

	registerInput.registers.status = 3U | 0x000008U;
	std::vector<u32> gammaNoDitherPixels;
	const rvk2::VIFrameSummary gammaNoDitherSummary =
		rendererSquare.present(registerInput, &gammaNoDitherPixels);
	registerInput.registers.status = 3U | 0x000008U | 0x000004U;
	std::vector<u32> gammaDitherPixels;
	const rvk2::VIFrameSummary gammaDitherSummary =
		rendererSquare.present(registerInput, &gammaDitherPixels);
	expectTrue(
		gammaNoDitherSummary.presentHash != gammaDitherSummary.presentHash,
		"VIRenderer gamma dither flag should alter present hash");
	bool gammaDitherPixelsDiffer = false;
	if (gammaNoDitherPixels.size() == gammaDitherPixels.size()) {
		for (size_t i = 0; i < gammaNoDitherPixels.size(); ++i) {
			if (gammaNoDitherPixels[i] != gammaDitherPixels[i]) {
				gammaDitherPixelsDiffer = true;
				break;
			}
		}
	}
	expectTrue(
		gammaDitherPixelsDiffer,
		"VIRenderer gamma dither flag should alter sampled pixel values");

	std::vector<u32> aaSource{
		0x000000FFU, 0xFFFFFFFFU, 0x404040FFU, 0xFFFFFFFFU,
		0x000000FFU, 0xFFFFFFFFU, 0x404040FFU, 0xFFFFFFFFU
	};
	rvk2::VIRendererConfig configWide{};
	configWide.aspectX = 2U;
	configWide.aspectY = 1U;
	rvk2::VIRenderer rendererWide(configWide);
	rvk2::VIFrameInput aaInput{};
	aaInput.sourceWidth = 4U;
	aaInput.sourceHeight = 2U;
	aaInput.sourcePixels = &aaSource;
	aaInput.registers.valid = true;
	aaInput.registers.status = 3U;
	aaInput.registers.width = 4U;
	aaInput.registers.vSync = 525U;
	aaInput.registers.hStart = (0U << 16U) | 4U;
	aaInput.registers.vStart = (0U << 16U) | 4U;
	aaInput.registers.xScale = 1024U;
	aaInput.registers.yScale = 1024U;
	std::vector<u32> aaMode0Pixels;
	const rvk2::VIFrameSummary aaMode0Summary =
		rendererWide.present(aaInput, &aaMode0Pixels);
	aaInput.registers.status = 3U | (1U << 8U);
	std::vector<u32> aaMode1Pixels;
	const rvk2::VIFrameSummary aaMode1Summary =
		rendererWide.present(aaInput, &aaMode1Pixels);
	aaInput.registers.status = 3U | (2U << 8U);
	std::vector<u32> aaMode2Pixels;
	const rvk2::VIFrameSummary aaMode2Summary =
		rendererWide.present(aaInput, &aaMode2Pixels);
	expectTrue(
		aaMode0Summary.presentHash != aaMode1Summary.presentHash,
		"VIRenderer AA mode1 should alter present hash");
	expectTrue(
		aaMode1Summary.presentHash != aaMode2Summary.presentHash,
		"VIRenderer AA mode2 should alter present hash");
	expectTrue(
		aaMode0Pixels.size() == aaMode1Pixels.size()
			&& aaMode0Pixels.size() == aaMode2Pixels.size(),
		"VIRenderer AA mode sampled pixel size mismatch");
	expectTrue(
		aaMode0Pixels != aaMode1Pixels,
		"VIRenderer AA mode1 should alter sampled pixel values");
	expectTrue(
		aaMode1Pixels != aaMode2Pixels,
		"VIRenderer AA mode2 should alter sampled pixel values");

	std::vector<u32> viTypeSource{
		0x12345678U, 0x89ABCDEFU,
		0x0A1B2C3DU, 0x44556677U
	};
	rvk2::VIFrameInput viTypeInput{};
	viTypeInput.sourceWidth = 2U;
	viTypeInput.sourceHeight = 2U;
	viTypeInput.sourcePixels = &viTypeSource;
	viTypeInput.registers.valid = true;
	viTypeInput.registers.status = 3U;
	viTypeInput.registers.width = 2U;
	viTypeInput.registers.vSync = 525U;
	viTypeInput.registers.hStart = (0U << 16U) | 2U;
	viTypeInput.registers.vStart = (0U << 16U) | 4U;
	viTypeInput.registers.xScale = 1024U;
	viTypeInput.registers.yScale = 1024U;
	std::vector<u32> viType32Pixels;
	const rvk2::VIFrameSummary viType32Summary =
		rendererSquare.present(viTypeInput, &viType32Pixels);
	viTypeInput.registers.status = 2U;
	std::vector<u32> viType16Pixels;
	const rvk2::VIFrameSummary viType16Summary =
		rendererSquare.present(viTypeInput, &viType16Pixels);
	expectTrue(
		viType32Summary.presentHash != viType16Summary.presentHash,
		"VIRenderer type2 decode should alter present hash vs type3");
	expectEq(viType16Pixels.size(), viType32Pixels.size(), "VIRenderer type decode pixel count mismatch");
	expectEq(viType16Pixels[0], 0x10315200U, "VIRenderer type2 quantized sample mismatch");

	std::vector<u32> divotSource{
		0x000000FFU, 0xFFFFFFFFU, 0x000000FFU, 0x000000FFU,
		0x000000FFU, 0xFFFFFFFFU, 0x000000FFU, 0x000000FFU,
		0x000000FFU, 0xFFFFFFFFU, 0x000000FFU, 0x000000FFU,
		0x000000FFU, 0xFFFFFFFFU, 0x000000FFU, 0x000000FFU
	};
	rvk2::VIFrameInput divotInput{};
	divotInput.sourceWidth = 4U;
	divotInput.sourceHeight = 4U;
	divotInput.sourcePixels = &divotSource;
	divotInput.registers.valid = true;
	divotInput.registers.status = 3U;
	divotInput.registers.width = 4U;
	divotInput.registers.vSync = 525U;
	divotInput.registers.hStart = (0U << 16U) | 4U;
	divotInput.registers.vStart = (0U << 16U) | 8U;
	divotInput.registers.xScale = 1024U;
	divotInput.registers.yScale = 1024U;
	std::vector<u32> noDivotPixels;
	const rvk2::VIFrameSummary noDivotSummary =
		rendererSquare.present(divotInput, &noDivotPixels);
	divotInput.registers.status = 3U | 0x000010U;
	std::vector<u32> withDivotPixels;
	const rvk2::VIFrameSummary withDivotSummary =
		rendererSquare.present(divotInput, &withDivotPixels);
	expectTrue(
		noDivotSummary.presentHash != withDivotSummary.presentHash,
		"VIRenderer divot flag should alter present hash");
	expectEq(noDivotPixels[1], 0xFFFFFFFFU, "VIRenderer no-divot sample mismatch");
	expectEq(withDivotPixels[1], 0x000000FFU, "VIRenderer divot median sample mismatch");

	std::vector<u32> interlaceSource{
		0x101010FFU, 0x111111FFU,
		0x202020FFU, 0x212121FFU,
		0x303030FFU, 0x313131FFU,
		0x404040FFU, 0x414141FFU
	};
	rvk2::VIFrameInput interlaceInput{};
	interlaceInput.sourceWidth = 2U;
	interlaceInput.sourceHeight = 4U;
	interlaceInput.sourcePixels = &interlaceSource;
	interlaceInput.registers.valid = true;
	interlaceInput.registers.status = 3U;
	interlaceInput.registers.width = 2U;
	interlaceInput.registers.vSync = 525U;
	interlaceInput.registers.hStart = (0U << 16U) | 2U;
	interlaceInput.registers.vStart = (0U << 16U) | 4U;
	interlaceInput.registers.xScale = 1024U;
	interlaceInput.registers.yScale = 1024U;
	std::vector<u32> nonInterlacedPixels;
	const rvk2::VIFrameSummary nonInterlacedSummary =
		rendererSquare.present(interlaceInput, &nonInterlacedPixels);
	interlaceInput.registers.status = 3U | 0x000040U;
	interlaceInput.registers.vCurrentLine = 1U;
	std::vector<u32> interlacedPixels;
	const rvk2::VIFrameSummary interlacedSummary =
		rendererSquare.present(interlaceInput, &interlacedPixels);
	expectTrue(
		nonInterlacedSummary.presentHash != interlacedSummary.presentHash,
		"VIRenderer interlace flag should alter present hash");
	expectEq(nonInterlacedPixels[0], 0x101010FFU, "VIRenderer non-interlaced sample mismatch");
	expectEq(interlacedPixels[0], 0x202020FFU, "VIRenderer interlaced field sample mismatch");

	registerInput.registers.status = 0U;
	const rvk2::VIFrameSummary blankSummary = rendererSquare.present(registerInput);
	expectEq(blankSummary.presentWidth, 0U, "VIRenderer blank width mismatch");
	expectEq(blankSummary.presentHeight, 0U, "VIRenderer blank height mismatch");
}

void testExecutorVIOriginPresentationSelection()
{
	auto makeFillWork = [](
		u64 _packetId,
		u32 _colorAddress,
		u32 _fillColor) -> rvk2::RenderWorkPacket {
		rvk2::RenderWorkPacket work{};
		work.sourcePacketId = _packetId;
		work.sourceOpcode = 0x36U;
		work.opKind = static_cast<u8>(rvk2::RasterOpKind::kFillRect);
		work.phase = static_cast<u8>(rvk2::RenderPhase::kFill);
		work.cycleType = 3U;
		work.rectULX = 0U;
		work.rectULY = 0U;
		work.rectLRX = 1U;
		work.rectLRY = 1U;
		work.colorImageFormat = 0U;
		work.colorImageSize = 3U;
		work.colorImageWidth = 2U;
		work.colorImageAddress = _colorAddress;
		work.fillColor = _fillColor;
		return work;
	};

	rvk2::RenderWorkPacket fillA = makeFillWork(1ULL, 0x00100000U, 0xFF0000FFU);
	rvk2::RenderWorkPacket fillB = makeFillWork(2ULL, 0x00200000U, 0x00FF00FFU);
	std::vector<rvk2::RenderWorkPacket> workPackets{fillA, fillB};
	rvk2::SubmissionBatchPacket batch{};
	batch.batchIndex = 0U;
	batch.phase = static_cast<u8>(rvk2::RenderPhase::kFill);
	batch.cycleType = 3U;
	batch.firstWorkIndex = 0U;
	batch.lastWorkIndex = 1U;
	batch.workCount = 2U;
	std::vector<rvk2::SubmissionBatchPacket> batches{batch};

	rvk2::ExecutorConfig defaultConfig{};
	defaultConfig.presentAspectX = 1U;
	defaultConfig.presentAspectY = 1U;
	rvk2::Executor defaultExecutor(defaultConfig);
	const rvk2::ExecutorOutput defaultOut =
		defaultExecutor.executeWithOutput(workPackets, batches);
	expectTrue(
		!defaultOut.presentFrame.pixels.empty(),
		"Executor default output should produce present pixels");
	expectEq(
		defaultOut.presentFrame.pixels[0],
		fillB.fillColor,
		"Executor default should present last render target");

	rvk2::ExecutorConfig viConfig = defaultConfig;
	viConfig.viRegistersValid = true;
	viConfig.viStatus = 3U;
	viConfig.viOrigin = fillA.colorImageAddress;
	viConfig.viWidth = 2U;
	viConfig.viVSync = 525U;
	viConfig.viHStart = (0U << 16U) | 2U;
	viConfig.viVStart = (0U << 16U) | 4U;
	viConfig.viXScale = 1024U;
	viConfig.viYScale = 1024U;
	rvk2::Executor viExecutor(viConfig);
	const rvk2::ExecutorOutput viOut =
		viExecutor.executeWithOutput(workPackets, batches);
	expectTrue(
		!viOut.presentFrame.pixels.empty(),
		"Executor VI-origin output should produce present pixels");
	expectEq(
		viOut.presentFrame.pixels[0],
		fillA.fillColor,
		"Executor VI origin should select matching render target");
	expectTrue(
		viOut.summary.presentHash != defaultOut.summary.presentHash,
		"Executor VI origin should alter present hash when selecting another surface");
}

void testExecutorTriangleCoefficientConsumption()
{
	auto makeTriangleWork = []() -> rvk2::RenderWorkPacket {
		rvk2::RenderWorkPacket work{};
		work.sourcePacketId = 1ULL;
		work.sourceOpcode = 0x0FU;
		work.opKind = static_cast<u8>(rvk2::RasterOpKind::kTriangle);
		work.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
		work.cycleType = 0U;
		work.tile = 0U;
		work.textured = false;
		work.depthTest = true;
		work.rectULX = 0U;
		work.rectULY = 0U;
		work.rectLRX = 2U;
		work.rectLRY = 2U;
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
		work.triangleZBufferEnable = true;
		work.triangleShadeR = 0x2000;
		work.triangleShadeG = 0x4000;
		work.triangleShadeB = 0x6000;
		work.triangleShadeA = 0xFF00;
		work.triangleZ = 100;
		work.combineMux = 0x1122334455667788ULL;
		return work;
	};

	auto makeSingleBatch = []() -> rvk2::SubmissionBatchPacket {
		rvk2::SubmissionBatchPacket batch{};
		batch.batchIndex = 0U;
		batch.firstWorkIndex = 0U;
		batch.lastWorkIndex = 0U;
		batch.workCount = 1U;
		return batch;
	};

	rvk2::Executor executor;

	rvk2::RenderWorkPacket nearWork = makeTriangleWork();
	std::vector<rvk2::RenderWorkPacket> nearOnly{nearWork};
	std::vector<rvk2::SubmissionBatchPacket> nearBatch{makeSingleBatch()};
	const rvk2::ExecutorOutput nearOut = executor.executeWithOutput(nearOnly, nearBatch);
	expectTrue(nearOut.summary.colorWriteCount > 0ULL, "triangle near pass should write color");

	rvk2::RenderWorkPacket shadeWork = nearWork;
	shadeWork.triangleShadeR = 0xE000;
	shadeWork.triangleShadeG = 0x1000;
	shadeWork.triangleShadeB = 0x3000;
	std::vector<rvk2::RenderWorkPacket> shadeOnly{shadeWork};
	const rvk2::ExecutorOutput shadeOut = executor.executeWithOutput(shadeOnly, nearBatch);
	expectTrue(
		nearOut.summary.presentHash != shadeOut.summary.presentHash,
		"triangle shade coefficients should affect present hash");

	rvk2::RenderWorkPacket texWorkA = nearWork;
	texWorkA.textured = true;
	texWorkA.triangleTextureEnable = true;
	texWorkA.triangleTexS = 0x01000000;
	texWorkA.triangleTexT = 0x02000000;
	texWorkA.triangleTexW = 0x00100000;
	texWorkA.triangleTexDSDX = 0x00004000;
	texWorkA.triangleTexDTDX = 0x00002000;
	texWorkA.triangleTexDWDX = 0;
	texWorkA.triangleTexDSDY = 0x00001000;
	texWorkA.triangleTexDTDY = 0x00000800;
	texWorkA.triangleTexDWDY = 0;
	texWorkA.triangleTexDSDE = 0;
	texWorkA.triangleTexDTDE = 0;
	texWorkA.triangleTexDWDE = 0;
	std::vector<rvk2::RenderWorkPacket> texOnlyA{texWorkA};
	const rvk2::ExecutorOutput texOutA = executor.executeWithOutput(texOnlyA, nearBatch);

	rvk2::RenderWorkPacket texWorkB = texWorkA;
	texWorkB.triangleTexS += 0x00100000;
	std::vector<rvk2::RenderWorkPacket> texOnlyB{texWorkB};
	const rvk2::ExecutorOutput texOutB = executor.executeWithOutput(texOnlyB, nearBatch);
	expectTrue(
		texOutA.summary.presentHash != texOutB.summary.presentHash,
		"triangle texture coefficients should affect present hash");

	rvk2::RenderWorkPacket farWork = shadeWork;
	farWork.triangleZ = 200;
	farWork.sourcePacketId = 2ULL;
	std::vector<rvk2::RenderWorkPacket> nearThenFar{nearWork, farWork};
	rvk2::SubmissionBatchPacket combinedBatch = makeSingleBatch();
	combinedBatch.lastWorkIndex = 1U;
	combinedBatch.workCount = 2U;
	std::vector<rvk2::SubmissionBatchPacket> combinedBatches{combinedBatch};
	const rvk2::ExecutorOutput combinedOut = executor.executeWithOutput(nearThenFar, combinedBatches);
	expectEq(
		combinedOut.summary.colorWriteCount,
		nearOut.summary.colorWriteCount,
		"triangle depth test should reject farther triangle writes");
	expectEq(
		combinedOut.summary.presentHash,
		nearOut.summary.presentHash,
		"triangle depth test should preserve near-triangle present hash");
}

void testExecutorFrameOutput()
{
	rvk2::Runtime runtime;
	runtime.beginFrame(9ULL);
	rvk2::CommandProvenance provenance{};
	provenance.taskId = 9U;
	provenance.microcode = 20U;

	runtime.submitRDPWord(
		0x4000U,
		(0x3FU << 24) | (0U << 21) | (3U << 19) | 3U,
		0x00100000U,
		provenance);
	runtime.submitRDPWord(
		0x4008U,
		(0x37U << 24),
		0xFFFFFFFFU,
		provenance);
	runtime.submitRDPWord(
		0x4010U,
		(0x36U << 24) | (3U << 14) | (2U << 2),
		0U,
		provenance);

	rvk2::Executor executor;
	const rvk2::ExecutorOutput output =
		executor.executeWithOutput(runtime.renderPlan(), runtime.submissionPlan());
	expectEq(output.summary.executedWorkCount, 1ULL, "Executor output work count mismatch");
	expectTrue(output.presentFrame.width > 0U, "Executor output width should be positive");
	expectTrue(output.presentFrame.height > 0U, "Executor output height should be positive");
	expectEq(
		output.presentFrame.pixels.size(),
		static_cast<size_t>(output.presentFrame.width * output.presentFrame.height),
		"Executor output pixel count mismatch");
}

} // namespace

int main()
{
	testSchemaVersion();
	testOpcodeDecodeIdentity();
	testCommandHashStability();
	testExtendedPayloadCaptureAndHash();
	testTriangleExtendedSemanticDecode();
	testRDPStateTransitions();
	testRDPExtendedStateFields();
	testTMEMStateTransitions();
	testRuntimeDrawSemanticCapture();
	testSyntheticTrianglePacking();
	testTexRectSemanticExtraction();
	testVIRendererAspectScaling();
	testExecutorVIOriginPresentationSelection();
	testExecutorTriangleCoefficientConsumption();
	testExecutorFrameOutput();

	if (g_failures == 0) {
		std::printf("rvk2 unit tests: PASS\n");
		return 0;
	}

	std::fprintf(stderr, "rvk2 unit tests: FAIL (%d failure(s))\n", g_failures);
	return 1;
}
