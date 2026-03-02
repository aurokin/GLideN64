#include "rvk2_RDPState.h"

namespace {

constexpr u8 kCmdSetScissor = 0x2DU;
constexpr u8 kCmdSetOtherModes = 0x2FU;
constexpr u8 kCmdSetKeyGB = 0x2AU;
constexpr u8 kCmdSetKeyR = 0x2BU;
constexpr u8 kCmdSetConvert = 0x2CU;
constexpr u8 kCmdLoadSync = 0x26U;
constexpr u8 kCmdPipeSync = 0x27U;
constexpr u8 kCmdTileSync = 0x28U;
constexpr u8 kCmdFullSync = 0x29U;
constexpr u8 kCmdSetPrimDepth = 0x2EU;
constexpr u8 kCmdSetFillColor = 0x37U;
constexpr u8 kCmdSetFogColor = 0x38U;
constexpr u8 kCmdSetBlendColor = 0x39U;
constexpr u8 kCmdSetPrimColor = 0x3AU;
constexpr u8 kCmdSetEnvColor = 0x3BU;
constexpr u8 kCmdSetCombineMode = 0x3CU;
constexpr u8 kCmdSetDepthImage = 0x3EU;
constexpr u8 kCmdSetColorImage = 0x3FU;
// SetColorImage/SetDepthImage expose dramAddress[23:0] in the command word.
constexpr u32 kRdramAddressMask = 0x00FFFFFFU;

inline u32 bitRange(u32 _value, u32 _shift, u32 _width)
{
	return (_value >> _shift) & ((1U << _width) - 1U);
}

inline s16 signExtend(u32 _value, u32 _bits)
{
	const u32 mask = (1U << _bits) - 1U;
	const u32 signBit = 1U << (_bits - 1U);
	const u32 x = _value & mask;
	return static_cast<s16>((x ^ signBit) - signBit);
}

void decodeOtherModes(u32 _mode0, u32 _mode1, rvk2::OtherModesDecoded & _decoded)
{
	_decoded.alphaCompare = static_cast<u8>(bitRange(_mode1, 0, 2));
	_decoded.depthSource = static_cast<u8>(bitRange(_mode1, 2, 1));
	_decoded.aaEnable = bitRange(_mode1, 3, 1) != 0U;
	_decoded.depthCompare = bitRange(_mode1, 4, 1) != 0U;
	_decoded.depthUpdate = bitRange(_mode1, 5, 1) != 0U;
	_decoded.imageRead = bitRange(_mode1, 6, 1) != 0U;
	_decoded.colorOnCvg = bitRange(_mode1, 7, 1) != 0U;
	_decoded.cvgDest = static_cast<u8>(bitRange(_mode1, 8, 2));
	_decoded.depthMode = static_cast<u8>(bitRange(_mode1, 10, 2));
	_decoded.cvgXAlpha = bitRange(_mode1, 12, 1) != 0U;
	_decoded.alphaCvgSel = bitRange(_mode1, 13, 1) != 0U;
	_decoded.forceBlender = bitRange(_mode1, 14, 1) != 0U;
	_decoded.textureEdge = bitRange(_mode1, 15, 1) != 0U;
	_decoded.c2_m2b = static_cast<u8>(bitRange(_mode1, 16, 2));
	_decoded.c1_m2b = static_cast<u8>(bitRange(_mode1, 18, 2));
	_decoded.c2_m2a = static_cast<u8>(bitRange(_mode1, 20, 2));
	_decoded.c1_m2a = static_cast<u8>(bitRange(_mode1, 22, 2));
	_decoded.c2_m1b = static_cast<u8>(bitRange(_mode1, 24, 2));
	_decoded.c1_m1b = static_cast<u8>(bitRange(_mode1, 26, 2));
	_decoded.c2_m1a = static_cast<u8>(bitRange(_mode1, 28, 2));
	_decoded.c1_m1a = static_cast<u8>(bitRange(_mode1, 30, 2));

	_decoded.blendMask = static_cast<u8>(bitRange(_mode0, 0, 4));
	_decoded.alphaDither = static_cast<u8>(bitRange(_mode0, 4, 2));
	_decoded.colorDither = static_cast<u8>(bitRange(_mode0, 6, 2));
	_decoded.combineKey = bitRange(_mode0, 8, 1) != 0U;
	_decoded.convertOne = bitRange(_mode0, 9, 1) != 0U;
	_decoded.biLerp1 = bitRange(_mode0, 10, 1) != 0U;
	_decoded.biLerp0 = bitRange(_mode0, 11, 1) != 0U;
	_decoded.textureFilter = static_cast<u8>(bitRange(_mode0, 12, 2));
	_decoded.textureLUT = static_cast<u8>(bitRange(_mode0, 14, 2));
	_decoded.textureLOD = bitRange(_mode0, 16, 1) != 0U;
	_decoded.textureDetail = static_cast<u8>(bitRange(_mode0, 17, 2));
	_decoded.texturePersp = bitRange(_mode0, 19, 1) != 0U;
	_decoded.unusedColorDither = bitRange(_mode0, 22, 1) != 0U;
	_decoded.pipelineMode = bitRange(_mode0, 23, 1) != 0U;
}

} // namespace

