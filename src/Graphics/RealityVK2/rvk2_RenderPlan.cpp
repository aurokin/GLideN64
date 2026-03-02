#include "rvk2_RenderPlan.h"

#include <algorithm>
#include <limits>

namespace {

u8 classifyPhase(u8 _cycleType, u8 _opKind)
{
	if (_opKind == static_cast<u8>(rvk2::RasterOpKind::kFillRect) || _cycleType == 3U)
		return static_cast<u8>(rvk2::RenderPhase::kFill);
	if (_cycleType == 2U)
		return static_cast<u8>(rvk2::RenderPhase::kCopy);
	if (_cycleType == 1U)
		return static_cast<u8>(rvk2::RenderPhase::kCycle2);
	if (_cycleType == 0U)
		return static_cast<u8>(rvk2::RenderPhase::kCycle1);
	return static_cast<u8>(rvk2::RenderPhase::kUnknown);
}

void setBarrierBitIfAdvanced(
	rvk2::PacketId _packetId,
	rvk2::PacketId & _lastPacketId,
	u8 _bit,
	u8 & _mask)
{
	if (_packetId != 0ULL && _packetId != _lastPacketId)
		_mask |= _bit;
	_lastPacketId = _packetId;
}

inline s32 clampToRectCoord(s32 _value)
{
	if (_value < 0)
		return 0;
	if (_value > 0xFFFF)
		return 0xFFFF;
	return _value;
}

inline s32 evalEdgeXFixed16(s32 _xStart, s32 _dxdy, s32 _yStart, s32 _yTarget)
{
	const s64 deltaY = static_cast<s64>(_yTarget) - static_cast<s64>(_yStart);
	const s64 x = static_cast<s64>(_xStart) + static_cast<s64>(_dxdy) * deltaY;
	if (x < static_cast<s64>(std::numeric_limits<s32>::min()))
		return std::numeric_limits<s32>::min();
	if (x > static_cast<s64>(std::numeric_limits<s32>::max()))
		return std::numeric_limits<s32>::max();
	return static_cast<s32>(x);
}

void applyTriangleRectBounds(const rvk2::RasterOpPacket & _op, rvk2::RenderWorkPacket & _work)
{
	const s32 yh = static_cast<s32>(_op.triangleYH);
	const s32 ym = static_cast<s32>(_op.triangleYM);
	const s32 yl = static_cast<s32>(_op.triangleYL);
	const s32 yMinSubpixel = std::min(yh, std::min(ym, yl));
	const s32 yMaxSubpixel = std::max(yh, std::max(ym, yl));
	const s32 yMin = clampToRectCoord(yMinSubpixel >> 2);
	const s32 yMax = clampToRectCoord((yMaxSubpixel + 3) >> 2);

	const s32 xh = _op.triangleXH;
	const s32 xm = _op.triangleXM;
	const s32 xl = _op.triangleXL;
	const s32 xLongAtYM = evalEdgeXFixed16(xh, _op.triangleDxHDY, yh, ym);
	const s32 xLongAtYL = evalEdgeXFixed16(xh, _op.triangleDxHDY, yh, yl);
	const s32 xMidAtYM = evalEdgeXFixed16(xm, _op.triangleDxMDY, yh, ym);
	const s32 xLowAtYL = evalEdgeXFixed16(xl, _op.triangleDxLDY, ym, yl);

	const s32 xMinFixed = std::min(
		std::min(std::min(xh, xm), std::min(xl, xLongAtYM)),
		std::min(std::min(xLongAtYL, xMidAtYM), xLowAtYL));
	const s32 xMaxFixed = std::max(
		std::max(std::max(xh, xm), std::max(xl, xLongAtYM)),
		std::max(std::max(xLongAtYL, xMidAtYM), xLowAtYL));
	const s32 xMin = clampToRectCoord(xMinFixed >> 16);
	const s32 xMax = clampToRectCoord((xMaxFixed + 0xFFFF) >> 16);

	_work.rectULX = static_cast<u16>(std::min(xMin, xMax));
	_work.rectULY = static_cast<u16>(std::min(yMin, yMax));
	_work.rectLRX = static_cast<u16>(std::max(xMin, xMax));
	_work.rectLRY = static_cast<u16>(std::max(yMin, yMax));
}

} // namespace

