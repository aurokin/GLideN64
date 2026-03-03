#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "rvk2_CommandStream.h"
#include "rvk2_Executor.h"
#include "rvk2_RDPState.h"
#include "rvk2_RenderPlan.h"
#include "rvk2_Runtime.h"
#include "rvk2_RSPFrontend.h"
#include "rvk2_SubmissionPlan.h"
#include "rvk2_SyntheticTriangle.h"
#include "rvk2_TMEMModel.h"
#include "rvk2_TextureReplacement.h"
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

struct ScopedEnvVar
{
	explicit ScopedEnvVar(const char * _key)
		: key(_key != nullptr ? _key : "")
		, hadValue(false)
	{
		const char * value = std::getenv(key.c_str());
		if (value != nullptr) {
			hadValue = true;
			savedValue = value;
		}
	}

	~ScopedEnvVar()
	{
		if (hadValue)
			::setenv(key.c_str(), savedValue.c_str(), 1);
		else
			::unsetenv(key.c_str());
	}

	void set(const std::string & _value) const
	{
		::setenv(key.c_str(), _value.c_str(), 1);
	}

	void clear() const
	{
		::unsetenv(key.c_str());
	}

	std::string key;
	bool hadValue;
	std::string savedValue;
};

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
	expectEq(engine.snapshot().scissorXH, static_cast<u16>(0x123U >> 2U), "SetScissor XH mismatch");
	expectEq(engine.snapshot().scissorYH, static_cast<u16>(0x456U >> 2U), "SetScissor YH mismatch");
	expectEq(engine.snapshot().scissorXL, static_cast<u16>(0x789U >> 2U), "SetScissor XL mismatch");
	expectEq(engine.snapshot().scissorYL, static_cast<u16>(0xABCU >> 2U), "SetScissor YL mismatch");

	rvk2::CommandPacket setColor{};
	setColor.id = 4ULL;
	setColor.domain = rvk2::CommandDomain::kRDP;
	setColor.opcode = 0x3FU;
	setColor.w0 = (0x3FU << 24) | (5U << 21) | (2U << 19) | 0x2ABU;
	setColor.w1 = 0x8F123456U;
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
	setDepth.w1 = 0xFFFEDCBAU;
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
	setTexture.w1 = 0x80ABCDEFU;
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
	expectEq(fill.rectULY, static_cast<u16>(0x22U >> 2U), "Draw semantic fill ULY mismatch");
	expectEq(fill.rectLRX, static_cast<u16>(0U), "Draw semantic fill LRX mismatch");
	expectEq(fill.rectLRY, static_cast<u16>(0x11U >> 2U), "Draw semantic fill LRY mismatch");

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

