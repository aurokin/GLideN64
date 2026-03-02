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

inline bool isTriangleShadeOpcode(u8 _opcode)
{
	return _opcode == 0x0CU
		|| _opcode == 0x0DU
		|| _opcode == 0x0EU
		|| _opcode == 0x0FU;
}

inline bool isTriangleTextureOpcode(u8 _opcode)
{
	return _opcode == 0x0AU
		|| _opcode == 0x0BU
		|| _opcode == 0x0EU
		|| _opcode == 0x0FU;
}

inline bool isTriangleZBufferOpcode(u8 _opcode)
{
	return _opcode == 0x09U
		|| _opcode == 0x0BU
		|| _opcode == 0x0DU
		|| _opcode == 0x0FU;
}

inline u8 payloadWordCount(const rvk2::CommandPacket & _packet)
{
	if (_packet.payloadWordCount > 0U)
		return _packet.payloadWordCount;
	return _packet.extraWordCount;
}

inline u32 payloadWord(const rvk2::CommandPacket & _packet, u8 _index)
{
	if (_packet.payloadWordCount > _index)
		return _packet.payloadWords[_index];
	switch (_index) {
	case 0U:
		return _packet.w2;
	case 1U:
		return _packet.w3;
	case 2U:
		return _packet.w4;
	case 3U:
		return _packet.w5;
	case 4U:
		return _packet.w6;
	case 5U:
		return _packet.w7;
	default:
		break;
	}
	return 0U;
}

inline void copyTrianglePayloadRange(
	const rvk2::CommandPacket & _packet,
	u8 _payloadOffset,
	u8 _wordCount,
	u8 _expandedOffset,
	u32 (&_expanded)[44])
{
	const u8 count = payloadWordCount(_packet);
	for (u8 i = 0U; i < _wordCount; ++i) {
		const u8 src = static_cast<u8>(_payloadOffset + i);
		const u8 dst = static_cast<u8>(_expandedOffset + i);
		if (src >= count || dst >= 44U)
			break;
		_expanded[dst] = payloadWord(_packet, src);
	}
}

inline void buildTriangleExpandedWords(
	const rvk2::CommandPacket & _packet,
	u32 (&_expanded)[44])
{
	for (u32 i = 0U; i < 44U; ++i)
		_expanded[i] = 0U;
	_expanded[0] = _packet.w0;
	_expanded[1] = _packet.w1;
	switch (_packet.opcode) {
	case 0x08U: // Fill
		copyTrianglePayloadRange(_packet, 0U, 6U, 2U, _expanded);
		break;
	case 0x09U: // Fill+Z
		copyTrianglePayloadRange(_packet, 0U, 6U, 2U, _expanded);
		copyTrianglePayloadRange(_packet, 6U, 4U, 40U, _expanded);
		break;
	case 0x0AU: // Tex
		copyTrianglePayloadRange(_packet, 0U, 6U, 2U, _expanded);
		copyTrianglePayloadRange(_packet, 6U, 16U, 24U, _expanded);
		break;
	case 0x0BU: // Tex+Z
		copyTrianglePayloadRange(_packet, 0U, 6U, 2U, _expanded);
		copyTrianglePayloadRange(_packet, 6U, 16U, 24U, _expanded);
		copyTrianglePayloadRange(_packet, 22U, 4U, 40U, _expanded);
		break;
	case 0x0CU: // Shade
		copyTrianglePayloadRange(_packet, 0U, 22U, 2U, _expanded);
		break;
	case 0x0DU: // Shade+Z
		copyTrianglePayloadRange(_packet, 0U, 22U, 2U, _expanded);
		copyTrianglePayloadRange(_packet, 22U, 4U, 40U, _expanded);
		break;
	case 0x0EU: // Shade+Tex
		copyTrianglePayloadRange(_packet, 0U, 38U, 2U, _expanded);
		break;
	case 0x0FU: // Shade+Tex+Z
		copyTrianglePayloadRange(_packet, 0U, 42U, 2U, _expanded);
		break;
	default:
		break;
	}
}

inline s32 decodePairPrimary(u32 _a, u32 _b)
{
	return static_cast<s32>((_a & 0xFFFF0000U) | ((_b >> 16U) & 0xFFFFU));
}

inline s32 decodePairSecondary(u32 _a, u32 _b)
{
	return static_cast<s32>(((_a << 16U) & 0xFFFF0000U) | (_b & 0xFFFFU));
}