namespace rvk2 {

bool isRenderableRasterOp(const RasterOpPacket & _op)
{
	return classifyPhase(_op.cycleType, _op.opKind) != static_cast<u8>(RenderPhase::kUnknown);
}

RenderWorkPacket buildRenderWorkPacket(
	const RasterOpPacket & _op,
	const RDPStateSnapshot & _rdpState,
	const TMEMSnapshot & _tmemState,
	RenderPlanState & _planState)
{
	RenderWorkPacket work{};
	work.sourcePacketId = _op.sourcePacketId;
	work.sourceOpcode = _op.sourceOpcode;
	work.opKind = _op.opKind;
	work.phase = classifyPhase(_op.cycleType, _op.opKind);
	work.cycleType = _op.cycleType;
	work.tile = _op.tile;
	work.texRectFlip = _op.texRectFlip;
	work.textured = _op.textured;
	work.depthTest = _op.depthTest;
	work.rectULX = _op.rectULX;
	work.rectULY = _op.rectULY;
	work.rectLRX = _op.rectLRX;
	work.rectLRY = _op.rectLRY;
	work.texS = _op.texS;
	work.texT = _op.texT;
	work.texDSDX = _op.texDSDX;
	work.texDTDY = _op.texDTDY;
	work.triangleLMajor = _op.triangleLMajor;
	work.triangleLevel = _op.triangleLevel;
	work.triangleYL = _op.triangleYL;
	work.triangleYM = _op.triangleYM;
	work.triangleYH = _op.triangleYH;
	work.triangleXL = _op.triangleXL;
	work.triangleXH = _op.triangleXH;
	work.triangleXM = _op.triangleXM;
	work.triangleDxLDY = _op.triangleDxLDY;
	work.triangleDxHDY = _op.triangleDxHDY;
	work.triangleDxMDY = _op.triangleDxMDY;
	if (_op.opKind == static_cast<u8>(RasterOpKind::kTriangle))
		applyTriangleRectBounds(_op, work);

	work.colorImageFormat = _rdpState.colorImageFormat;
	work.colorImageSize = _rdpState.colorImageSize;
	work.colorImageWidth = _rdpState.colorImageWidth;
	work.colorImageAddress = _rdpState.colorImageAddress;
	work.depthImageAddress = _rdpState.depthImageAddress;
	work.scissorMode = _rdpState.scissorMode;
	work.scissorXH = _rdpState.scissorXH;
	work.scissorYH = _rdpState.scissorYH;
	work.scissorXL = _rdpState.scissorXL;
	work.scissorYL = _rdpState.scissorYL;

	work.textureImageFormat = _tmemState.textureImage.format;
	work.textureImageSize = _tmemState.textureImage.size;
	work.textureImageWidth = _tmemState.textureImage.width;
	work.textureImageAddress = _tmemState.textureImage.address;

	const TileDescriptorState & tile = _tmemState.tiles[_op.tile & 0x7U];
	work.tileFormat = tile.format;
	work.tileSize = tile.size;
	work.tileLine = tile.line;
	work.tileTmem = tile.tmem;
	work.tilePalette = tile.palette;
	work.tileCmt = tile.cmt;
	work.tileCms = tile.cms;
	work.tileMaskt = tile.maskt;
	work.tileMasks = tile.masks;
	work.tileShiftt = tile.shiftt;
	work.tileShifts = tile.shifts;
	work.tileULS = tile.uls;
	work.tileULT = tile.ult;
	work.tileLRS = tile.lrs;
	work.tileLRT = tile.lrt;

	work.tmemLoadKind = static_cast<u8>(_tmemState.lastLoad.kind);
	work.tmemLoadTile = _tmemState.lastLoad.tile;
	work.tmemLoadULS = _tmemState.lastLoad.uls;
	work.tmemLoadULT = _tmemState.lastLoad.ult;
	work.tmemLoadLRS = _tmemState.lastLoad.lrs;
	work.tmemLoadLRT = _tmemState.lastLoad.lrt;
	work.tmemLoadDXT = _tmemState.lastLoad.dxt;

	work.combineMux = _op.combineMux;
	work.blendParams = _op.blendParams;
	work.fillColor = _op.fillColor;
	work.syncEpoch = _op.syncEpoch;
	work.loadSyncPacketId = _op.loadSyncPacketId;
	work.pipeSyncPacketId = _op.pipeSyncPacketId;
	work.tileSyncPacketId = _op.tileSyncPacketId;
	work.fullSyncPacketId = _op.fullSyncPacketId;

	work.barrierMask = render_barrier::kNone;
	setBarrierBitIfAdvanced(
		work.loadSyncPacketId,
		_planState.lastLoadSyncPacketId,
		render_barrier::kLoadSync,
		work.barrierMask);
	setBarrierBitIfAdvanced(
		work.pipeSyncPacketId,
		_planState.lastPipeSyncPacketId,
		render_barrier::kPipeSync,
		work.barrierMask);
	setBarrierBitIfAdvanced(
		work.tileSyncPacketId,
		_planState.lastTileSyncPacketId,
		render_barrier::kTileSync,
		work.barrierMask);
	setBarrierBitIfAdvanced(
		work.fullSyncPacketId,
		_planState.lastFullSyncPacketId,
		render_barrier::kFullSync,
		work.barrierMask);

	return work;
}

} // namespace rvk2