void testTriangleSignedYBounds()
{
	rvk2::Runtime runtime;
	runtime.beginFrame(41ULL);

	rvk2::CommandProvenance provenance{};
	provenance.taskId = 41U;
	provenance.microcode = 20U;

	// Triangle Y edges are signed 14-bit values in 10.2 subpixel units.
	const u16 yl = static_cast<u16>(0x3FF0U); // -16 subpixels => -4 pixels.
	const u16 ym = static_cast<u16>(0x0010U); // +16 subpixels => +4 pixels.
	const u16 yh = static_cast<u16>(0x0000U); // 0 subpixels.

	runtime.submitRDPWord(
		0x00410000U,
		(0x08U << 24) | (1U << 23) | yl,
		(static_cast<u32>(ym) << 16U) | static_cast<u32>(yh),
		provenance,
		6U,
		0x00010000U, // XL
		0x00000000U, // DxLDY
		0x00000000U, // XH
		0x00000000U, // DxHDY
		0x00000000U, // XM
		0x00000000U, // DxMDY
		8U);

	const std::vector<rvk2::RenderWorkPacket> & renderPlan = runtime.renderPlan();
	expectEq(renderPlan.size(), static_cast<size_t>(1U), "Signed-Y triangle render plan count mismatch");
	expectEq(renderPlan[0].triangleYL, yl, "Signed-Y triangle raw YL mismatch");
	expectEq(renderPlan[0].triangleYM, ym, "Signed-Y triangle raw YM mismatch");
	expectEq(renderPlan[0].triangleYH, yh, "Signed-Y triangle raw YH mismatch");
	expectEq(renderPlan[0].rectULY, static_cast<u16>(0U), "Signed-Y triangle rect ULY should clamp to zero");
	expectTrue(
		renderPlan[0].rectLRY <= static_cast<u16>(4U),
		"Signed-Y triangle rect LRY should stay near top edge");
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

void testSyntheticTrianglePerspectivePacking()
{
	SPVertex v0{};
	SPVertex v1{};
	SPVertex v2{};
	v0.x = 0.0f;
	v0.y = 0.0f;
	v1.x = 1.0f;
	v1.y = 0.0f;
	v2.x = 0.0f;
	v2.y = 1.0f;
	v0.s = 10.0f;
	v1.s = 14.0f;
	v2.s = 10.0f;
	v0.t = 20.0f;
	v1.t = 20.0f;
	v2.t = 24.0f;
	v0.w = 2.0f;
	v1.w = 2.0f;
	v2.w = 2.0f;

	rvk2::synthetic_triangle::TriangleCommand persp{};
	rvk2::synthetic_triangle::TriangleCommand affine{};
	const bool builtPersp =
		rvk2::synthetic_triangle::buildCommand(v0, v1, v2, 0x0AU, true, 0U, 0U, persp);
	const bool builtAffine =
		rvk2::synthetic_triangle::buildCommand(v0, v1, v2, 0x0AU, false, 0U, 0U, affine);
	expectTrue(builtPersp, "Synthetic triangle perspective command should build");
	expectTrue(builtAffine, "Synthetic triangle affine command should build");
	expectEq(persp.payloadWordCount, static_cast<u8>(22U), "Synthetic triangle perspective payload count mismatch");
	expectEq(affine.payloadWordCount, static_cast<u8>(22U), "Synthetic triangle affine payload count mismatch");

	auto decodePrimary = [](u32 _a, u32 _b) -> s32 {
		return static_cast<s32>((_a & 0xFFFF0000U) | ((_b >> 16U) & 0xFFFFU));
	};
	auto decodeSecondary = [](u32 _a, u32 _b) -> s32 {
		return static_cast<s32>(((_a << 16U) & 0xFFFF0000U) | (_b & 0xFFFFU));
	};

	const s32 perspSBase = decodePrimary(persp.payloadWords[6], persp.payloadWords[10]);
	const s32 perspTBase = decodeSecondary(persp.payloadWords[6], persp.payloadWords[10]);
	const s32 perspWBase = decodePrimary(persp.payloadWords[7], persp.payloadWords[11]);
	const s32 perspSDX = decodePrimary(persp.payloadWords[8], persp.payloadWords[12]);
	const s32 perspTDX = decodeSecondary(persp.payloadWords[8], persp.payloadWords[12]);
	const s32 perspWDX = decodePrimary(persp.payloadWords[9], persp.payloadWords[13]);
	const s32 perspSDY = decodePrimary(persp.payloadWords[16], persp.payloadWords[20]);
	const s32 perspTDY = decodeSecondary(persp.payloadWords[16], persp.payloadWords[20]);
	const s32 perspWDY = decodePrimary(persp.payloadWords[17], persp.payloadWords[21]);

	const s32 affineSBase = decodePrimary(affine.payloadWords[6], affine.payloadWords[10]);
	const s32 affineTBase = decodeSecondary(affine.payloadWords[6], affine.payloadWords[10]);
	const s32 affineWBase = decodePrimary(affine.payloadWords[7], affine.payloadWords[11]);
	const s32 affineSDX = decodePrimary(affine.payloadWords[8], affine.payloadWords[12]);
	const s32 affineTDX = decodeSecondary(affine.payloadWords[8], affine.payloadWords[12]);
	const s32 affineWDX = decodePrimary(affine.payloadWords[9], affine.payloadWords[13]);
	const s32 affineSDY = decodePrimary(affine.payloadWords[16], affine.payloadWords[20]);
	const s32 affineTDY = decodeSecondary(affine.payloadWords[16], affine.payloadWords[20]);
	const s32 affineWDY = decodePrimary(affine.payloadWords[17], affine.payloadWords[21]);

	expectEq(affineSBase, 320, "Synthetic triangle affine S base mismatch");
	expectEq(affineTBase, 640, "Synthetic triangle affine T base mismatch");
	expectEq(affineSDX, 128, "Synthetic triangle affine S dx mismatch");
	expectEq(affineTDX, 0, "Synthetic triangle affine T dx mismatch");
	expectEq(affineSDY, 0, "Synthetic triangle affine S dy mismatch");
	expectEq(affineTDY, 128, "Synthetic triangle affine T dy mismatch");

	expectEq(perspSBase, 160, "Synthetic triangle perspective S base mismatch");
	expectEq(perspTBase, 320, "Synthetic triangle perspective T base mismatch");
	expectEq(perspSDX, 64, "Synthetic triangle perspective S dx mismatch");
	expectEq(perspTDX, 0, "Synthetic triangle perspective T dx mismatch");
	expectEq(perspSDY, 0, "Synthetic triangle perspective S dy mismatch");
	expectEq(perspTDY, 64, "Synthetic triangle perspective T dy mismatch");

	expectEq(affineWBase, 32768, "Synthetic triangle affine W base mismatch");
	expectEq(affineWDX, 0, "Synthetic triangle affine W dx mismatch");
	expectEq(affineWDY, 0, "Synthetic triangle affine W dy mismatch");
	expectEq(perspWBase, 32768, "Synthetic triangle perspective W base mismatch");
	expectEq(perspWDX, 0, "Synthetic triangle perspective W dx mismatch");
	expectEq(perspWDY, 0, "Synthetic triangle perspective W dy mismatch");
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
	expectEq(texrect.rectULX, static_cast<u16>(0x080U >> 2U), "TexRect semantic ULX mismatch");
	expectEq(texrect.rectULY, static_cast<u16>(0x0A0U >> 2U), "TexRect semantic ULY mismatch");
	expectEq(texrect.rectLRX, static_cast<u16>(0x120U >> 2U), "TexRect semantic LRX mismatch");
	expectEq(texrect.rectLRY, static_cast<u16>(0x140U >> 2U), "TexRect semantic LRY mismatch");
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
	expectEq(summary169.contentX, 0U, "VIRenderer 16:9 contentX mismatch");
	expectEq(summary169.contentY, 0U, "VIRenderer 16:9 contentY mismatch");
	expectEq(summary169.contentWidth, 6U, "VIRenderer 16:9 content width mismatch");
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
	expectEq(summaryCrop.contentY, 0U, "VIRenderer 16:9->4:3 contentY mismatch");
	expectEq(summaryCrop.contentWidth, 8U, "VIRenderer 16:9->4:3 content width mismatch");
	expectEq(summaryCrop.contentHeight, 6U, "VIRenderer 16:9->4:3 content height mismatch");

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
	registerInput.registers.status = 3U | (3U << 8U);
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

	registerInput.registers.status = 3U | (3U << 8U);
	std::vector<u32> noGammaPixels;
	const rvk2::VIFrameSummary noGammaSummary =
		rendererSquare.present(registerInput, &noGammaPixels);
	registerInput.registers.status = (3U | (3U << 8U)) | 0x000008U;
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

	registerInput.registers.status = (3U | (3U << 8U)) | 0x000008U;
	std::vector<u32> gammaNoDitherPixels;
	const rvk2::VIFrameSummary gammaNoDitherSummary =
		rendererSquare.present(registerInput, &gammaNoDitherPixels);
	registerInput.registers.status = (3U | (3U << 8U)) | 0x000008U | 0x000004U;
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
	aaInput.registers.status = 3U | (3U << 8U);
	std::vector<u32> aaMode3Pixels;
	const rvk2::VIFrameSummary aaMode3Summary =
		rendererWide.present(aaInput, &aaMode3Pixels);
	expectTrue(
		aaMode0Summary.presentHash != aaMode1Summary.presentHash,
		"VIRenderer AA mode1 should alter present hash");
	expectTrue(
		aaMode1Summary.presentHash != aaMode2Summary.presentHash,
		"VIRenderer AA mode2 should alter present hash");
	expectTrue(
		aaMode2Summary.presentHash != aaMode3Summary.presentHash,
		"VIRenderer AA mode3 should alter present hash vs mode2");
	expectTrue(
		aaMode0Pixels.size() == aaMode1Pixels.size()
			&& aaMode0Pixels.size() == aaMode2Pixels.size()
			&& aaMode0Pixels.size() == aaMode3Pixels.size(),
		"VIRenderer AA mode sampled pixel size mismatch");
	expectTrue(
		aaMode0Pixels != aaMode1Pixels,
		"VIRenderer AA mode1 should alter sampled pixel values");
	expectTrue(
		aaMode1Pixels != aaMode2Pixels,
		"VIRenderer AA mode2 should alter sampled pixel values");
	expectTrue(
		aaMode2Pixels != aaMode3Pixels,
		"VIRenderer AA mode3 should alter sampled pixel values vs mode2");

	std::vector<u32> pixelAdvanceSource{
		0x101010FFU, 0x202020FFU, 0x303030FFU, 0x404040FFU,
		0x505050FFU, 0x606060FFU, 0x707070FFU, 0x808080FFU
	};
	rvk2::VIFrameInput pixelAdvanceInput{};
	pixelAdvanceInput.sourceWidth = 4U;
	pixelAdvanceInput.sourceHeight = 2U;
	pixelAdvanceInput.sourcePixels = &pixelAdvanceSource;
	pixelAdvanceInput.registers.valid = true;
	pixelAdvanceInput.registers.status = 3U | (3U << 8U);
	pixelAdvanceInput.registers.width = 4U;
	pixelAdvanceInput.registers.vSync = 525U;
	pixelAdvanceInput.registers.hStart = (0U << 16U) | 2U;
	pixelAdvanceInput.registers.vStart = (0U << 16U) | 4U;
	pixelAdvanceInput.registers.xScale = 1024U;
	pixelAdvanceInput.registers.yScale = 1024U;
	std::vector<u32> pixelAdvanceBasePixels;
	const rvk2::VIFrameSummary pixelAdvanceBaseSummary =
		rendererSquare.present(pixelAdvanceInput, &pixelAdvanceBasePixels);
	pixelAdvanceInput.registers.status = (3U | (3U << 8U)) | (4U << 12U);
	std::vector<u32> pixelAdvanceShiftedPixels;
	const rvk2::VIFrameSummary pixelAdvanceShiftedSummary =
		rendererSquare.present(pixelAdvanceInput, &pixelAdvanceShiftedPixels);
	expectEq(pixelAdvanceBaseSummary.presentWidth, 2U, "VIRenderer pixel-advance base width mismatch");
	expectEq(pixelAdvanceBaseSummary.presentHeight, 2U, "VIRenderer pixel-advance base height mismatch");
	expectEq(pixelAdvanceShiftedSummary.presentWidth, 2U, "VIRenderer pixel-advance shifted width mismatch");
	expectEq(pixelAdvanceShiftedSummary.presentHeight, 2U, "VIRenderer pixel-advance shifted height mismatch");
	expectTrue(
		pixelAdvanceBaseSummary.presentHash != pixelAdvanceShiftedSummary.presentHash,
		"VIRenderer pixel-advance should alter present hash");
	expectEq(
		pixelAdvanceShiftedPixels[0],
		pixelAdvanceBasePixels[1],
		"VIRenderer pixel-advance first sample mismatch");

	pixelAdvanceInput.registers.status = (3U | (3U << 8U)) | (12U << 12U);
	std::vector<u32> pixelAdvanceOverflowPixels;
	const rvk2::VIFrameSummary pixelAdvanceOverflowSummary =
		rendererSquare.present(pixelAdvanceInput, &pixelAdvanceOverflowPixels);
	expectEq(
		pixelAdvanceOverflowSummary.presentWidth,
		2U,
		"VIRenderer pixel-advance overflow width mismatch");
	expectEq(
		pixelAdvanceOverflowSummary.presentHeight,
		2U,
		"VIRenderer pixel-advance overflow height mismatch");
	expectEq(
		pixelAdvanceOverflowPixels[0],
		pixelAdvanceSource[3],
		"VIRenderer pixel-advance overflow first sample mismatch");
	expectEq(
		pixelAdvanceOverflowPixels[1],
		0x00000000U,
		"VIRenderer pixel-advance overflow second sample mismatch");

	std::vector<u32> viTypeSource{
		0x12345678U, 0x89ABCDEFU, 0x0A1B2C3DU, 0x44556677U,
		0x13579BDFU, 0x2468ACE0U, 0x10203040U, 0x50607080U
	};
	rvk2::VIFrameInput viTypeInput{};
	viTypeInput.sourceWidth = 4U;
	viTypeInput.sourceHeight = 2U;
	viTypeInput.sourcePixels = &viTypeSource;
	viTypeInput.registers.valid = true;
	viTypeInput.registers.status = 3U | (3U << 8U);
	viTypeInput.registers.width = 4U;
	viTypeInput.registers.vSync = 525U;
	viTypeInput.registers.hStart = (0U << 16U) | 2U;
	viTypeInput.registers.vStart = (0U << 16U) | 4U;
	viTypeInput.registers.xScale = 1024U;
	viTypeInput.registers.yScale = 1024U;
	std::vector<u32> viType32Pixels;
	const rvk2::VIFrameSummary viType32Summary =
		rendererSquare.present(viTypeInput, &viType32Pixels);
	viTypeInput.registers.status = 2U | (3U << 8U);
	std::vector<u32> viType16Pixels;
	const rvk2::VIFrameSummary viType16Summary =
		rendererSquare.present(viTypeInput, &viType16Pixels);
	expectTrue(
		viType32Summary.presentHash != viType16Summary.presentHash,
		"VIRenderer type2 decode should alter present hash vs type3");
	expectEq(viType16Pixels.size(), viType32Pixels.size(), "VIRenderer type decode pixel count mismatch");
	expectEq(viType16Pixels[0], 0x10315200U, "VIRenderer type2 quantized sample mismatch");

	rvk2::VIFrameInput viTypeReservedInput = viTypeInput;
	viTypeReservedInput.registers.status = 1U | (3U << 8U);
	const rvk2::VIFrameSummary viTypeReservedSummary =
		rendererSquare.present(viTypeReservedInput);
	expectEq(viTypeReservedSummary.presentWidth, 0U, "VIRenderer reserved type width mismatch");
	expectEq(viTypeReservedSummary.presentHeight, 0U, "VIRenderer reserved type height mismatch");

	rvk2::VIFrameInput viTypeOffsetInput = viTypeInput;
	viTypeOffsetInput.sourceAddressValid = true;
	viTypeOffsetInput.sourceAddress = 0U;
	viTypeOffsetInput.registers.status = 2U | (3U << 8U);
	viTypeOffsetInput.registers.origin = 8U;
	std::vector<u32> viTypeOffsetPixels;
	const rvk2::VIFrameSummary viTypeOffsetSummary =
		rendererSquare.present(viTypeOffsetInput, &viTypeOffsetPixels);
	expectEq(viTypeOffsetSummary.presentWidth, 2U, "VIRenderer type2 origin-offset width mismatch");
	expectEq(viTypeOffsetSummary.presentHeight, 2U, "VIRenderer type2 origin-offset height mismatch");
	expectEq(viTypeOffsetPixels.size(), static_cast<size_t>(4U), "VIRenderer type2 origin-offset pixel count mismatch");
	expectEq(viTypeOffsetPixels[0], viType16Pixels[2], "VIRenderer type2 origin-offset sample (0,0) mismatch");
	expectEq(viTypeOffsetPixels[1], viType16Pixels[3], "VIRenderer type2 origin-offset sample (1,0) mismatch");
	expectEq(viTypeOffsetPixels[2], 0x00000000U, "VIRenderer type2 origin-offset sample (0,1) mismatch");
	expectEq(viTypeOffsetPixels[3], 0x00000000U, "VIRenderer type2 origin-offset sample (1,1) mismatch");

	std::vector<u32> deditherSource{
		0x707070FFU, 0x808080FFU, 0x707070FFU, 0x808080FFU,
		0x808080FFU, 0x707070FFU, 0x808080FFU, 0x707070FFU,
		0x707070FFU, 0x808080FFU, 0x707070FFU, 0x808080FFU,
		0x808080FFU, 0x707070FFU, 0x808080FFU, 0x707070FFU
	};
	rvk2::VIFrameInput deditherInput{};
	deditherInput.sourceWidth = 4U;
	deditherInput.sourceHeight = 4U;
	deditherInput.sourcePixels = &deditherSource;
	deditherInput.registers.valid = true;
	deditherInput.registers.status = 2U | (3U << 8U);
	deditherInput.registers.width = 4U;
	deditherInput.registers.vSync = 525U;
	deditherInput.registers.hStart = (0U << 16U) | 4U;
	deditherInput.registers.vStart = (0U << 16U) | 8U;
	deditherInput.registers.xScale = 1024U;
	deditherInput.registers.yScale = 1024U;
	std::vector<u32> noDeditherPixels;
	const rvk2::VIFrameSummary noDeditherSummary =
		rendererSquare.present(deditherInput, &noDeditherPixels);
	deditherInput.registers.status = 2U | (3U << 8U) | 0x010000U;
	std::vector<u32> withDeditherPixels;
	const rvk2::VIFrameSummary withDeditherSummary =
		rendererSquare.present(deditherInput, &withDeditherPixels);
	expectTrue(
		noDeditherSummary.presentHash != withDeditherSummary.presentHash,
		"VIRenderer dedither flag should alter present hash");
	expectTrue(
		noDeditherPixels != withDeditherPixels,
		"VIRenderer dedither flag should alter sampled pixel values");

	deditherInput.registers.status = 2U | (1U << 8U);
	std::vector<u32> aaNeededNoDeditherPixels;
	const rvk2::VIFrameSummary aaNeededNoDeditherSummary =
		rendererSquare.present(deditherInput, &aaNeededNoDeditherPixels);
	deditherInput.registers.status = 2U | (1U << 8U) | 0x010000U;
	std::vector<u32> aaNeededWithDeditherPixels;
	const rvk2::VIFrameSummary aaNeededWithDeditherSummary =
		rendererSquare.present(deditherInput, &aaNeededWithDeditherPixels);
	expectEq(
		aaNeededNoDeditherSummary.presentHash,
		aaNeededWithDeditherSummary.presentHash,
		"VIRenderer dedither should be inactive for AA-needed mode");
	expectEq(
		aaNeededNoDeditherPixels,
		aaNeededWithDeditherPixels,
		"VIRenderer dedither should not alter AA-needed sampled pixels");

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
	divotInput.registers.status = 3U | (3U << 8U);
	divotInput.registers.width = 4U;
	divotInput.registers.vSync = 525U;
	divotInput.registers.hStart = (0U << 16U) | 4U;
	divotInput.registers.vStart = (0U << 16U) | 8U;
	divotInput.registers.xScale = 1024U;
	divotInput.registers.yScale = 1024U;
	std::vector<u32> noDivotPixels;
	const rvk2::VIFrameSummary noDivotSummary =
		rendererSquare.present(divotInput, &noDivotPixels);
	divotInput.registers.status = (3U | (3U << 8U)) | 0x000010U;
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
	interlaceInput.registers.status = 3U | (3U << 8U);
	interlaceInput.registers.width = 2U;
	interlaceInput.registers.vSync = 525U;
	interlaceInput.registers.hStart = (0U << 16U) | 2U;
	interlaceInput.registers.vStart = (0U << 16U) | 4U;
	interlaceInput.registers.xScale = 1024U;
	interlaceInput.registers.yScale = 1024U;
	std::vector<u32> nonInterlacedPixels;
	const rvk2::VIFrameSummary nonInterlacedSummary =
		rendererSquare.present(interlaceInput, &nonInterlacedPixels);
	interlaceInput.registers.status = (3U | (3U << 8U)) | 0x000040U;
	interlaceInput.registers.vCurrentLine = 1U;
	std::vector<u32> interlacedPixels;
	const rvk2::VIFrameSummary interlacedSummary =
		rendererSquare.present(interlaceInput, &interlacedPixels);
	expectTrue(
		nonInterlacedSummary.presentHash != interlacedSummary.presentHash,
		"VIRenderer interlace flag should alter present hash");
	expectEq(nonInterlacedPixels[0], 0x101010FFU, "VIRenderer non-interlaced sample mismatch");
	expectEq(interlacedPixels[0], 0x202020FFU, "VIRenderer interlaced field sample mismatch");

	std::vector<u32> interlacePhaseSource{
		0x000000FFU, 0x000000FFU,
		0x101010FFU, 0x101010FFU,
		0x202020FFU, 0x202020FFU,
		0x303030FFU, 0x303030FFU,
		0x404040FFU, 0x404040FFU,
		0x505050FFU, 0x505050FFU,
		0x606060FFU, 0x606060FFU,
		0x707070FFU, 0x707070FFU
	};
	rvk2::VIFrameInput interlacePhaseInput{};
	interlacePhaseInput.sourceWidth = 2U;
	interlacePhaseInput.sourceHeight = 8U;
	interlacePhaseInput.sourcePixels = &interlacePhaseSource;
	interlacePhaseInput.registers.valid = true;
	interlacePhaseInput.registers.status = (3U | (3U << 8U)) | 0x000040U;
	interlacePhaseInput.registers.vCurrentLine = 1U;
	interlacePhaseInput.registers.width = 2U;
	interlacePhaseInput.registers.vSync = 525U;
	interlacePhaseInput.registers.hStart = (0U << 16U) | 2U;
	interlacePhaseInput.registers.vStart = (0U << 16U) | 4U;
	interlacePhaseInput.registers.xScale = 1024U;
	interlacePhaseInput.registers.yScale = (512U << 16U) | 2048U;
	std::vector<u32> interlacePhasePixels;
	const rvk2::VIFrameSummary interlacePhaseSummary =
		rendererSquare.present(interlacePhaseInput, &interlacePhasePixels);
	expectEq(interlacePhaseSummary.presentWidth, 2U, "VIRenderer interlace phase width mismatch");
	expectEq(interlacePhaseSummary.presentHeight, 2U, "VIRenderer interlace phase height mismatch");
	expectEq(interlacePhasePixels.size(), static_cast<size_t>(4U), "VIRenderer interlace phase pixel count mismatch");
	expectEq(interlacePhasePixels[0], 0x202020FFU, "VIRenderer interlace phase row0 mismatch");
	expectEq(interlacePhasePixels[2], 0x606060FFU, "VIRenderer interlace phase row1 mismatch");

	std::vector<u32> clipSource{
		0x000000FFU, 0x111111FFU, 0x222222FFU, 0x333333FFU,
		0x404040FFU, 0x515151FFU, 0x626262FFU, 0x737373FFU,
		0x808080FFU, 0x919191FFU, 0xA2A2A2FFU, 0xB3B3B3FFU,
		0xC0C0C0FFU, 0xD1D1D1FFU, 0xE2E2E2FFU, 0xF3F3F3FFU
	};
	rvk2::VIFrameInput clipInput{};
	clipInput.sourceWidth = 4U;
	clipInput.sourceHeight = 4U;
	clipInput.sourcePixels = &clipSource;
	clipInput.registers.valid = true;
	clipInput.registers.status = 3U | (3U << 8U);
	clipInput.registers.width = 4U;
	clipInput.registers.vSync = 525U;
	clipInput.registers.hStart = (0U << 16U) | 2U;
	clipInput.registers.vStart = (0U << 16U) | 4U;
	clipInput.registers.xScale = (4095U << 16U) | 1024U;
	clipInput.registers.yScale = (3072U << 16U) | 2048U;
	std::vector<u32> clipPixels;
	const rvk2::VIFrameSummary clipSummary =
		rendererSquare.present(clipInput, &clipPixels);
	expectEq(clipSummary.presentWidth, 2U, "VIRenderer clip width mismatch");
	expectEq(clipSummary.presentHeight, 2U, "VIRenderer clip height mismatch");
	expectEq(clipPixels.size(), static_cast<size_t>(4U), "VIRenderer clip pixel count mismatch");
	expectEq(clipPixels[0], 0xF3F3F3FFU, "VIRenderer clip in-range sample mismatch");
	expectEq(clipPixels[1], 0x00000000U, "VIRenderer clip x-overflow sample mismatch");
	expectEq(clipPixels[2], 0x00000000U, "VIRenderer clip y-overflow sample mismatch");
	expectEq(clipPixels[3], 0x00000000U, "VIRenderer clip xy-overflow sample mismatch");

	std::vector<u32> originSource{
		0x101010FFU, 0x202020FFU, 0x303030FFU, 0x404040FFU,
		0x505050FFU, 0x606060FFU, 0x707070FFU, 0x808080FFU
	};
	rvk2::VIFrameInput originInput{};
	originInput.sourceAddressValid = true;
	originInput.sourceAddress = 0x00100000U;
	originInput.sourceWidth = 4U;
	originInput.sourceHeight = 2U;
	originInput.sourcePixels = &originSource;
	originInput.registers.valid = true;
	originInput.registers.status = 3U | (3U << 8U);
	originInput.registers.origin = originInput.sourceAddress + 8U;
	originInput.registers.width = 4U;
	originInput.registers.vSync = 525U;
	originInput.registers.hStart = (0U << 16U) | 2U;
	originInput.registers.vStart = (0U << 16U) | 4U;
	originInput.registers.xScale = 1024U;
	originInput.registers.yScale = 1024U;
	std::vector<u32> originPixels;
	const rvk2::VIFrameSummary originSummary =
		rendererSquare.present(originInput, &originPixels);
	expectEq(originSummary.presentWidth, 2U, "VIRenderer origin-offset width mismatch");
	expectEq(originSummary.presentHeight, 2U, "VIRenderer origin-offset height mismatch");
	expectEq(originPixels.size(), static_cast<size_t>(4U), "VIRenderer origin-offset pixel count mismatch");
	expectEq(originPixels[0], originSource[2], "VIRenderer origin-offset sample (0,0) mismatch");
	expectEq(originPixels[1], originSource[3], "VIRenderer origin-offset sample (1,0) mismatch");
	expectEq(originPixels[2], originSource[6], "VIRenderer origin-offset sample (0,1) mismatch");
	expectEq(originPixels[3], originSource[7], "VIRenderer origin-offset sample (1,1) mismatch");

	originInput.registers.origin = originInput.sourceAddress + 32U;
	std::vector<u32> originOutOfRangePixels;
	const rvk2::VIFrameSummary originOutOfRangeSummary =
		rendererSquare.present(originInput, &originOutOfRangePixels);
	expectEq(originOutOfRangeSummary.presentWidth, 2U, "VIRenderer origin OOR width mismatch");
	expectEq(originOutOfRangeSummary.presentHeight, 2U, "VIRenderer origin OOR height mismatch");
	expectEq(originOutOfRangePixels.size(), static_cast<size_t>(4U), "VIRenderer origin OOR pixel count mismatch");
	expectEq(originOutOfRangePixels[0], 0x00000000U, "VIRenderer origin OOR sample (0,0) mismatch");
	expectEq(originOutOfRangePixels[1], 0x00000000U, "VIRenderer origin OOR sample (1,0) mismatch");
	expectEq(originOutOfRangePixels[2], 0x00000000U, "VIRenderer origin OOR sample (0,1) mismatch");
	expectEq(originOutOfRangePixels[3], 0x00000000U, "VIRenderer origin OOR sample (1,1) mismatch");

	std::vector<u32> strideSource(32U, 0U);
	for (size_t i = 0; i < strideSource.size(); ++i) {
		const u8 value = static_cast<u8>(i & 0xFFU);
		strideSource[i] = (static_cast<u32>(value) << 24U)
			| (static_cast<u32>(value) << 16U)
			| (static_cast<u32>(value) << 8U)
			| 0xFFU;
	}
	rvk2::VIFrameInput strideInput{};
	strideInput.sourceWidth = 8U;
	strideInput.sourceHeight = 4U;
	strideInput.sourcePixels = &strideSource;
	strideInput.registers.valid = true;
	strideInput.registers.status = 3U | (3U << 8U);
	strideInput.registers.width = 4U;
	strideInput.registers.vSync = 525U;
	strideInput.registers.hStart = (0U << 16U) | 2U;
	strideInput.registers.vStart = (0U << 16U) | 4U;
	strideInput.registers.xScale = 2048U;
	strideInput.registers.yScale = 2048U;
	std::vector<u32> stridePixels;
	const rvk2::VIFrameSummary strideSummary =
		rendererSquare.present(strideInput, &stridePixels);
	expectEq(strideSummary.presentWidth, 2U, "VIRenderer stride width mismatch");
	expectEq(strideSummary.presentHeight, 2U, "VIRenderer stride height mismatch");
	expectEq(stridePixels.size(), static_cast<size_t>(4U), "VIRenderer stride pixel count mismatch");
	expectEq(stridePixels[0], strideSource[0], "VIRenderer stride sample (0,0) mismatch");
	expectEq(stridePixels[1], strideSource[2], "VIRenderer stride sample (1,0) mismatch");
	expectEq(stridePixels[2], strideSource[8], "VIRenderer stride sample (0,1) mismatch");
	expectEq(stridePixels[3], strideSource[10], "VIRenderer stride sample (1,1) mismatch");

	std::vector<u32> strideShortSource{
		0x101010FFU, 0x202020FFU, 0x303030FFU, 0x404040FFU,
		0x505050FFU, 0x606060FFU, 0x707070FFU, 0x808080FFU
	};
	rvk2::VIFrameInput strideOverflowInput{};
	strideOverflowInput.sourceWidth = 4U;
	strideOverflowInput.sourceHeight = 2U;
	strideOverflowInput.sourcePixels = &strideShortSource;
	strideOverflowInput.registers.valid = true;
	strideOverflowInput.registers.status = 3U | (3U << 8U);
	strideOverflowInput.registers.width = 8U;
	strideOverflowInput.registers.vSync = 525U;
	strideOverflowInput.registers.hStart = (0U << 16U) | 2U;
	strideOverflowInput.registers.vStart = (0U << 16U) | 4U;
	strideOverflowInput.registers.xScale = 2048U;
	strideOverflowInput.registers.yScale = 2048U;
	std::vector<u32> strideOverflowPixels;
	const rvk2::VIFrameSummary strideOverflowSummary =
		rendererSquare.present(strideOverflowInput, &strideOverflowPixels);
	expectEq(strideOverflowSummary.presentWidth, 2U, "VIRenderer stride overflow width mismatch");
	expectEq(strideOverflowSummary.presentHeight, 2U, "VIRenderer stride overflow height mismatch");
	expectEq(
		strideOverflowPixels.size(),
		static_cast<size_t>(4U),
		"VIRenderer stride overflow pixel count mismatch");
	expectEq(
		strideOverflowPixels[0],
		strideShortSource[0],
		"VIRenderer stride overflow sample (0,0) mismatch");
	expectEq(
		strideOverflowPixels[1],
		strideShortSource[2],
		"VIRenderer stride overflow sample (1,0) mismatch");
	expectEq(
		strideOverflowPixels[2],
		0x00000000U,
		"VIRenderer stride overflow sample (0,1) mismatch");
	expectEq(
		strideOverflowPixels[3],
		0x00000000U,
		"VIRenderer stride overflow sample (1,1) mismatch");

	rvk2::VIFrameInput wrappedVStartInput{};
	wrappedVStartInput.sourceWidth = 2U;
	wrappedVStartInput.sourceHeight = 2U;
	wrappedVStartInput.sourcePixels = &interlaceSource;
	wrappedVStartInput.registers.valid = true;
	wrappedVStartInput.registers.status = 3U | (3U << 8U);
	wrappedVStartInput.registers.width = 2U;
	wrappedVStartInput.registers.vSync = 5U;
	wrappedVStartInput.registers.hStart = (0U << 16U) | 2U;
	wrappedVStartInput.registers.vStart = (4U << 16U) | 2U;
	wrappedVStartInput.registers.xScale = 1024U;
	wrappedVStartInput.registers.yScale = 1024U;
	const rvk2::VIFrameSummary wrappedVStartSummary = rendererSquare.present(wrappedVStartInput);
	expectEq(wrappedVStartSummary.presentWidth, 2U, "VIRenderer wrapped vStart width mismatch");
	expectEq(wrappedVStartSummary.presentHeight, 2U, "VIRenderer wrapped vStart height mismatch");

	rvk2::VIFrameInput invalidWidthInput = wrappedVStartInput;
	invalidWidthInput.registers.width = 0U;
	const rvk2::VIFrameSummary invalidWidthSummary = rendererSquare.present(invalidWidthInput);
	expectEq(invalidWidthSummary.presentWidth, 0U, "VIRenderer zero VI width mismatch");
	expectEq(invalidWidthSummary.presentHeight, 0U, "VIRenderer zero VI height mismatch");

	rvk2::VIFrameInput invalidStride16Input = wrappedVStartInput;
	invalidStride16Input.registers.status = 2U | (3U << 8U);
	invalidStride16Input.registers.width = 2U;
	const rvk2::VIFrameSummary invalidStride16Summary = rendererSquare.present(invalidStride16Input);
	expectEq(invalidStride16Summary.presentWidth, 0U, "VIRenderer invalid 16bpp stride width mismatch");
	expectEq(invalidStride16Summary.presentHeight, 0U, "VIRenderer invalid 16bpp stride height mismatch");

	rvk2::VIFrameInput invalidStride32Input = wrappedVStartInput;
	invalidStride32Input.registers.status = 3U | (3U << 8U);
	invalidStride32Input.registers.width = 1U;
	const rvk2::VIFrameSummary invalidStride32Summary = rendererSquare.present(invalidStride32Input);
	expectEq(invalidStride32Summary.presentWidth, 0U, "VIRenderer invalid 32bpp stride width mismatch");
	expectEq(invalidStride32Summary.presentHeight, 0U, "VIRenderer invalid 32bpp stride height mismatch");

	rvk2::VIFrameInput invalidWindowInput = clipInput;
	invalidWindowInput.registers.xScale = 1024U;
	invalidWindowInput.registers.yScale = 1024U;
	invalidWindowInput.registers.hStart = (4U << 16U) | 4U;
	const rvk2::VIFrameSummary invalidHWindowSummary = rendererSquare.present(invalidWindowInput);
	expectEq(invalidHWindowSummary.presentWidth, 0U, "VIRenderer invalid H window width mismatch");
	expectEq(invalidHWindowSummary.presentHeight, 0U, "VIRenderer invalid H window height mismatch");

	invalidWindowInput.registers.hStart = (0U << 16U) | 2U;
	invalidWindowInput.registers.vStart = (4U << 16U) | 4U;
	const rvk2::VIFrameSummary invalidVWindowSummary = rendererSquare.present(invalidWindowInput);
	expectEq(invalidVWindowSummary.presentWidth, 0U, "VIRenderer invalid V window width mismatch");
	expectEq(invalidVWindowSummary.presentHeight, 0U, "VIRenderer invalid V window height mismatch");

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

	rvk2::ExecutorConfig viRangeConfig = viConfig;
	viRangeConfig.viOrigin = fillA.colorImageAddress + 4U;
	rvk2::Executor viRangeExecutor(viRangeConfig);
	const rvk2::ExecutorOutput viRangeOut =
		viRangeExecutor.executeWithOutput(workPackets, batches);
	expectTrue(
		!viRangeOut.presentFrame.pixels.empty(),
		"Executor VI-range output should produce present pixels");
	expectEq(
		viRangeOut.presentFrame.pixels[0],
		fillA.fillColor,
		"Executor VI origin in-range should select containing render target");

	rvk2::ExecutorConfig viNoMatchConfig = viConfig;
	viNoMatchConfig.viOrigin = 0x00300000U;
	rvk2::Executor viNoMatchExecutor(viNoMatchConfig);
	const rvk2::ExecutorOutput viNoMatchOut =
		viNoMatchExecutor.executeWithOutput(workPackets, batches);
	expectTrue(
		!viNoMatchOut.presentFrame.pixels.empty(),
		"Executor VI no-match output should produce present pixels");
	expectEq(
		viNoMatchOut.presentFrame.pixels[0],
		fillB.fillColor,
		"Executor VI no-match should fall back to last render target");
	expectEq(
		viNoMatchOut.summary.presentHash,
		defaultOut.summary.presentHash,
		"Executor VI no-match hash should match default presentation");
}

void testExecutorVIOriginWidthPreference()
{
	auto makeFillWork = [](
		u64 _packetId,
		u32 _colorAddress,
		u32 _fillColor,
		u16 _colorWidth,
		u32 _lrx,
		u32 _lry) -> rvk2::RenderWorkPacket {
		rvk2::RenderWorkPacket work{};
		work.sourcePacketId = _packetId;
		work.sourceOpcode = 0x36U;
		work.opKind = static_cast<u8>(rvk2::RasterOpKind::kFillRect);
		work.phase = static_cast<u8>(rvk2::RenderPhase::kFill);
		work.cycleType = 3U;
		work.rectULX = 0U;
		work.rectULY = 0U;
		work.rectLRX = _lrx;
		work.rectLRY = _lry;
		work.colorImageFormat = 0U;
		work.colorImageSize = 3U;
		work.colorImageWidth = _colorWidth;
		work.colorImageAddress = _colorAddress;
		work.fillColor = _fillColor;
		return work;
	};

	// Narrow surface starts far earlier but still contains VI origin in-range.
	// Wide surface starts close to VI origin. Width preference should decide.
	const rvk2::RenderWorkPacket fillNarrow = makeFillWork(
		10ULL,
		0x00100000U,
		0xC02020FFU,
		2U,
		1U,
		80U);
	const rvk2::RenderWorkPacket fillWide = makeFillWork(
		11ULL,
		0x00100100U,
		0x20C020FFU,
		8U,
		7U,
		3U);

	const std::vector<rvk2::RenderWorkPacket> workPackets{fillNarrow, fillWide};
	rvk2::SubmissionBatchPacket batch{};
	batch.batchIndex = 0U;
	batch.phase = static_cast<u8>(rvk2::RenderPhase::kFill);
	batch.cycleType = 3U;
	batch.firstWorkIndex = 0U;
	batch.lastWorkIndex = 1U;
	batch.workCount = 2U;
	const std::vector<rvk2::SubmissionBatchPacket> batches{batch};

	rvk2::ExecutorConfig baseConfig{};
	baseConfig.presentAspectX = 1U;
	baseConfig.presentAspectY = 1U;
	rvk2::Executor baseExecutor(baseConfig);
	const rvk2::ExecutorOutput baseOut =
		baseExecutor.executeWithOutput(workPackets, batches);
	expectTrue(
		!baseOut.presentFrame.pixels.empty(),
		"Executor width preference baseline should produce present pixels");

	rvk2::ExecutorConfig viNarrowConfig = baseConfig;
	viNarrowConfig.viRegistersValid = true;
	viNarrowConfig.viStatus = 3U;
	viNarrowConfig.viOrigin = 0x00100108U;
	viNarrowConfig.viWidth = 2U;
	viNarrowConfig.viVSync = 525U;
	viNarrowConfig.viHStart = (0U << 16U) | 2U;
	viNarrowConfig.viVStart = (0U << 16U) | 4U;
	viNarrowConfig.viXScale = 1024U;
	viNarrowConfig.viYScale = 1024U;
	rvk2::Executor viNarrowExecutor(viNarrowConfig);
	const rvk2::ExecutorOutput viNarrowOut =
		viNarrowExecutor.executeWithOutput(workPackets, batches);
	expectTrue(
		!viNarrowOut.presentFrame.pixels.empty(),
		"Executor width preference (narrow) should produce present pixels");
	expectEq(
		viNarrowOut.presentFrame.pixels[0],
		fillNarrow.fillColor,
		"Executor VI width preference should choose in-range surface matching VI width");
	expectEq(
		viNarrowOut.summary.selectedPresentSurfaceAddress,
		fillNarrow.colorImageAddress,
		"Executor VI width preference should select narrow surface address");

	rvk2::ExecutorConfig viWideConfig = viNarrowConfig;
	viWideConfig.viWidth = 8U;
	rvk2::Executor viWideExecutor(viWideConfig);
	const rvk2::ExecutorOutput viWideOut =
		viWideExecutor.executeWithOutput(workPackets, batches);
	expectTrue(
		!viWideOut.presentFrame.pixels.empty(),
		"Executor width preference (wide) should produce present pixels");
	expectEq(
		viWideOut.presentFrame.pixels[0],
		fillWide.fillColor,
		"Executor VI width preference should pick wide surface when width matches");
	expectEq(
		viWideOut.summary.selectedPresentSurfaceAddress,
		fillWide.colorImageAddress,
		"Executor VI width preference should select wide surface address");
}

void testExecutorPreviousSurfaceFallback()
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

	const rvk2::RenderWorkPacket fill = makeFillWork(1ULL, 0x00100000U, 0x11223344U);
	rvk2::SubmissionBatchPacket batch{};
	batch.batchIndex = 0U;
	batch.phase = static_cast<u8>(rvk2::RenderPhase::kFill);
	batch.cycleType = 3U;
	batch.firstWorkIndex = 0U;
	batch.lastWorkIndex = 0U;
	batch.workCount = 1U;
	const std::vector<rvk2::RenderWorkPacket> firstWorkPackets{fill};
	const std::vector<rvk2::SubmissionBatchPacket> firstBatches{batch};

	rvk2::ExecutorConfig config{};
	config.presentAspectX = 1U;
	config.presentAspectY = 1U;
	rvk2::Executor executor(config);
	const rvk2::ExecutorOutput firstOut =
		executor.executeWithOutput(firstWorkPackets, firstBatches);
	expectTrue(
		!firstOut.presentFrame.pixels.empty(),
		"Executor first frame should produce present pixels");
	expectEq(
		firstOut.summary.presentSelectionReason,
		static_cast<u8>(rvk2::kExecutorPresentSelectionLastSurface),
		"Executor first frame should present current surface");

	const std::vector<rvk2::RenderWorkPacket> emptyWorkPackets;
	const std::vector<rvk2::SubmissionBatchPacket> emptyBatches;
	const rvk2::ExecutorOutput secondOut =
		executor.executeWithOutput(emptyWorkPackets, emptyBatches);
	expectTrue(
		!secondOut.presentFrame.pixels.empty(),
		"Executor no-work frame should still present previous surface");
	expectEq(
		secondOut.summary.presentSelectionReason,
		static_cast<u8>(rvk2::kExecutorPresentSelectionPreviousSurface),
		"Executor no-work frame should mark previous-surface selection");
	expectEq(
		secondOut.summary.presentHash,
		firstOut.summary.presentHash,
		"Executor no-work frame should preserve previous present hash");
}

