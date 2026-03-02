#include "rvk2_RasterPipeline.h"

namespace rvk2 {

bool isRasterizableSemantic(const DrawSemanticPacket & _semantic)
{
	return _semantic.drawType == 1U || _semantic.drawType == 2U || _semantic.drawType == 3U;
}

RasterOpPacket buildRasterOpPacket(
	const DrawSemanticPacket & _semantic,
	const RDPStateSnapshot & _rdpState)
{
	RasterOpPacket op{};
	op.sourcePacketId = _semantic.sourcePacketId;
	op.sourceOpcode = _semantic.sourceOpcode;
	switch (_semantic.drawType) {
	case 1U:
		op.opKind = static_cast<u8>(RasterOpKind::kTriangle);
		break;
	case 2U:
		op.opKind = static_cast<u8>(RasterOpKind::kTexRect);
		break;
	case 3U:
		op.opKind = static_cast<u8>(RasterOpKind::kFillRect);
		break;
	default:
		op.opKind = static_cast<u8>(RasterOpKind::kUnknown);
		break;
	}
	op.cycleType = _semantic.cycleType;
	op.tile = _semantic.tile;
	op.texRectFlip = _semantic.texRectFlip;
	op.textured = _semantic.textured;
	op.depthTest = _semantic.depthTest;
	op.rectULX = _semantic.rectULX;
	op.rectULY = _semantic.rectULY;
	op.rectLRX = _semantic.rectLRX;
	op.rectLRY = _semantic.rectLRY;
	op.texS = _semantic.texS;
	op.texT = _semantic.texT;
	op.texDSDX = _semantic.texDSDX;
	op.texDTDY = _semantic.texDTDY;
	op.triangleLMajor = _semantic.triangleLMajor;
	op.triangleLevel = _semantic.triangleLevel;
	op.triangleYL = _semantic.triangleYL;
	op.triangleYM = _semantic.triangleYM;
	op.triangleYH = _semantic.triangleYH;
	op.triangleXL = _semantic.triangleXL;
	op.triangleXH = _semantic.triangleXH;
	op.triangleXM = _semantic.triangleXM;
	op.triangleDxLDY = _semantic.triangleDxLDY;
	op.triangleDxHDY = _semantic.triangleDxHDY;
	op.triangleDxMDY = _semantic.triangleDxMDY;
	op.combineMux = _semantic.combineMux;
	op.blendParams = _semantic.blendParams;
	op.fillColor = _rdpState.fillColor;
	op.syncEpoch = _semantic.syncEpoch;
	op.loadSyncPacketId = _semantic.loadSyncPacketId;
	op.pipeSyncPacketId = _semantic.pipeSyncPacketId;
	op.tileSyncPacketId = _semantic.tileSyncPacketId;
	op.fullSyncPacketId = _semantic.fullSyncPacketId;
	return op;
}

} // namespace rvk2