inline void decodePackedPair(u32 _a, u32 _b, s32 & _first, s32 & _second)
{
	_first = decodePairPrimary(_a, _b);
	_second = decodePairSecondary(_a, _b);
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
	const u8 payloadCount = payloadWordCount(_packet);
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
		// Rectangle edges are encoded in 10.2 fixed-point pixel units.
		semantic.rectULX = static_cast<u16>(bitRange(_packet.w1, 12, 12) >> 2U);
		semantic.rectULY = static_cast<u16>(bitRange(_packet.w1, 0, 12) >> 2U);
		semantic.rectLRX = static_cast<u16>(bitRange(_packet.w0, 12, 12) >> 2U);
		semantic.rectLRY = static_cast<u16>(bitRange(_packet.w0, 0, 12) >> 2U);
	}

	if (semantic.drawType == 2U) {
		semantic.tile = static_cast<u8>(bitRange(_packet.w1, 24, 3));
		semantic.texRectFlip = _packet.opcode == 0x25U;
		if (payloadCount > 1U) {
			const u32 w2 = payloadWord(_packet, 0U);
			const u32 w3 = payloadWord(_packet, 1U);
			semantic.texS = static_cast<s16>(bitRange(w2, 16, 16));
			semantic.texT = static_cast<s16>(bitRange(w2, 0, 16));
			semantic.texDSDX = static_cast<s16>(bitRange(w3, 16, 16));
			semantic.texDTDY = static_cast<s16>(bitRange(w3, 0, 16));
		}
	}
	else if (semantic.drawType == 1U) {
		semantic.triangleLMajor = bitRange(_packet.w0, 23, 1) != 0U;
		semantic.triangleLevel = static_cast<u8>(bitRange(_packet.w0, 19, 3));
		semantic.tile = static_cast<u8>(bitRange(_packet.w0, 16, 3));
		semantic.triangleYL = static_cast<u16>(bitRange(_packet.w0, 0, 14));
		semantic.triangleYM = static_cast<u16>(bitRange(_packet.w1, 16, 14));
		semantic.triangleYH = static_cast<u16>(bitRange(_packet.w1, 0, 14));
		if (payloadCount > 1U) {
			const u32 w2 = payloadWord(_packet, 0U);
			const u32 w3 = payloadWord(_packet, 1U);
			semantic.triangleXL = decodeSignedFixed(bitRange(w2, 16, 12), 12, bitRange(w2, 0, 16));
			semantic.triangleDxLDY = decodeSignedFixed(bitRange(w3, 16, 14), 14, bitRange(w3, 0, 16));
		}
		if (payloadCount > 3U) {
			const u32 w4 = payloadWord(_packet, 2U);
			const u32 w5 = payloadWord(_packet, 3U);
			semantic.triangleXH = decodeSignedFixed(bitRange(w4, 16, 12), 12, bitRange(w4, 0, 16));
			semantic.triangleDxHDY = decodeSignedFixed(bitRange(w5, 16, 14), 14, bitRange(w5, 0, 16));
		}
		if (payloadCount > 5U) {
			const u32 w6 = payloadWord(_packet, 4U);
			const u32 w7 = payloadWord(_packet, 5U);
			semantic.triangleXM = decodeSignedFixed(bitRange(w6, 16, 12), 12, bitRange(w6, 0, 16));
			semantic.triangleDxMDY = decodeSignedFixed(bitRange(w7, 16, 14), 14, bitRange(w7, 0, 16));
		}

		semantic.triangleShadeEnable = isTriangleShadeOpcode(_packet.opcode);
		semantic.triangleTextureEnable = isTriangleTextureOpcode(_packet.opcode);
		semantic.triangleZBufferEnable = isTriangleZBufferOpcode(_packet.opcode);

		u32 expandedWords[44]{};
		buildTriangleExpandedWords(_packet, expandedWords);

		if (semantic.triangleShadeEnable) {
			decodePackedPair(expandedWords[8], expandedWords[12], semantic.triangleShadeR, semantic.triangleShadeG);
			decodePackedPair(expandedWords[9], expandedWords[13], semantic.triangleShadeB, semantic.triangleShadeA);
			decodePackedPair(expandedWords[10], expandedWords[14], semantic.triangleShadeDRDX, semantic.triangleShadeDGDX);
			decodePackedPair(expandedWords[11], expandedWords[15], semantic.triangleShadeDBDX, semantic.triangleShadeDADX);
			decodePackedPair(expandedWords[16], expandedWords[20], semantic.triangleShadeDRDE, semantic.triangleShadeDGDE);
			decodePackedPair(expandedWords[17], expandedWords[21], semantic.triangleShadeDBDE, semantic.triangleShadeDADE);
			decodePackedPair(expandedWords[18], expandedWords[22], semantic.triangleShadeDRDY, semantic.triangleShadeDGDY);
			decodePackedPair(expandedWords[19], expandedWords[23], semantic.triangleShadeDBDY, semantic.triangleShadeDADY);
		}

		if (semantic.triangleTextureEnable) {
			decodePackedPair(expandedWords[24], expandedWords[28], semantic.triangleTexS, semantic.triangleTexT);
			semantic.triangleTexW = decodePairPrimary(expandedWords[25], expandedWords[29]);
			decodePackedPair(expandedWords[26], expandedWords[30], semantic.triangleTexDSDX, semantic.triangleTexDTDX);
			semantic.triangleTexDWDX = decodePairPrimary(expandedWords[27], expandedWords[31]);
			decodePackedPair(expandedWords[32], expandedWords[36], semantic.triangleTexDSDE, semantic.triangleTexDTDE);
			semantic.triangleTexDWDE = decodePairPrimary(expandedWords[33], expandedWords[37]);
			decodePackedPair(expandedWords[34], expandedWords[38], semantic.triangleTexDSDY, semantic.triangleTexDTDY);
			semantic.triangleTexDWDY = decodePairPrimary(expandedWords[35], expandedWords[39]);
		}

		if (semantic.triangleZBufferEnable) {
			semantic.triangleZ = static_cast<s32>(expandedWords[40]);
			semantic.triangleDZDX = static_cast<s32>(expandedWords[41]);
			semantic.triangleDZDE = static_cast<s32>(expandedWords[42]);
			semantic.triangleDZDY = static_cast<s32>(expandedWords[43]);
		}
	}
	else if (semantic.textured) {
		semantic.tile = static_cast<u8>(bitRange(_packet.w0, 16, 3));
	}

	(void)_tmemState;

	return semantic;
}

} // namespace rvk2
