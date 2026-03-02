#include "rvk2_DrawSemantic.h"

namespace {

inline bool isTriangleOpcode(u8 _opcode)
{
	return _opcode >= 0x08U && _opcode <= 0x0FU;
}

inline bool isTexRectOpcode(u8 _opcode)
{
	return _opcode == 0x24U || _opcode == 0x25U;
}

inline bool isFillRectOpcode(u8 _opcode)
{
	return _opcode == 0x36U;
}

inline u32 bitRange(u32 _value, u32 _shift, u32 _width)
{
	return (_value >> _shift) & ((1U << _width) - 1U);
}

inline s32 signExtend(u32 _value, u32 _bits)
{
	const u32 signBit = 1U << (_bits - 1U);
	const u32 mask = (1U << _bits) - 1U;
	const u32 x = _value & mask;
	return static_cast<s32>((x ^ signBit) - signBit);
}

inline s32 decodeSignedFixed(u32 _integerPart, u32 _integerBits, u32 _fractionalPart)
{
	return signExtend((_integerPart << 16U) | (_fractionalPart & 0xFFFFU), _integerBits + 16U);
}

inline bool isTexturedOpcode(u8 _opcode)
{
	return _opcode == 0x0AU
		|| _opcode == 0x0BU
		|| _opcode == 0x0EU
		|| _opcode == 0x0FU
		|| isTexRectOpcode(_opcode);
}

inline bool isDepthOpcode(u8 _opcode)
{
	return _opcode == 0x09U
		|| _opcode == 0x0BU
		|| _opcode == 0x0DU
		|| _opcode == 0x0FU;
}

} // namespace

namespace rvk2 {

bool isDrawOpcode(u8 _opcode)
{
	return isTriangleOpcode(_opcode) || isTexRectOpcode(_opcode) || isFillRectOpcode(_opcode);
}

DrawSemanticPacket buildDrawSemanticPacket(
	const CommandPacket & _packet,
	const RDPStateSnapshot & _rdpState,
	const TMEMSnapshot & _tmemState)
{
	DrawSemanticPacket semantic{};
	semantic.sourcePacketId = _packet.id;
	semantic.sourceOpcode = _packet.opcode;
	semantic.cycleType = _rdpState.cycleType;
	semantic.combineMux = _rdpState.combineMux;
	semantic.blendMux1 = static_cast<u32>(_rdpState.otherModes >> 32);
	semantic.blendMux2 = static_cast<u32>(_rdpState.otherModes & 0xFFFFFFFFULL);
	semantic.blendParams =
		(static_cast<u32>(_rdpState.otherModesDecoded.alphaCompare) & 0x3U)
		| ((static_cast<u32>(_rdpState.otherModesDecoded.cvgDest) & 0x3U) << 2)
		| ((static_cast<u32>(_rdpState.otherModesDecoded.depthMode) & 0x3U) << 4)
		| ((_rdpState.otherModesDecoded.forceBlender ? 1U : 0U) << 6)
		| ((_rdpState.otherModesDecoded.depthCompare ? 1U : 0U) << 7)
		| ((_rdpState.otherModesDecoded.depthUpdate ? 1U : 0U) << 8)
		| ((_rdpState.otherModesDecoded.alphaCvgSel ? 1U : 0U) << 9)
		| ((_rdpState.otherModesDecoded.cvgXAlpha ? 1U : 0U) << 10)
		| ((_rdpState.otherModesDecoded.textureEdge ? 1U : 0U) << 11);
	semantic.textured = isTexturedOpcode(_packet.opcode);
	semantic.depthTest = _rdpState.otherModesDecoded.depthCompare && isDepthOpcode(_packet.opcode);
	semantic.syncEpoch = _rdpState.syncEpoch;
	semantic.loadSyncPacketId = _rdpState.loadSyncPacketId;
	semantic.pipeSyncPacketId = _rdpState.pipeSyncPacketId;
	semantic.tileSyncPacketId = _rdpState.tileSyncPacketId;
	semantic.fullSyncPacketId = _rdpState.fullSyncPacketId;

	if (isTriangleOpcode(_packet.opcode))
		semantic.drawType = 1U;
	else if (isTexRectOpcode(_packet.opcode))
		semantic.drawType = 2U;
	else if (isFillRectOpcode(_packet.opcode))
		semantic.drawType = 3U;

	if (semantic.drawType == 2U || semantic.drawType == 3U) {
		semantic.rectULX = static_cast<u16>(bitRange(_packet.w1, 12, 12));
		semantic.rectULY = static_cast<u16>(bitRange(_packet.w1, 0, 12));
		semantic.rectLRX = static_cast<u16>(bitRange(_packet.w0, 12, 12));
		semantic.rectLRY = static_cast<u16>(bitRange(_packet.w0, 0, 12));
	}

	if (semantic.drawType == 2U) {
		semantic.tile = static_cast<u8>(bitRange(_packet.w1, 24, 3));
		semantic.texRectFlip = _packet.opcode == 0x25U;
		if (_packet.extraWordCount > 1U) {
			semantic.texS = static_cast<s16>(bitRange(_packet.w2, 16, 16));
			semantic.texT = static_cast<s16>(bitRange(_packet.w2, 0, 16));
			semantic.texDSDX = static_cast<s16>(bitRange(_packet.w3, 16, 16));
			semantic.texDTDY = static_cast<s16>(bitRange(_packet.w3, 0, 16));
		}
	}
	else if (semantic.drawType == 1U) {
		semantic.triangleLMajor = bitRange(_packet.w0, 23, 1) != 0U;
		semantic.triangleLevel = static_cast<u8>(bitRange(_packet.w0, 19, 3));
		semantic.tile = static_cast<u8>(bitRange(_packet.w0, 16, 3));
		semantic.triangleYL = static_cast<u16>(bitRange(_packet.w0, 0, 14));
		semantic.triangleYM = static_cast<u16>(bitRange(_packet.w1, 16, 14));
		semantic.triangleYH = static_cast<u16>(bitRange(_packet.w1, 0, 14));
		if (_packet.extraWordCount > 1U) {
			semantic.triangleXL = decodeSignedFixed(bitRange(_packet.w2, 16, 12), 12, bitRange(_packet.w2, 0, 16));
			semantic.triangleDxLDY = decodeSignedFixed(bitRange(_packet.w3, 16, 14), 14, bitRange(_packet.w3, 0, 16));
		}
		if (_packet.extraWordCount > 3U) {
			semantic.triangleXH = decodeSignedFixed(bitRange(_packet.w4, 16, 12), 12, bitRange(_packet.w4, 0, 16));
			semantic.triangleDxHDY = decodeSignedFixed(bitRange(_packet.w5, 16, 14), 14, bitRange(_packet.w5, 0, 16));
		}
		if (_packet.extraWordCount > 5U) {
			semantic.triangleXM = decodeSignedFixed(bitRange(_packet.w6, 16, 12), 12, bitRange(_packet.w6, 0, 16));
			semantic.triangleDxMDY = decodeSignedFixed(bitRange(_packet.w7, 16, 14), 14, bitRange(_packet.w7, 0, 16));
		}
	}
	else if (semantic.textured) {
		semantic.tile = static_cast<u8>(bitRange(_packet.w0, 16, 3));
	}

	if (semantic.textured) {
		const u32 tileIndex = semantic.tile & 0x7U;
		const TileDescriptorState & tile = _tmemState.tiles[tileIndex];
		if (tile.format == 0U && tile.size == 0U && tile.line == 0U && tile.tmem == 0U)
			semantic.textured = false;
	}

	return semantic;
}

} // namespace rvk2