namespace rvk2 {

RDPStateEngine::RDPStateEngine()
	: m_snapshot()
{
}

void RDPStateEngine::reset()
{
	m_snapshot = RDPStateSnapshot{};
}

void RDPStateEngine::apply(const CommandPacket & _packet)
{
	if (_packet.domain != CommandDomain::kRDP)
		return;

	switch (_packet.opcode) {
	case kCmdLoadSync:
		applyLoadSync(_packet);
		break;
	case kCmdPipeSync:
		applyPipeSync(_packet);
		break;
	case kCmdTileSync:
		applyTileSync(_packet);
		break;
	case kCmdFullSync:
		applyFullSync(_packet);
		break;
	case kCmdSetOtherModes:
		applySetOtherModes(_packet);
		break;
	case kCmdSetKeyGB:
		applySetKeyGB(_packet);
		break;
	case kCmdSetKeyR:
		applySetKeyR(_packet);
		break;
	case kCmdSetConvert:
		applySetConvert(_packet);
		break;
	case kCmdSetCombineMode:
		applySetCombineMode(_packet);
		break;
	case kCmdSetPrimDepth:
		applySetPrimDepth(_packet);
		break;
	case kCmdSetScissor:
		applySetScissor(_packet);
		break;
	case kCmdSetFillColor:
		applySetFillColor(_packet);
		break;
	case kCmdSetFogColor:
		applySetFogColor(_packet);
		break;
	case kCmdSetBlendColor:
		applySetBlendColor(_packet);
		break;
	case kCmdSetPrimColor:
		applySetPrimColor(_packet);
		break;
	case kCmdSetEnvColor:
		applySetEnvColor(_packet);
		break;
	case kCmdSetColorImage:
		applySetColorImage(_packet);
		break;
	case kCmdSetDepthImage:
		applySetDepthImage(_packet);
		break;
	default:
		break;
	}

	m_snapshot.lastPacketId = _packet.id;
}

const RDPStateSnapshot & RDPStateEngine::snapshot() const
{
	return m_snapshot;
}

void RDPStateEngine::applySetOtherModes(const CommandPacket & _packet)
{
	const u32 mode0 = _packet.w0 & 0x00FFFFFFU;
	const u32 mode1 = _packet.w1;
	m_snapshot.otherModes = (static_cast<u64>(mode0) << 32) | static_cast<u64>(mode1);
	m_snapshot.cycleType = static_cast<u8>(bitRange(mode0, 20, 2));
	decodeOtherModes(mode0, mode1, m_snapshot.otherModesDecoded);
	m_snapshot.changedMask |= rdp_state_changed::kOtherModes;
}

void RDPStateEngine::applySetCombineMode(const CommandPacket & _packet)
{
	const u32 muxs0 = _packet.w0 & 0x00FFFFFFU;
	m_snapshot.combineMux = (static_cast<u64>(muxs0) << 32) | static_cast<u64>(_packet.w1);
	m_snapshot.changedMask |= rdp_state_changed::kCombine;
}

void RDPStateEngine::applySetScissor(const CommandPacket & _packet)
{
	m_snapshot.scissorMode = static_cast<u8>(bitRange(_packet.w1, 24, 2));
	// Scissor edges are encoded in 10.2 fixed-point pixel units.
	m_snapshot.scissorXH = static_cast<u16>(bitRange(_packet.w0, 12, 12) >> 2U);
	m_snapshot.scissorYH = static_cast<u16>(bitRange(_packet.w0, 0, 12) >> 2U);
	m_snapshot.scissorXL = static_cast<u16>(bitRange(_packet.w1, 12, 12) >> 2U);
	m_snapshot.scissorYL = static_cast<u16>(bitRange(_packet.w1, 0, 12) >> 2U);
	m_snapshot.changedMask |= rdp_state_changed::kScissor;
}

void RDPStateEngine::applySetColorImage(const CommandPacket & _packet)
{
	m_snapshot.colorImageFormat = static_cast<u8>(bitRange(_packet.w0, 21, 3));
	m_snapshot.colorImageSize = static_cast<u8>(bitRange(_packet.w0, 19, 2));
	m_snapshot.colorImageWidth = static_cast<u16>(bitRange(_packet.w0, 0, 12) + 1U);
	m_snapshot.colorImageAddress = _packet.w1 & kRdramAddressMask;
	m_snapshot.changedMask |= rdp_state_changed::kColorImage;
}

void RDPStateEngine::applySetDepthImage(const CommandPacket & _packet)
{
	m_snapshot.depthImageAddress = _packet.w1 & kRdramAddressMask;
	m_snapshot.changedMask |= rdp_state_changed::kDepthImage;
}

void RDPStateEngine::applySetPrimColor(const CommandPacket & _packet)
{
	m_snapshot.primColorMinLevel = static_cast<u8>(bitRange(_packet.w0, 8, 5));
	m_snapshot.primColorLodFrac = static_cast<u8>(bitRange(_packet.w0, 0, 8));
	m_snapshot.primColor.r = static_cast<u8>(bitRange(_packet.w1, 24, 8));
	m_snapshot.primColor.g = static_cast<u8>(bitRange(_packet.w1, 16, 8));
	m_snapshot.primColor.b = static_cast<u8>(bitRange(_packet.w1, 8, 8));
	m_snapshot.primColor.a = static_cast<u8>(bitRange(_packet.w1, 0, 8));
	m_snapshot.changedMask |= rdp_state_changed::kPrimColor;
}

void RDPStateEngine::applySetEnvColor(const CommandPacket & _packet)
{
	m_snapshot.envColor.r = static_cast<u8>(bitRange(_packet.w1, 24, 8));
	m_snapshot.envColor.g = static_cast<u8>(bitRange(_packet.w1, 16, 8));
	m_snapshot.envColor.b = static_cast<u8>(bitRange(_packet.w1, 8, 8));
	m_snapshot.envColor.a = static_cast<u8>(bitRange(_packet.w1, 0, 8));
	m_snapshot.changedMask |= rdp_state_changed::kEnvColor;
}

void RDPStateEngine::applySetBlendColor(const CommandPacket & _packet)
{
	m_snapshot.blendColor.r = static_cast<u8>(bitRange(_packet.w1, 24, 8));
	m_snapshot.blendColor.g = static_cast<u8>(bitRange(_packet.w1, 16, 8));
	m_snapshot.blendColor.b = static_cast<u8>(bitRange(_packet.w1, 8, 8));
	m_snapshot.blendColor.a = static_cast<u8>(bitRange(_packet.w1, 0, 8));
	m_snapshot.changedMask |= rdp_state_changed::kBlendColor;
}

void RDPStateEngine::applySetFogColor(const CommandPacket & _packet)
{
	m_snapshot.fogColor.r = static_cast<u8>(bitRange(_packet.w1, 24, 8));
	m_snapshot.fogColor.g = static_cast<u8>(bitRange(_packet.w1, 16, 8));
	m_snapshot.fogColor.b = static_cast<u8>(bitRange(_packet.w1, 8, 8));
	m_snapshot.fogColor.a = static_cast<u8>(bitRange(_packet.w1, 0, 8));
	m_snapshot.changedMask |= rdp_state_changed::kFogColor;
}

void RDPStateEngine::applySetFillColor(const CommandPacket & _packet)
{
	m_snapshot.fillColor = _packet.w1;
	m_snapshot.changedMask |= rdp_state_changed::kFillColor;
}

void RDPStateEngine::applySetPrimDepth(const CommandPacket & _packet)
{
	m_snapshot.primDepthZ = static_cast<u16>(bitRange(_packet.w1, 16, 16));
	m_snapshot.primDepthDelta = static_cast<u16>(bitRange(_packet.w1, 0, 16));
	m_snapshot.changedMask |= rdp_state_changed::kPrimDepth;
}

void RDPStateEngine::applySetConvert(const CommandPacket & _packet)
{
	const u32 k0 = bitRange(_packet.w0, 13, 9);
	const u32 k1 = bitRange(_packet.w0, 4, 9);
	const u32 k2 = (bitRange(_packet.w0, 0, 4) << 5) | bitRange(_packet.w1, 27, 5);
	const u32 k3 = bitRange(_packet.w1, 18, 9);
	const u32 k4 = bitRange(_packet.w1, 9, 9);
	const u32 k5 = bitRange(_packet.w1, 0, 9);
	m_snapshot.convertK0 = signExtend(k0, 9);
	m_snapshot.convertK1 = signExtend(k1, 9);
	m_snapshot.convertK2 = signExtend(k2, 9);
	m_snapshot.convertK3 = signExtend(k3, 9);
	m_snapshot.convertK4 = static_cast<s16>(k4);
	m_snapshot.convertK5 = static_cast<s16>(k5);
	m_snapshot.changedMask |= rdp_state_changed::kConvert;
}

void RDPStateEngine::applySetKeyR(const CommandPacket & _packet)
{
	m_snapshot.keyCenterR = static_cast<u8>(bitRange(_packet.w1, 8, 8));
	m_snapshot.keyScaleR = static_cast<u8>(bitRange(_packet.w1, 0, 8));
	m_snapshot.keyWidthR = static_cast<u16>(bitRange(_packet.w1, 16, 12));
	m_snapshot.changedMask |= rdp_state_changed::kKeyR;
}

void RDPStateEngine::applySetKeyGB(const CommandPacket & _packet)
{
	m_snapshot.keyCenterG = static_cast<u8>(bitRange(_packet.w1, 24, 8));
	m_snapshot.keyScaleG = static_cast<u8>(bitRange(_packet.w1, 16, 8));
	m_snapshot.keyWidthG = static_cast<u16>(bitRange(_packet.w0, 12, 12));
	m_snapshot.keyCenterB = static_cast<u8>(bitRange(_packet.w1, 8, 8));
	m_snapshot.keyScaleB = static_cast<u8>(bitRange(_packet.w1, 0, 8));
	m_snapshot.keyWidthB = static_cast<u16>(bitRange(_packet.w0, 0, 12));
	m_snapshot.changedMask |= rdp_state_changed::kKeyGB;
}

void RDPStateEngine::applyLoadSync(const CommandPacket & _packet)
{
	++m_snapshot.syncEpoch;
	++m_snapshot.loadSyncCount;
	m_snapshot.loadSyncPacketId = _packet.id;
	m_snapshot.changedMask |= rdp_state_changed::kLoadSync;
}

void RDPStateEngine::applyPipeSync(const CommandPacket & _packet)
{
	++m_snapshot.syncEpoch;
	++m_snapshot.pipeSyncCount;
	m_snapshot.pipeSyncPacketId = _packet.id;
	m_snapshot.changedMask |= rdp_state_changed::kPipeSync;
}

void RDPStateEngine::applyTileSync(const CommandPacket & _packet)
{
	++m_snapshot.syncEpoch;
	++m_snapshot.tileSyncCount;
	m_snapshot.tileSyncPacketId = _packet.id;
	m_snapshot.changedMask |= rdp_state_changed::kTileSync;
}

void RDPStateEngine::applyFullSync(const CommandPacket & _packet)
{
	++m_snapshot.syncEpoch;
	++m_snapshot.fullSyncCount;
	m_snapshot.fullSyncPacketId = _packet.id;
	m_snapshot.changedMask |= rdp_state_changed::kFullSync;
}

} // namespace rvk2