void testExecutorVIOriginHistorySelectionPreservesReason()
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

	rvk2::SubmissionBatchPacket batch{};
	batch.batchIndex = 0U;
	batch.phase = static_cast<u8>(rvk2::RenderPhase::kFill);
	batch.cycleType = 3U;
	batch.firstWorkIndex = 0U;
	batch.lastWorkIndex = 0U;
	batch.workCount = 1U;
	const std::vector<rvk2::SubmissionBatchPacket> batches{batch};

	const rvk2::RenderWorkPacket fillA = makeFillWork(1ULL, 0x00100000U, 0xA01020FFU);
	const rvk2::RenderWorkPacket fillB = makeFillWork(2ULL, 0x00200000U, 0x10A020FFU);
	const std::vector<rvk2::RenderWorkPacket> firstWorkPackets{fillA};
	const std::vector<rvk2::RenderWorkPacket> secondWorkPackets{fillB};

	rvk2::ExecutorConfig config{};
	config.presentAspectX = 1U;
	config.presentAspectY = 1U;
	config.viRegistersValid = true;
	config.viStatus = 3U;
	config.viOrigin = fillA.colorImageAddress + 4U;
	config.viWidth = 2U;
	config.viVSync = 525U;
	config.viHStart = (0U << 16U) | 2U;
	config.viVStart = (0U << 16U) | 4U;
	config.viXScale = 1024U;
	config.viYScale = 1024U;
	rvk2::Executor executor(config);

	const rvk2::ExecutorOutput firstOut =
		executor.executeWithOutput(firstWorkPackets, batches);
	expectTrue(
		!firstOut.presentFrame.pixels.empty(),
		"Executor history-selection setup frame should produce present pixels");
	expectEq(
		firstOut.summary.presentSelectionReason,
		static_cast<u8>(rvk2::kExecutorPresentSelectionVIOriginRange),
		"Executor setup frame should classify as VI-origin range");

	const rvk2::ExecutorOutput secondOut =
		executor.executeWithOutput(secondWorkPackets, batches);
	expectTrue(
		!secondOut.presentFrame.pixels.empty(),
		"Executor history-selection frame should produce present pixels");
	expectEq(
		secondOut.presentFrame.pixels[0],
		fillA.fillColor,
		"Executor should present cached history surface when VI origin points to previous buffer");
	expectEq(
		secondOut.summary.selectedPresentSurfaceAddress,
		fillA.colorImageAddress,
		"Executor should keep selected address at history-matched surface");
	expectEq(
		secondOut.summary.presentSelectionReason,
		static_cast<u8>(rvk2::kExecutorPresentSelectionVIOriginRange),
		"Executor should preserve VI-origin range reason for history-selected surface");
	expectEq(
		secondOut.summary.viOriginMatchedSurface,
		static_cast<u8>(1U),
		"Executor should keep VI-origin match flag for history-selected surface");
}

