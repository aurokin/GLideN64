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

inline s32 signExtend14(u16 _value)
{
	const u32 raw = static_cast<u32>(_value) & 0x3FFFU;
	return static_cast<s32>((raw ^ 0x2000U) - 0x2000U);
}

void applyTriangleRectBounds(const rvk2::RasterOpPacket & _op, rvk2::RenderWorkPacket & _work)
{
	// Triangle Y edges are signed 14-bit s10.2 values.
	const s32 yh = signExtend14(_op.triangleYH);
	const s32 ym = signExtend14(_op.triangleYM);
	const s32 yl = signExtend14(_op.triangleYL);
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

inline u32 packColorRGBA(const rvk2::ColorRGBA8 & _color)
{
	return (static_cast<u32>(_color.r) << 24U)
		| (static_cast<u32>(_color.g) << 16U)
		| (static_cast<u32>(_color.b) << 8U)
		| static_cast<u32>(_color.a);
}

inline void mixDigest(u64 & _state, u64 _value)
{
	_state ^= _value + 0x9E3779B97F4A7C15ULL + (_state << 6U) + (_state >> 2U);
}

inline u64 buildKeyStateDigest(const rvk2::RDPStateSnapshot & _rdpState)
{
	u64 digest = 1469598103934665603ULL;
	mixDigest(digest, static_cast<u64>(_rdpState.keyCenterR));
	mixDigest(digest, static_cast<u64>(_rdpState.keyScaleR));
	mixDigest(digest, static_cast<u64>(_rdpState.keyCenterG));
	mixDigest(digest, static_cast<u64>(_rdpState.keyScaleG));
	mixDigest(digest, static_cast<u64>(_rdpState.keyCenterB));
	mixDigest(digest, static_cast<u64>(_rdpState.keyScaleB));
	mixDigest(digest, static_cast<u64>(_rdpState.keyWidthR));
	mixDigest(digest, static_cast<u64>(_rdpState.keyWidthG));
	mixDigest(digest, static_cast<u64>(_rdpState.keyWidthB));
	return digest;
}

inline u64 buildConvertStateDigest(const rvk2::RDPStateSnapshot & _rdpState)
{
	u64 digest = 1469598103934665603ULL;
	mixDigest(digest, static_cast<u64>(static_cast<u16>(_rdpState.convertK0)));
	mixDigest(digest, static_cast<u64>(static_cast<u16>(_rdpState.convertK1)));
	mixDigest(digest, static_cast<u64>(static_cast<u16>(_rdpState.convertK2)));
	mixDigest(digest, static_cast<u64>(static_cast<u16>(_rdpState.convertK3)));
	mixDigest(digest, static_cast<u64>(static_cast<u16>(_rdpState.convertK4)));
	mixDigest(digest, static_cast<u64>(static_cast<u16>(_rdpState.convertK5)));
	mixDigest(digest, static_cast<u64>(_rdpState.primColorMinLevel));
	mixDigest(digest, static_cast<u64>(_rdpState.primColorLodFrac));
	return digest;
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
	work.depthCompareEnable = _rdpState.otherModesDecoded.depthCompare;
	work.depthUpdateEnable = _rdpState.otherModesDecoded.depthUpdate;
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
	work.triangleShadeEnable = _op.triangleShadeEnable;
	work.triangleTextureEnable = _op.triangleTextureEnable;
	work.triangleZBufferEnable = _op.triangleZBufferEnable;
	work.triangleShadeR = _op.triangleShadeR;
	work.triangleShadeG = _op.triangleShadeG;
	work.triangleShadeB = _op.triangleShadeB;
	work.triangleShadeA = _op.triangleShadeA;
	work.triangleShadeDRDX = _op.triangleShadeDRDX;
	work.triangleShadeDGDX = _op.triangleShadeDGDX;
	work.triangleShadeDBDX = _op.triangleShadeDBDX;
	work.triangleShadeDADX = _op.triangleShadeDADX;
	work.triangleShadeDRDE = _op.triangleShadeDRDE;
	work.triangleShadeDGDE = _op.triangleShadeDGDE;
	work.triangleShadeDBDE = _op.triangleShadeDBDE;
	work.triangleShadeDADE = _op.triangleShadeDADE;
	work.triangleShadeDRDY = _op.triangleShadeDRDY;
	work.triangleShadeDGDY = _op.triangleShadeDGDY;
	work.triangleShadeDBDY = _op.triangleShadeDBDY;
	work.triangleShadeDADY = _op.triangleShadeDADY;
	work.triangleTexS = _op.triangleTexS;
	work.triangleTexT = _op.triangleTexT;
	work.triangleTexW = _op.triangleTexW;
	work.triangleTexDSDX = _op.triangleTexDSDX;
	work.triangleTexDTDX = _op.triangleTexDTDX;
	work.triangleTexDWDX = _op.triangleTexDWDX;
	work.triangleTexDSDE = _op.triangleTexDSDE;
	work.triangleTexDTDE = _op.triangleTexDTDE;
	work.triangleTexDWDE = _op.triangleTexDWDE;
	work.triangleTexDSDY = _op.triangleTexDSDY;
	work.triangleTexDTDY = _op.triangleTexDTDY;
	work.triangleTexDWDY = _op.triangleTexDWDY;
	work.triangleZ = _op.triangleZ;
	work.triangleDZDX = _op.triangleDZDX;
	work.triangleDZDE = _op.triangleDZDE;
	work.triangleDZDY = _op.triangleDZDY;
	if (_op.opKind == static_cast<u8>(RasterOpKind::kTriangle))
		applyTriangleRectBounds(_op, work);

	work.colorImageFormat = _rdpState.colorImageFormat;
	work.colorImageSize = _rdpState.colorImageSize;
	work.colorImageWidth = _rdpState.colorImageWidth;
	work.colorImageAddress = _rdpState.colorImageAddress;
	work.depthImageAddress = _rdpState.depthImageAddress;
	work.alphaCompare = _rdpState.otherModesDecoded.alphaCompare;
	work.cvgDest = _rdpState.otherModesDecoded.cvgDest;
	work.blendMask = _rdpState.otherModesDecoded.blendMask;
	work.cvgXAlpha = _rdpState.otherModesDecoded.cvgXAlpha;
	work.alphaCvgSel = _rdpState.otherModesDecoded.alphaCvgSel;
	work.colorOnCvg = _rdpState.otherModesDecoded.colorOnCvg;
	work.forceBlender = _rdpState.otherModesDecoded.forceBlender;
	work.depthSource = _rdpState.otherModesDecoded.depthSource;
	work.primDepthZ = _rdpState.primDepthZ;
	work.primDepthDelta = _rdpState.primDepthDelta;
	work.otherModes = _rdpState.otherModes;
	work.primColor = packColorRGBA(_rdpState.primColor);
	work.envColor = packColorRGBA(_rdpState.envColor);
	work.blendColor = packColorRGBA(_rdpState.blendColor);
	work.fogColor = packColorRGBA(_rdpState.fogColor);
	work.primColorMinLevel = _rdpState.primColorMinLevel;
	work.primColorLodFrac = _rdpState.primColorLodFrac;
	work.convertK4 = _rdpState.convertK4;
	work.convertK5 = _rdpState.convertK5;
	work.keyCenterR = _rdpState.keyCenterR;
	work.keyScaleR = _rdpState.keyScaleR;
	work.keyCenterG = _rdpState.keyCenterG;
	work.keyScaleG = _rdpState.keyScaleG;
	work.keyCenterB = _rdpState.keyCenterB;
	work.keyScaleB = _rdpState.keyScaleB;
	work.keyState = buildKeyStateDigest(_rdpState);
	work.convertState = buildConvertStateDigest(_rdpState);
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
	const u8 tile1Index = static_cast<u8>(((_op.tile & 0x7U) + 1U) & 0x7U);
	const TileDescriptorState & tile1 = _tmemState.tiles[tile1Index];
	work.tile1Valid = true;
	work.tile1Index = tile1Index;
	work.tile1Format = tile1.format;
	work.tile1Size = tile1.size;
	work.tile1Line = tile1.line;
	work.tile1Tmem = tile1.tmem;
	work.tile1Palette = tile1.palette;
	work.tile1Cmt = tile1.cmt;
	work.tile1Cms = tile1.cms;
	work.tile1Maskt = tile1.maskt;
	work.tile1Masks = tile1.masks;
	work.tile1Shiftt = tile1.shiftt;
	work.tile1Shifts = tile1.shifts;
	work.tile1ULS = tile1.uls;
	work.tile1ULT = tile1.ult;
	work.tile1LRS = tile1.lrs;
	work.tile1LRT = tile1.lrt;

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