void testExecutorSurfaceHistoryEvictionDeterminism()
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

	const u32 addresses[7] = {
		0x00100000U,
		0x00101000U,
		0x00102000U,
		0x00103000U,
		0x00104000U,
		0x00105000U,
		0x00106000U
	};
	const u32 colors[7] = {
		0xA01020FFU,
		0x20A010FFU,
		0x1020A0FFU,
		0xB04020FFU,
		0x40B020FFU,
		0x2040B0FFU,
		0xE06030FFU
	};

	std::vector<rvk2::RenderWorkPacket> setupWorkPackets;
	setupWorkPackets.reserve(7U);
	for (u32 i = 0U; i < 7U; ++i)
		setupWorkPackets.push_back(makeFillWork(static_cast<u64>(100U + i), addresses[i], colors[i]));

	rvk2::SubmissionBatchPacket setupBatch{};
	setupBatch.batchIndex = 0U;
	setupBatch.phase = static_cast<u8>(rvk2::RenderPhase::kFill);
	setupBatch.cycleType = 3U;
	setupBatch.firstWorkIndex = 0U;
	setupBatch.lastWorkIndex = static_cast<u32>(setupWorkPackets.size() - 1U);
	setupBatch.workCount = static_cast<u32>(setupWorkPackets.size());
	const std::vector<rvk2::SubmissionBatchPacket> setupBatches{setupBatch};

	rvk2::ExecutorConfig config{};
	config.presentAspectX = 1U;
	config.presentAspectY = 1U;
	rvk2::Executor executor(config);
	const rvk2::ExecutorOutput setupOut =
		executor.executeWithOutput(setupWorkPackets, setupBatches);
	expectTrue(
		!setupOut.presentFrame.pixels.empty(),
		"Executor history eviction setup frame should produce present pixels");

	rvk2::ExecutorConfig viConfig = config;
	viConfig.viRegistersValid = true;
	viConfig.viStatus = 3U;
	viConfig.viOrigin = addresses[0];
	viConfig.viWidth = 2U;
	viConfig.viVSync = 525U;
	viConfig.viHStart = (0U << 16U) | 2U;
	viConfig.viVStart = (0U << 16U) | 4U;
	viConfig.viXScale = 1024U;
	viConfig.viYScale = 1024U;
	executor.updateConfig(viConfig);

	const std::vector<rvk2::RenderWorkPacket> emptyWorkPackets;
	const std::vector<rvk2::SubmissionBatchPacket> emptyBatches;
	const rvk2::ExecutorOutput queryOut =
		executor.executeWithOutput(emptyWorkPackets, emptyBatches);
	expectTrue(
		!queryOut.presentFrame.pixels.empty(),
		"Executor history eviction query frame should produce present pixels");
	expectEq(
		queryOut.summary.presentSelectionReason,
		static_cast<u8>(rvk2::kExecutorPresentSelectionVIOriginNearest),
		"Executor history eviction should deterministically resolve to nearest address after trim");
	expectEq(
		queryOut.summary.selectedPresentSurfaceAddress,
		addresses[1],
		"Executor history eviction should deterministically evict lowest-address equal-age history slot");
	expectEq(
		queryOut.presentFrame.pixels[0],
		colors[1],
		"Executor history eviction deterministic nearest selection should present expected surface color");
}

void testExecutorTriangleCoefficientConsumption()
{
	auto makeCycle2CombinerMux = [](
		u8 colorA,
		u8 colorB,
		u8 colorC,
		u8 colorD,
		u8 alphaA,
		u8 alphaB,
		u8 alphaC,
		u8 alphaD) -> u64 {
		u32 mode0 = 0U;
		u32 mode1 = 0U;
		mode0 |= (static_cast<u32>(colorA & 0xFU) << 5U);
		mode0 |= static_cast<u32>(colorC & 0x1FU);
		mode1 |= (static_cast<u32>(colorB & 0xFU) << 24U);
		mode1 |= (static_cast<u32>(colorD & 0x7U) << 6U);
		mode1 |= (static_cast<u32>(alphaA & 0x7U) << 21U);
		mode1 |= (static_cast<u32>(alphaB & 0x7U) << 3U);
		mode1 |= (static_cast<u32>(alphaC & 0x7U) << 18U);
		mode1 |= static_cast<u32>(alphaD & 0x7U);
		return (static_cast<u64>(mode0) << 32U) | static_cast<u64>(mode1);
	};

	auto makeTriangleWork = [&]() -> rvk2::RenderWorkPacket {
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
		// 1-cycle mode uses the cycle-2 combiner configuration.
		work.combineMux = makeCycle2CombinerMux(
			4U, // A: SHADE
			0U, // B: COMBINED (undefined first cycle in hardware)
			6U, // C: 1
			0U, // D: COMBINED (undefined first cycle in hardware)
			4U, // alpha A: SHADE alpha
			7U, // alpha B: 0
			6U, // alpha C: 1
			0U  // alpha D: COMBINED alpha
		);
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
	texWorkA.combineMux = makeCycle2CombinerMux(
		1U, // A: TEX0
		0U, // B: COMBINED
		6U, // C: 1
		0U, // D: COMBINED
		1U, // alpha A: TEX0 alpha
		7U, // alpha B: 0
		6U, // alpha C: 1
		0U  // alpha D: COMBINED alpha
	);
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

void testSubmissionPlanSplitClassification()
{
	auto makeWork = []() -> rvk2::RenderWorkPacket {
		rvk2::RenderWorkPacket work{};
		work.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
		work.cycleType = 0U;
		work.barrierMask = rvk2::render_barrier::kNone;
		work.colorImageFormat = 0U;
		work.colorImageSize = 3U;
		work.colorImageWidth = 320U;
		work.colorImageAddress = 0x00100000U;
		work.depthImageAddress = 0x00200000U;
		work.scissorMode = 0U;
		work.scissorXH = 0U;
		work.scissorYH = 0U;
		work.scissorXL = 319U;
		work.scissorYL = 239U;
		work.sourcePacketId = 1ULL;
		return work;
	};

	{
		std::vector<rvk2::SubmissionBatchPacket> batches;
		rvk2::RenderWorkPacket first = makeWork();
		rvk2::RenderWorkPacket appended = first;
		appended.sourcePacketId = 2ULL;
		appended.textured = true;
		appended.depthTest = true;
		rvk2::appendRenderWorkToSubmissionPlan(first, 0U, batches);
		rvk2::appendRenderWorkToSubmissionPlan(appended, 1U, batches);
		expectEq(batches.size(), static_cast<size_t>(1U), "submission append should keep same-state work in one batch");
		expectEq(batches[0].splitReason, static_cast<u8>(rvk2::SubmissionSplitReason::kStart), "submission start split reason mismatch");
		expectEq(batches[0].workCount, 2U, "submission append work count mismatch");
		expectEq(batches[0].firstWorkIndex, 0U, "submission append first work index mismatch");
		expectEq(batches[0].lastWorkIndex, 1U, "submission append last work index mismatch");
		expectEq(batches[0].texturedWorkCount, 1U, "submission append textured count mismatch");
		expectEq(batches[0].depthTestWorkCount, 1U, "submission append depth count mismatch");
		expectEq(batches[0].firstSourcePacketId, 1ULL, "submission append first source packet mismatch");
		expectEq(batches[0].lastSourcePacketId, 2ULL, "submission append last source packet mismatch");
	}

	{
		std::vector<rvk2::SubmissionBatchPacket> batches;
		rvk2::RenderWorkPacket first = makeWork();
		rvk2::RenderWorkPacket barrier = first;
		barrier.sourcePacketId = 2ULL;
		barrier.barrierMask = rvk2::render_barrier::kLoadSync;
		rvk2::appendRenderWorkToSubmissionPlan(first, 0U, batches);
		rvk2::appendRenderWorkToSubmissionPlan(barrier, 1U, batches);
		expectEq(batches.size(), static_cast<size_t>(2U), "barrier split should create a new batch");
		expectEq(
			batches[1].splitReason,
			static_cast<u8>(rvk2::SubmissionSplitReason::kBarrier),
			"barrier split reason mismatch");
		expectEq(
			batches[1].splitBarrierMask,
			static_cast<u8>(rvk2::render_barrier::kLoadSync),
			"barrier split mask mismatch");
	}

	{
		std::vector<rvk2::SubmissionBatchPacket> batches;
		rvk2::RenderWorkPacket first = makeWork();
		rvk2::RenderWorkPacket phaseChange = first;
		phaseChange.sourcePacketId = 2ULL;
		phaseChange.phase = static_cast<u8>(rvk2::RenderPhase::kCopy);
		rvk2::appendRenderWorkToSubmissionPlan(first, 0U, batches);
		rvk2::appendRenderWorkToSubmissionPlan(phaseChange, 1U, batches);
		expectEq(
			batches[1].splitReason,
			static_cast<u8>(rvk2::SubmissionSplitReason::kPhaseChange),
			"phase-change split reason mismatch");
	}

	{
		std::vector<rvk2::SubmissionBatchPacket> batches;
		rvk2::RenderWorkPacket first = makeWork();
		rvk2::RenderWorkPacket cycleChange = first;
		cycleChange.sourcePacketId = 2ULL;
		cycleChange.cycleType = 1U;
		rvk2::appendRenderWorkToSubmissionPlan(first, 0U, batches);
		rvk2::appendRenderWorkToSubmissionPlan(cycleChange, 1U, batches);
		expectEq(
			batches[1].splitReason,
			static_cast<u8>(rvk2::SubmissionSplitReason::kCycleTypeChange),
			"cycle-type split reason mismatch");
	}

	{
		std::vector<rvk2::SubmissionBatchPacket> batches;
		rvk2::RenderWorkPacket first = makeWork();
		rvk2::RenderWorkPacket rtChange = first;
		rtChange.sourcePacketId = 2ULL;
		rtChange.colorImageAddress ^= 0x1000U;
		rvk2::appendRenderWorkToSubmissionPlan(first, 0U, batches);
		rvk2::appendRenderWorkToSubmissionPlan(rtChange, 1U, batches);
		expectEq(
			batches[1].splitReason,
			static_cast<u8>(rvk2::SubmissionSplitReason::kRenderTargetChange),
			"render-target split reason mismatch");
	}

	{
		std::vector<rvk2::SubmissionBatchPacket> batches;
		rvk2::RenderWorkPacket first = makeWork();
		rvk2::RenderWorkPacket scissorChange = first;
		scissorChange.sourcePacketId = 2ULL;
		scissorChange.scissorXL = 255U;
		rvk2::appendRenderWorkToSubmissionPlan(first, 0U, batches);
		rvk2::appendRenderWorkToSubmissionPlan(scissorChange, 1U, batches);
		expectEq(
			batches[1].splitReason,
			static_cast<u8>(rvk2::SubmissionSplitReason::kScissorChange),
			"scissor split reason mismatch");
	}

	{
		std::vector<rvk2::SubmissionBatchPacket> batches;
		rvk2::RenderWorkPacket unknown = makeWork();
		unknown.phase = static_cast<u8>(rvk2::RenderPhase::kUnknown);
		expectTrue(!rvk2::isSubmittableRenderWork(unknown), "unknown phase should not be submittable");
		rvk2::appendRenderWorkToSubmissionPlan(unknown, 0U, batches);
		expectEq(batches.size(), static_cast<size_t>(0U), "unknown phase should not emit submission batches");
	}
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

void testTextureReplacementContract()
{
	rvk2::RenderWorkPacket base{};
	base.textured = true;
	base.textureImageFormat = 2U;
	base.textureImageSize = 2U;
	base.textureImageWidth = 64U;
	base.textureImageAddress = 0x00123400U;
	base.tile = 3U;
	base.tileFormat = 2U;
	base.tileSize = 2U;
	base.tileLine = 32U;
	base.tileTmem = 0x80U;
	base.tilePalette = 5U;
	base.tileCms = 1U;
	base.tileCmt = 2U;
	base.tileMasks = 4U;
	base.tileMaskt = 5U;
	base.tileShifts = 1U;
	base.tileShiftt = 2U;
	base.tileULS = 0U;
	base.tileULT = 0U;
	base.tileLRS = 124U;
	base.tileLRT = 60U;
	base.tmemLoadKind = static_cast<u8>(rvk2::TmemLoadKind::kTLUT);
	base.tmemLoadTile = 3U;
	base.tmemLoadULS = 0U;
	base.tmemLoadULT = 0U;
	base.tmemLoadLRS = 124U;
	base.tmemLoadLRT = 60U;
	base.tmemLoadDXT = 44U;
	base.keyState = 0x0F0E0D0C0B0A0908ULL;
	base.convertState = 0x0102030405060708ULL;

	rvk2::TextureReplacementRequest requestA{};
	requestA.work = &base;
	requestA.s = 17;
	requestA.t = -9;
	requestA.w = 33;
	requestA.perspective = true;

	const rvk2::TextureReplacementKey keyA =
		rvk2::buildTextureReplacementKey(requestA);
	const rvk2::TextureReplacementKey keyB =
		rvk2::buildTextureReplacementKey(requestA);
	expectEq(
		keyA.deterministicKey,
		keyB.deterministicKey,
		"texture replacement key must be deterministic for stable input");
	expectEq(
		keyA.nativeTextureHash,
		keyB.nativeTextureHash,
		"texture replacement native hash must be deterministic");
	expectEq(
		keyA.paletteHash,
		keyB.paletteHash,
		"texture replacement palette hash must be deterministic");
	expectTrue(
		keyA.nativeWidth > 0U && keyA.nativeHeight > 0U,
		"texture replacement key must expose positive native dimensions");

	rvk2::TextureReplacementCacheKey cacheA =
		rvk2::buildTextureReplacementCacheKey(keyA);
	rvk2::TextureReplacementCacheKey cacheB =
		rvk2::buildTextureReplacementCacheKey(keyB);
	expectEq(
		cacheA.hi,
		cacheB.hi,
		"texture replacement cache key hi must be deterministic");
	expectEq(
		cacheA.lo,
		cacheB.lo,
		"texture replacement cache key lo must be deterministic");

	rvk2::RenderWorkPacket textureAddressVariant = base;
	textureAddressVariant.textureImageAddress ^= 0x1000U;
	rvk2::TextureReplacementRequest requestTextureVariant = requestA;
	requestTextureVariant.work = &textureAddressVariant;
	const rvk2::TextureReplacementKey keyTextureVariant =
		rvk2::buildTextureReplacementKey(requestTextureVariant);
	expectTrue(
		keyTextureVariant.nativeTextureHash != keyA.nativeTextureHash,
		"texture replacement native hash must react to source image address changes");
	expectTrue(
		keyTextureVariant.deterministicKey != keyA.deterministicKey,
		"texture replacement deterministic key must react to source image address changes");

	rvk2::RenderWorkPacket paletteVariant = base;
	paletteVariant.tilePalette ^= 0x3U;
	rvk2::TextureReplacementRequest requestPaletteVariant = requestA;
	requestPaletteVariant.work = &paletteVariant;
	const rvk2::TextureReplacementKey keyPaletteVariant =
		rvk2::buildTextureReplacementKey(requestPaletteVariant);
	expectTrue(
		keyPaletteVariant.paletteHash != keyA.paletteHash,
		"texture replacement palette hash must react to palette selection changes");

	rvk2::TextureReplacementRequest requestCoordVariant = requestA;
	requestCoordVariant.s += 1;
	const rvk2::TextureReplacementKey keyCoordVariant =
		rvk2::buildTextureReplacementKey(requestCoordVariant);
	expectTrue(
		keyCoordVariant.deterministicKey != keyA.deterministicKey,
		"texture replacement deterministic key must react to coordinate changes");
}

void testTextureReplacementCacheIO()
{
	rvk2::TextureReplacementStore store;
	rvk2::TextureReplacementCacheKey keyA{0x1111222233334444ULL, 0xAAAABBBBCCCCDDDDULL};
	rvk2::TextureReplacementImage imageA{};
	imageA.width = 2U;
	imageA.height = 2U;
	imageA.pixels = {
		0x11223344U,
		0x55667788U,
		0x99AABBCCU,
		0xDDEEFF11U
	};
	expectTrue(
		store.insert(keyA, imageA),
		"texture replacement cache insert should accept valid image");

	rvk2::TextureReplacementCacheKey keyB{0x00000000000000FFULL, 0x0000000000000001ULL};
	rvk2::TextureReplacementImage imageB{};
	imageB.width = 1U;
	imageB.height = 1U;
	imageB.pixels = {0xCAFEBABEU};
	expectTrue(
		store.insert(keyB, imageB),
		"texture replacement cache insert should accept second valid image");
	expectEq(
		store.entryCount(),
		static_cast<size_t>(2U),
		"texture replacement cache should track entry count");

	const char * cachePath = "/tmp/rvk2_texture_replacement_unit.hts";
	std::remove(cachePath);
	expectTrue(
		rvk2::writeTextureReplacementHTS(cachePath, store),
		"texture replacement cache should serialize to hts");

	rvk2::TextureReplacementStore loaded;
	expectTrue(
		rvk2::loadTextureReplacementHTS(cachePath, loaded),
		"texture replacement cache should deserialize from hts");
	expectEq(
		loaded.entryCount(),
		store.entryCount(),
		"loaded texture replacement cache should preserve entry count");

	const rvk2::TextureReplacementImage * loadedA = loaded.find(keyA);
	expectTrue(
		loadedA != nullptr,
		"loaded texture replacement cache should include first key");
	if (loadedA != nullptr) {
		expectEq(loadedA->width, imageA.width, "loaded replacement width mismatch");
		expectEq(loadedA->height, imageA.height, "loaded replacement height mismatch");
		expectEq(
			loadedA->pixels[3],
			imageA.pixels[3],
			"loaded replacement pixel mismatch");
		expectEq(
			rvk2::sampleTextureReplacementImage(*loadedA, 0, 0),
			imageA.pixels[0],
			"replacement sampling at origin mismatch");
		expectEq(
			rvk2::sampleTextureReplacementImage(*loadedA, 64, 32),
			rvk2::sampleTextureReplacementImage(*loadedA, 0, 32),
			"replacement sampling wrap mismatch");
	}

	std::remove(cachePath);
}

void testTextureReplacementPackIngest()
{
	const char * packDir = "/tmp/rvk2_pack_unit";
	const char * indexPath = "/tmp/rvk2_pack_unit/rkv2_pack_index_v1.tsv";
	const char * imagePath = "/tmp/rvk2_pack_unit/sample.rgba32";
	std::remove(indexPath);
	std::remove(imagePath);
	if (::mkdir(packDir, 0700) != 0 && errno != EEXIST) {
		expectTrue(false, "texture replacement pack test mkdir failed");
		return;
	}

	rvk2::TextureReplacementCacheKey key{0x123456789ABCDEF0ULL, 0x0FEDCBA987654321ULL};
	std::FILE * imageFile = std::fopen(imagePath, "wb");
	expectTrue(imageFile != nullptr, "texture replacement pack image open failed");
	if (imageFile != nullptr) {
		const u8 bytes[8]{
			0x11U, 0x22U, 0x33U, 0x44U,
			0x55U, 0x66U, 0x77U, 0x88U
		};
		const size_t written = std::fwrite(bytes, 1U, sizeof(bytes), imageFile);
		std::fclose(imageFile);
		expectEq(
			written,
			sizeof(bytes),
			"texture replacement pack image write size mismatch");
	}

	std::FILE * indexFile = std::fopen(indexPath, "wb");
	expectTrue(indexFile != nullptr, "texture replacement pack index open failed");
	if (indexFile != nullptr) {
		std::fprintf(
			indexFile,
			"# hi\tlo\twidth\theight\trgba_file\n0x%016llX\t0x%016llX\t2\t1\tsample.rgba32\n",
			static_cast<unsigned long long>(key.hi),
			static_cast<unsigned long long>(key.lo));
		std::fclose(indexFile);
	}

	rvk2::TextureReplacementStore loaded;
	expectTrue(
		rvk2::loadTextureReplacementPack(packDir, loaded),
		"texture replacement pack ingest should load valid index");
	const rvk2::TextureReplacementImage * image = loaded.find(key);
	expectTrue(
		image != nullptr,
		"texture replacement pack ingest should expose indexed key");
	if (image != nullptr) {
		expectEq(image->width, static_cast<u16>(2U), "texture replacement pack width mismatch");
		expectEq(image->height, static_cast<u16>(1U), "texture replacement pack height mismatch");
		expectEq(
			image->pixels[0],
			0x44332211U,
			"texture replacement pack first pixel mismatch");
		expectEq(
			image->pixels[1],
			0x88776655U,
			"texture replacement pack second pixel mismatch");
	}

	std::remove(indexPath);
	std::remove(imagePath);
	std::remove(packDir);
}

void testTextureReplacementStoreLimits()
{
	rvk2::TextureReplacementStore store;
	for (u32 i = 0U; i < 4U; ++i) {
		rvk2::TextureReplacementCacheKey key{
			0x1000ULL + static_cast<u64>(i),
			0x2000ULL + static_cast<u64>(i)
		};
		rvk2::TextureReplacementImage image{};
		image.width = static_cast<u16>(i + 1U);
		image.height = 1U;
		image.pixels.resize(static_cast<size_t>(image.width), 0x10203040U + i);
		expectTrue(
			store.insert(key, image),
			"texture replacement limit setup insert should succeed");
	}
	expectEq(
		store.entryCount(),
		static_cast<size_t>(4U),
		"texture replacement limit setup entry count mismatch");
	expectEq(
		store.totalPixels(),
		static_cast<u64>(10ULL),
		"texture replacement limit setup pixel count mismatch");

	store.applyLimits(2U, 0ULL);
	expectEq(
		store.entryCount(),
		static_cast<size_t>(2U),
		"texture replacement entry limit should cap entries");
	expectEq(
		store.totalPixels(),
		static_cast<u64>(3ULL),
		"texture replacement entry limit should keep deterministic earliest keys");

	store.clear();
	for (u32 i = 0U; i < 4U; ++i) {
		rvk2::TextureReplacementCacheKey key{
			0x1000ULL + static_cast<u64>(i),
			0x2000ULL + static_cast<u64>(i)
		};
		rvk2::TextureReplacementImage image{};
		image.width = static_cast<u16>(i + 1U);
		image.height = 1U;
		image.pixels.resize(static_cast<size_t>(image.width), 0x55667788U + i);
		store.insert(key, image);
	}
	store.applyLimits(0U, 5ULL);
	expectEq(
		store.entryCount(),
		static_cast<size_t>(2U),
		"texture replacement pixel limit should cap entries by accumulated pixels");
	expectEq(
		store.totalPixels(),
		static_cast<u64>(3ULL),
		"texture replacement pixel limit should keep deterministic earliest keys");
}

void testExecutorTextureReplacementSampling()
{
	rvk2::RenderWorkPacket work{};
	work.sourcePacketId = 1ULL;
	work.sourceOpcode = 0x24U;
	work.opKind = static_cast<u8>(rvk2::RasterOpKind::kTexRect);
	work.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	work.cycleType = 0U;
	work.textured = true;
	work.tile = 0U;
	work.rectULX = 0U;
	work.rectULY = 0U;
	work.rectLRX = 1U;
	work.rectLRY = 1U;
	work.texS = 0;
	work.texT = 0;
	work.texDSDX = 0;
	work.texDTDY = 0;
	work.colorImageFormat = 0U;
	work.colorImageSize = 3U;
	work.colorImageWidth = 4U;
	work.colorImageAddress = 0x00330000U;
	work.textureImageFormat = 0U;
	work.textureImageSize = 2U;
	work.textureImageWidth = 4U;
	work.textureImageAddress = 0x00120000U;
	work.tileFormat = 0U;
	work.tileSize = 2U;
	work.tileLine = 1U;
	work.tileTmem = 0U;
	work.tilePalette = 0U;
	work.tileULS = 0U;
	work.tileULT = 0U;
	work.tileLRS = 4U;
	work.tileLRT = 4U;

	rvk2::TextureReplacementRequest request{};
	request.work = &work;
	request.s = 0;
	request.t = 0;
	request.w = 0;
	request.perspective = false;
	const rvk2::TextureReplacementKey replacementKey =
		rvk2::buildTextureReplacementKey(request);
	const rvk2::TextureReplacementCacheKey cacheKey =
		rvk2::buildTextureReplacementCacheKey(replacementKey);

	rvk2::TextureReplacementStore store;
	rvk2::TextureReplacementImage replacementImage{};
	replacementImage.width = 1U;
	replacementImage.height = 1U;
	replacementImage.pixels = {0x1A2B3CFFU};
	expectTrue(
		store.insert(cacheKey, replacementImage),
		"executor texture replacement setup insert should succeed");

	const char * cachePath = "/tmp/rvk2_executor_texture_replacement_unit.hts";
	std::remove(cachePath);
	expectTrue(
		rvk2::writeTextureReplacementHTS(cachePath, store),
		"executor texture replacement setup write should succeed");

	const char * packDir = "/tmp/rvk2_executor_pack_unit";
	const char * packIndexPath = "/tmp/rvk2_executor_pack_unit/rkv2_pack_index_v1.tsv";
	const char * packImagePath = "/tmp/rvk2_executor_pack_unit/repl.rgba32";
	std::remove(packIndexPath);
	std::remove(packImagePath);
	if (::mkdir(packDir, 0700) != 0 && errno != EEXIST) {
		expectTrue(false, "executor texture replacement pack mkdir failed");
		std::remove(cachePath);
		return;
	}
	std::FILE * packImageFile = std::fopen(packImagePath, "wb");
	expectTrue(packImageFile != nullptr, "executor texture replacement pack image open failed");
	if (packImageFile != nullptr) {
		const u8 bytes[4]{
			0xFFU, 0x00U, 0x00U, 0xFFU
		};
		const size_t written = std::fwrite(bytes, 1U, sizeof(bytes), packImageFile);
		std::fclose(packImageFile);
		expectEq(
			written,
			sizeof(bytes),
			"executor texture replacement pack image write size mismatch");
	}
	std::FILE * packIndexFile = std::fopen(packIndexPath, "wb");
	expectTrue(packIndexFile != nullptr, "executor texture replacement pack index open failed");
	if (packIndexFile != nullptr) {
		std::fprintf(
			packIndexFile,
			"0x%016llX\t0x%016llX\t1\t1\trepl.rgba32\n",
			static_cast<unsigned long long>(cacheKey.hi),
			static_cast<unsigned long long>(cacheKey.lo));
		std::fclose(packIndexFile);
	}

	rvk2::SubmissionBatchPacket batch{};
	batch.batchIndex = 0U;
	batch.firstWorkIndex = 0U;
	batch.lastWorkIndex = 0U;
	batch.workCount = 1U;
	batch.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	batch.cycleType = 0U;
	const std::vector<rvk2::RenderWorkPacket> workPackets{work};
	const std::vector<rvk2::SubmissionBatchPacket> batches{batch};

	rvk2::Executor baselineExecutor;
	const rvk2::ExecutorOutput baselineOut =
		baselineExecutor.executeWithOutput(workPackets, batches);

	rvk2::ExecutorConfig replacementConfig{};
	replacementConfig.textureReplacementEnable = true;
	replacementConfig.textureReplacementCachePath = cachePath;
	rvk2::Executor replacementExecutor(replacementConfig);
	const rvk2::ExecutorOutput replacementOut =
		replacementExecutor.executeWithOutput(workPackets, batches);
	rvk2::ExecutorConfig packConfig{};
	packConfig.textureReplacementEnable = true;
	packConfig.textureReplacementPackPath = packDir;
	rvk2::Executor packExecutor(packConfig);
	const rvk2::ExecutorOutput packOut =
		packExecutor.executeWithOutput(workPackets, batches);
	rvk2::ExecutorConfig bothConfig{};
	bothConfig.textureReplacementEnable = true;
	bothConfig.textureReplacementCachePath = cachePath;
	bothConfig.textureReplacementPackPath = packDir;
	rvk2::Executor bothExecutor(bothConfig);
	const rvk2::ExecutorOutput bothOut =
		bothExecutor.executeWithOutput(workPackets, batches);

	expectTrue(
		replacementOut.summary.colorWriteCount > 0ULL,
		"executor replacement scene should write pixels");
	expectTrue(
		!baselineOut.summary.textureReplacementEnabled,
		"executor baseline run should keep replacement disabled");
	expectEq(
		baselineOut.summary.textureReplacementEntryCount,
		0ULL,
		"executor baseline run should have zero replacement entries");
	expectEq(
		baselineOut.summary.textureReplacementSampleCount,
		0ULL,
		"executor baseline run should not sample replacement cache");
	expectEq(
		replacementOut.summary.colorWriteCount,
		baselineOut.summary.colorWriteCount,
		"executor replacement toggle should preserve write count");
	expectTrue(
		replacementOut.summary.textureReplacementEnabled,
		"executor replacement run should report replacement enabled");
	expectEq(
		replacementOut.summary.textureReplacementEntryCount,
		1ULL,
		"executor replacement run should load one replacement entry");
	expectEq(
		replacementOut.summary.textureReplacementPixelCount,
		1ULL,
		"executor replacement run should report replacement pixel count");
	expectTrue(
		replacementOut.summary.textureReplacementSampleCount > 0ULL,
		"executor replacement run should attempt replacement samples");
	expectEq(
		replacementOut.summary.textureReplacementHitCount,
		replacementOut.summary.textureReplacementSampleCount,
		"executor replacement run should hit replacement on each sample");
	expectEq(
		replacementOut.summary.textureReplacementMissCount,
		0ULL,
		"executor replacement run should not miss replacement lookups");
	expectTrue(
		replacementOut.summary.presentHash != baselineOut.summary.presentHash,
		"executor replacement toggle should alter present hash");
	expectEq(
		packOut.summary.textureReplacementEntryCount,
		1ULL,
		"executor pack replacement run should load one replacement entry");
	expectTrue(
		packOut.summary.textureReplacementHitCount > 0ULL,
		"executor pack replacement run should hit replacement lookups");
	expectEq(
		packOut.summary.textureReplacementMissCount,
		0ULL,
		"executor pack replacement run should not miss replacement lookups");
	expectTrue(
		packOut.summary.presentHash != baselineOut.summary.presentHash,
		"executor pack replacement toggle should alter present hash");
	expectTrue(
		packOut.summary.presentHash != replacementOut.summary.presentHash,
		"executor pack replacement should differ from cache replacement");
	expectEq(
		bothOut.summary.presentHash,
		packOut.summary.presentHash,
		"executor pack replacement should override cache when both are provided");
	expectEq(
		bothOut.summary.textureReplacementEntryCount,
		1ULL,
		"executor combined cache+pack run should keep deterministic replacement cardinality");

	std::remove(cachePath);
	std::remove(packIndexPath);
	std::remove(packImagePath);
	std::remove(packDir);
}

void testExecutorTextureReplacementLifecycle()
{
	rvk2::RenderWorkPacket work{};
	work.sourcePacketId = 1ULL;
	work.sourceOpcode = 0x24U;
	work.opKind = static_cast<u8>(rvk2::RasterOpKind::kTexRect);
	work.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	work.cycleType = 0U;
	work.textured = true;
	work.rectULX = 0U;
	work.rectULY = 0U;
	work.rectLRX = 1U;
	work.rectLRY = 1U;
	work.colorImageFormat = 0U;
	work.colorImageSize = 3U;
	work.colorImageWidth = 4U;
	work.colorImageAddress = 0x00331000U;
	work.textureImageFormat = 0U;
	work.textureImageSize = 2U;
	work.textureImageWidth = 4U;
	work.textureImageAddress = 0x00121000U;
	work.tileFormat = 0U;
	work.tileSize = 2U;
	work.tileLine = 1U;
	work.tileULS = 0U;
	work.tileULT = 0U;
	work.tileLRS = 4U;
	work.tileLRT = 4U;

	rvk2::TextureReplacementRequest request{};
	request.work = &work;
	request.s = 0;
	request.t = 0;
	request.w = 0;
	request.perspective = false;
	const rvk2::TextureReplacementCacheKey cacheKey =
		rvk2::buildTextureReplacementCacheKey(
			rvk2::buildTextureReplacementKey(request));

	auto writeSingleColorCache = [&](const char * _path, u32 _color) -> bool {
		rvk2::TextureReplacementStore store;
		rvk2::TextureReplacementImage image{};
		image.width = 1U;
		image.height = 1U;
		image.pixels = {_color};
		if (!store.insert(cacheKey, image))
			return false;
		return rvk2::writeTextureReplacementHTS(_path, store);
	};

	const char * cachePath = "/tmp/rvk2_executor_lifecycle.hts";
	std::remove(cachePath);
	expectTrue(
		writeSingleColorCache(cachePath, 0x001122FFU),
		"executor lifecycle setup cache write A should succeed");

	rvk2::SubmissionBatchPacket batch{};
	batch.batchIndex = 0U;
	batch.firstWorkIndex = 0U;
	batch.lastWorkIndex = 0U;
	batch.workCount = 1U;
	batch.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	const std::vector<rvk2::RenderWorkPacket> workPackets{work};
	const std::vector<rvk2::SubmissionBatchPacket> batches{batch};

	rvk2::ExecutorConfig config{};
	config.textureReplacementEnable = true;
	config.textureReplacementCachePath = cachePath;
	rvk2::Executor executor(config);
	const rvk2::ExecutorOutput outA =
		executor.executeWithOutput(workPackets, batches);
	expectTrue(
		outA.summary.presentHash != 1469598103934665603ULL,
		"executor lifecycle baseline should produce a present hash");

	expectTrue(
		writeSingleColorCache(cachePath, 0xFF0000FFU),
		"executor lifecycle setup cache write B should succeed");
	executor.updateConfig(config);
	const rvk2::ExecutorOutput outNoReload =
		executor.executeWithOutput(workPackets, batches);
	expectEq(
		outNoReload.summary.presentHash,
		outA.summary.presentHash,
		"executor lifecycle should not reload cache without reload token change");

	rvk2::ExecutorConfig reloadConfig = config;
	reloadConfig.textureReplacementReloadToken = 1ULL;
	executor.updateConfig(reloadConfig);
	const rvk2::ExecutorOutput outReload =
		executor.executeWithOutput(workPackets, batches);
	expectTrue(
		outReload.summary.presentHash != outA.summary.presentHash,
		"executor lifecycle reload token should force cache reload");

	rvk2::ExecutorConfig invalidateConfig = reloadConfig;
	invalidateConfig.textureReplacementInvalidateToken = 1ULL;
	invalidateConfig.textureReplacementCachePath.clear();
	executor.updateConfig(invalidateConfig);
	const rvk2::ExecutorOutput outInvalidate =
		executor.executeWithOutput(workPackets, batches);
	expectTrue(
		outInvalidate.summary.presentHash != outReload.summary.presentHash,
		"executor lifecycle invalidate should clear replacement participation");

	std::remove(cachePath);
}

void testExecutorConfigTextureReplacementControlFile()
{
	const char * controlPath = "/tmp/rvk2_tx_control_config_unit.txt";
	std::remove(controlPath);

	ScopedEnvVar envControl("REALITYVK_RVK2_TX_CONTROL_FILE");
	ScopedEnvVar envEnable("REALITYVK_RVK2_TEX_REPLACEMENT");
	ScopedEnvVar envCachePath("REALITYVK_RVK2_TX_HTS_PATH");
	ScopedEnvVar envPackPath("REALITYVK_RVK2_TX_PACK_PATH");
	ScopedEnvVar envMaxEntries("REALITYVK_RVK2_TX_MAX_ENTRIES");
	ScopedEnvVar envMaxPixels("REALITYVK_RVK2_TX_MAX_PIXELS");
	ScopedEnvVar envReloadToken("REALITYVK_RVK2_TX_RELOAD_TOKEN");
	ScopedEnvVar envInvalidateToken("REALITYVK_RVK2_TX_INVALIDATE_TOKEN");
	ScopedEnvVar envLogSummary("REALITYVK_RVK2_TX_LOG_SUMMARY");
	ScopedEnvVar envSummaryPath("REALITYVK_RVK2_TX_SUMMARY_PATH");

	envEnable.clear();
	envCachePath.clear();
	envPackPath.clear();
	envMaxEntries.clear();
	envMaxPixels.clear();
	envReloadToken.clear();
	envInvalidateToken.clear();
	envLogSummary.clear();
	envSummaryPath.clear();
	envControl.clear();

	envEnable.set("1");
	envReloadToken.set("99");
	envLogSummary.set("1");

	{
		std::FILE * control = std::fopen(controlPath, "wb");
		expectTrue(control != nullptr, "control-file config test open failed");
		if (control != nullptr) {
			std::fprintf(control, "enable=0\n");
			std::fprintf(control, "cache_path=/tmp/control_cache.hts\n");
			std::fprintf(control, "pack_path=/tmp/control_pack\n");
			std::fprintf(control, "max_entries=17\n");
			std::fprintf(control, "max_pixels=4096\n");
			std::fprintf(control, "reload_token=7\n");
			std::fprintf(control, "invalidate_token=5\n");
			std::fprintf(control, "log_summary=0\n");
			std::fprintf(control, "summary_path=/tmp/rvk2_tx_summary_unit.txt\n");
			std::fclose(control);
		}
	}
	envControl.set(controlPath);

	const rvk2::ExecutorConfig config = rvk2::loadExecutorConfigFromEnv();
	expectTrue(
		!config.textureReplacementEnable,
		"control-file config should allow explicit disable even with cache/pack paths");
	expectTrue(
		config.textureReplacementCachePath == "/tmp/control_cache.hts",
		"control-file config cache path mismatch");
	expectTrue(
		config.textureReplacementPackPath == "/tmp/control_pack",
		"control-file config pack path mismatch");
	expectEq(
		config.textureReplacementMaxEntries,
		17U,
		"control-file config max entries mismatch");
	expectEq(
		config.textureReplacementMaxPixels,
		4096ULL,
		"control-file config max pixels mismatch");
	expectEq(
		config.textureReplacementReloadToken,
		7ULL,
		"control-file config reload token mismatch");
	expectEq(
		config.textureReplacementInvalidateToken,
		5ULL,
		"control-file config invalidate token mismatch");
	expectTrue(
		!config.textureReplacementLogSummary,
		"control-file config should override env log summary");
	expectTrue(
		config.textureReplacementSummaryPath == "/tmp/rvk2_tx_summary_unit.txt",
		"control-file config summary path mismatch");

	std::remove(controlPath);
}

void testExecutorTextureReplacementControlFileLifecycle()
{
	rvk2::RenderWorkPacket work{};
	work.sourcePacketId = 1ULL;
	work.sourceOpcode = 0x24U;
	work.opKind = static_cast<u8>(rvk2::RasterOpKind::kTexRect);
	work.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	work.cycleType = 0U;
	work.textured = true;
	work.rectULX = 0U;
	work.rectULY = 0U;
	work.rectLRX = 1U;
	work.rectLRY = 1U;
	work.colorImageFormat = 0U;
	work.colorImageSize = 3U;
	work.colorImageWidth = 4U;
	work.colorImageAddress = 0x00332000U;
	work.textureImageFormat = 0U;
	work.textureImageSize = 2U;
	work.textureImageWidth = 4U;
	work.textureImageAddress = 0x00122000U;
	work.tileFormat = 0U;
	work.tileSize = 2U;
	work.tileLine = 1U;
	work.tileULS = 0U;
	work.tileULT = 0U;
	work.tileLRS = 4U;
	work.tileLRT = 4U;

	rvk2::TextureReplacementRequest request{};
	request.work = &work;
	request.s = 0;
	request.t = 0;
	request.w = 0;
	request.perspective = false;
	const rvk2::TextureReplacementCacheKey cacheKey =
		rvk2::buildTextureReplacementCacheKey(
			rvk2::buildTextureReplacementKey(request));

	auto writeSingleColorCache = [&](const char * _path, u32 _color) -> bool {
		rvk2::TextureReplacementStore store;
		rvk2::TextureReplacementImage image{};
		image.width = 1U;
		image.height = 1U;
		image.pixels = {_color};
		if (!store.insert(cacheKey, image))
			return false;
		return rvk2::writeTextureReplacementHTS(_path, store);
	};

	const char * cachePath = "/tmp/rvk2_executor_control_lifecycle.hts";
	const char * controlPath = "/tmp/rvk2_executor_control_lifecycle.txt";
	std::remove(cachePath);
	std::remove(controlPath);
	expectTrue(
		writeSingleColorCache(cachePath, 0x102030FFU),
		"control lifecycle setup cache write A should succeed");

	auto writeControlFile = [&](bool _enable, u64 _reloadToken, u64 _invalidateToken) -> bool {
		std::FILE * control = std::fopen(controlPath, "wb");
		if (control == nullptr)
			return false;
		std::fprintf(control, "enable=%u\n", _enable ? 1U : 0U);
		std::fprintf(control, "cache_path=%s\n", cachePath);
		std::fprintf(control, "reload_token=%llu\n", static_cast<unsigned long long>(_reloadToken));
		std::fprintf(control, "invalidate_token=%llu\n", static_cast<unsigned long long>(_invalidateToken));
		std::fclose(control);
		return true;
	};

	expectTrue(
		writeControlFile(true, 0ULL, 0ULL),
		"control lifecycle setup control write A should succeed");

	rvk2::SubmissionBatchPacket batch{};
	batch.batchIndex = 0U;
	batch.firstWorkIndex = 0U;
	batch.lastWorkIndex = 0U;
	batch.workCount = 1U;
	batch.phase = static_cast<u8>(rvk2::RenderPhase::kCycle1);
	const std::vector<rvk2::RenderWorkPacket> workPackets{work};
	const std::vector<rvk2::SubmissionBatchPacket> batches{batch};

	ScopedEnvVar envControl("REALITYVK_RVK2_TX_CONTROL_FILE");
	ScopedEnvVar envEnable("REALITYVK_RVK2_TEX_REPLACEMENT");
	ScopedEnvVar envCachePath("REALITYVK_RVK2_TX_HTS_PATH");
	ScopedEnvVar envPackPath("REALITYVK_RVK2_TX_PACK_PATH");
	ScopedEnvVar envReloadToken("REALITYVK_RVK2_TX_RELOAD_TOKEN");
	ScopedEnvVar envInvalidateToken("REALITYVK_RVK2_TX_INVALIDATE_TOKEN");

	envEnable.clear();
	envCachePath.clear();
	envPackPath.clear();
	envReloadToken.clear();
	envInvalidateToken.clear();
	envControl.set(controlPath);

	rvk2::Executor executor(rvk2::loadExecutorConfigFromEnv());
	const rvk2::ExecutorOutput outA =
		executor.executeWithOutput(workPackets, batches);
	expectTrue(
		outA.summary.textureReplacementEnabled,
		"control lifecycle baseline should enable replacement from control file");

	expectTrue(
		writeSingleColorCache(cachePath, 0xFF0000FFU),
		"control lifecycle setup cache write B should succeed");
	expectTrue(
		writeControlFile(true, 1ULL, 0ULL),
		"control lifecycle setup control write B should succeed");
	executor.updateConfig(rvk2::loadExecutorConfigFromEnv());
	const rvk2::ExecutorOutput outReload =
		executor.executeWithOutput(workPackets, batches);
	expectTrue(
		outReload.summary.presentHash != outA.summary.presentHash,
		"control lifecycle reload token change should alter replacement output");

	expectTrue(
		writeControlFile(false, 1ULL, 1ULL),
		"control lifecycle setup control write C should succeed");
	executor.updateConfig(rvk2::loadExecutorConfigFromEnv());
	const rvk2::ExecutorOutput outDisabled =
		executor.executeWithOutput(workPackets, batches);
	expectTrue(
		!outDisabled.summary.textureReplacementEnabled,
		"control lifecycle disable should deactivate replacement");
	expectEq(
		outDisabled.summary.textureReplacementEntryCount,
		0ULL,
		"control lifecycle disable should clear replacement entries");
	expectEq(
		outDisabled.summary.textureReplacementSampleCount,
		0ULL,
		"control lifecycle disable should stop replacement sampling");
	expectTrue(
		outDisabled.summary.presentHash != outReload.summary.presentHash,
		"control lifecycle disable should alter output from replacement run");

	std::remove(controlPath);
	std::remove(cachePath);
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
	testTriangleSignedYBounds();
	testSyntheticTrianglePacking();
	testSyntheticTrianglePerspectivePacking();
	testTexRectSemanticExtraction();
	testVIRendererAspectScaling();
	testExecutorVIOriginPresentationSelection();
	testExecutorVIOriginWidthPreference();
	testExecutorPreviousSurfaceFallback();
	testExecutorVIOriginHistorySelectionPreservesReason();
	testExecutorSurfaceHistoryEvictionDeterminism();
	testExecutorTriangleCoefficientConsumption();
	testSubmissionPlanSplitClassification();
	testExecutorFrameOutput();
	testTextureReplacementContract();
	testTextureReplacementCacheIO();
	testTextureReplacementPackIngest();
	testTextureReplacementStoreLimits();
	testExecutorTextureReplacementSampling();
	testExecutorTextureReplacementLifecycle();
	testExecutorConfigTextureReplacementControlFile();
	testExecutorTextureReplacementControlFileLifecycle();

	if (g_failures == 0) {
		std::printf("rvk2 unit tests: PASS\n");
		return 0;
	}

	std::fprintf(stderr, "rvk2 unit tests: FAIL (%d failure(s))\n", g_failures);
	return 1;
}
