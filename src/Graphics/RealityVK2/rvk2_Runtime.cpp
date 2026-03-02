#include "rvk2_Runtime.h"

#include <cstddef>

#include "rvk2_Executor.h"

namespace {

constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;

inline void updateHashBytes(u64 & _hash, const void * _data, size_t _size)
{
	const u8 * bytes = static_cast<const u8 *>(_data);
	for (size_t i = 0; i < _size; ++i) {
		_hash ^= static_cast<u64>(bytes[i]);
		_hash *= kFnvPrime;
	}
}

template <typename T>
inline void updateHash(u64 & _hash, const T & _value)
{
	updateHashBytes(_hash, &_value, sizeof(T));
}

u64 hashOtherModesDecoded(const rvk2::OtherModesDecoded & _decoded)
{
	u64 hash = kFnvOffset;
	updateHash(hash, _decoded.alphaCompare);
	updateHash(hash, _decoded.depthSource);
	updateHash(hash, _decoded.cvgDest);
	updateHash(hash, _decoded.depthMode);
	updateHash(hash, _decoded.c2_m2b);
	updateHash(hash, _decoded.c1_m2b);
	updateHash(hash, _decoded.c2_m2a);
	updateHash(hash, _decoded.c1_m2a);
	updateHash(hash, _decoded.c2_m1b);
	updateHash(hash, _decoded.c1_m1b);
	updateHash(hash, _decoded.c2_m1a);
	updateHash(hash, _decoded.c1_m1a);
	updateHash(hash, _decoded.blendMask);
	updateHash(hash, _decoded.alphaDither);
	updateHash(hash, _decoded.colorDither);
	updateHash(hash, _decoded.textureFilter);
	updateHash(hash, _decoded.textureLUT);
	updateHash(hash, _decoded.textureDetail);
	updateHash(hash, _decoded.aaEnable);
	updateHash(hash, _decoded.depthCompare);
	updateHash(hash, _decoded.depthUpdate);
	updateHash(hash, _decoded.imageRead);
	updateHash(hash, _decoded.colorOnCvg);
	updateHash(hash, _decoded.cvgXAlpha);
	updateHash(hash, _decoded.alphaCvgSel);
	updateHash(hash, _decoded.forceBlender);
	updateHash(hash, _decoded.textureEdge);
	updateHash(hash, _decoded.combineKey);
	updateHash(hash, _decoded.convertOne);
	updateHash(hash, _decoded.biLerp1);
	updateHash(hash, _decoded.biLerp0);
	updateHash(hash, _decoded.textureLOD);
	updateHash(hash, _decoded.texturePersp);
	updateHash(hash, _decoded.unusedColorDither);
	updateHash(hash, _decoded.pipelineMode);
	return hash;
}

u64 hashRDPState(const rvk2::RDPStateSnapshot & _snapshot)
{
	u64 hash = kFnvOffset;
	updateHash(hash, _snapshot.lastPacketId);
	updateHash(hash, _snapshot.combineMux);
	updateHash(hash, _snapshot.otherModes);
	updateHash(hash, hashOtherModesDecoded(_snapshot.otherModesDecoded));
	updateHash(hash, _snapshot.cycleType);
	updateHash(hash, _snapshot.colorImageFormat);
	updateHash(hash, _snapshot.colorImageSize);
	updateHash(hash, _snapshot.colorImageWidth);
	updateHash(hash, _snapshot.colorImageAddress);
	updateHash(hash, _snapshot.depthImageAddress);
	updateHash(hash, _snapshot.scissorMode);
	updateHash(hash, _snapshot.scissorXH);
	updateHash(hash, _snapshot.scissorYH);
	updateHash(hash, _snapshot.scissorXL);
	updateHash(hash, _snapshot.scissorYL);
	updateHash(hash, _snapshot.primColor.r);
	updateHash(hash, _snapshot.primColor.g);
	updateHash(hash, _snapshot.primColor.b);
	updateHash(hash, _snapshot.primColor.a);
	updateHash(hash, _snapshot.envColor.r);
	updateHash(hash, _snapshot.envColor.g);
	updateHash(hash, _snapshot.envColor.b);
	updateHash(hash, _snapshot.envColor.a);
	updateHash(hash, _snapshot.blendColor.r);
	updateHash(hash, _snapshot.blendColor.g);
	updateHash(hash, _snapshot.blendColor.b);
	updateHash(hash, _snapshot.blendColor.a);
	updateHash(hash, _snapshot.fogColor.r);
	updateHash(hash, _snapshot.fogColor.g);
	updateHash(hash, _snapshot.fogColor.b);
	updateHash(hash, _snapshot.fogColor.a);
	updateHash(hash, _snapshot.primColorMinLevel);
	updateHash(hash, _snapshot.primColorLodFrac);
	updateHash(hash, _snapshot.fillColor);
	updateHash(hash, _snapshot.primDepthZ);
	updateHash(hash, _snapshot.primDepthDelta);
	updateHash(hash, _snapshot.convertK0);
	updateHash(hash, _snapshot.convertK1);
	updateHash(hash, _snapshot.convertK2);
	updateHash(hash, _snapshot.convertK3);
	updateHash(hash, _snapshot.convertK4);
	updateHash(hash, _snapshot.convertK5);
	updateHash(hash, _snapshot.keyCenterR);
	updateHash(hash, _snapshot.keyScaleR);
	updateHash(hash, _snapshot.keyCenterG);
	updateHash(hash, _snapshot.keyScaleG);
	updateHash(hash, _snapshot.keyCenterB);
	updateHash(hash, _snapshot.keyScaleB);
	updateHash(hash, _snapshot.keyWidthR);
	updateHash(hash, _snapshot.keyWidthG);
	updateHash(hash, _snapshot.keyWidthB);
	updateHash(hash, _snapshot.syncEpoch);
	updateHash(hash, _snapshot.loadSyncCount);
	updateHash(hash, _snapshot.pipeSyncCount);
	updateHash(hash, _snapshot.tileSyncCount);
	updateHash(hash, _snapshot.fullSyncCount);
	updateHash(hash, _snapshot.loadSyncPacketId);
	updateHash(hash, _snapshot.pipeSyncPacketId);
	updateHash(hash, _snapshot.tileSyncPacketId);
	updateHash(hash, _snapshot.fullSyncPacketId);
	updateHash(hash, _snapshot.changedMask);
	return hash;
}

u64 hashTMEMState(const rvk2::TMEMSnapshot & _snapshot)
{
	u64 hash = kFnvOffset;
	updateHash(hash, _snapshot.lastPacketId);
	updateHash(hash, _snapshot.textureImage.format);
	updateHash(hash, _snapshot.textureImage.size);
	updateHash(hash, _snapshot.textureImage.width);
	updateHash(hash, _snapshot.textureImage.address);
	for (u32 tileIndex = 0U; tileIndex < rvk2::kTileCount; ++tileIndex) {
		const rvk2::TileDescriptorState & tile = _snapshot.tiles[tileIndex];
		updateHash(hash, tile.format);
		updateHash(hash, tile.size);
		updateHash(hash, tile.line);
		updateHash(hash, tile.tmem);
		updateHash(hash, tile.palette);
		updateHash(hash, tile.cmt);
		updateHash(hash, tile.cms);
		updateHash(hash, tile.maskt);
		updateHash(hash, tile.masks);
		updateHash(hash, tile.shiftt);
		updateHash(hash, tile.shifts);
		updateHash(hash, tile.uls);
		updateHash(hash, tile.ult);
		updateHash(hash, tile.lrs);
		updateHash(hash, tile.lrt);
	}
	updateHash(hash, _snapshot.lastLoad.sourcePacketId);
	updateHash(hash, _snapshot.lastLoad.kind);
	updateHash(hash, _snapshot.lastLoad.tile);
	updateHash(hash, _snapshot.lastLoad.uls);
	updateHash(hash, _snapshot.lastLoad.ult);
	updateHash(hash, _snapshot.lastLoad.lrs);
	updateHash(hash, _snapshot.lastLoad.lrt);
	updateHash(hash, _snapshot.lastLoad.dxt);
	updateHash(hash, _snapshot.changedMask);
	return hash;
}

u64 combineStateHash(const rvk2::RDPStateSnapshot & _rdpSnapshot, const rvk2::TMEMSnapshot & _tmemSnapshot)
{
	u64 hash = kFnvOffset;
	updateHash(hash, hashRDPState(_rdpSnapshot));
	updateHash(hash, hashTMEMState(_tmemSnapshot));
	return hash;
}

u64 hashDrawSemanticPacket(const rvk2::DrawSemanticPacket & _semantic)
{
	u64 hash = kFnvOffset;
	updateHash(hash, _semantic.sourcePacketId);
	updateHash(hash, _semantic.sourceOpcode);
	updateHash(hash, _semantic.drawType);
	updateHash(hash, _semantic.tile);
	updateHash(hash, _semantic.texRectFlip);
	updateHash(hash, _semantic.cycleType);
	updateHash(hash, _semantic.combineMux);
	updateHash(hash, _semantic.blendMux1);
	updateHash(hash, _semantic.blendMux2);
	updateHash(hash, _semantic.blendParams);
	updateHash(hash, _semantic.rectULX);
	updateHash(hash, _semantic.rectULY);
	updateHash(hash, _semantic.rectLRX);
	updateHash(hash, _semantic.rectLRY);
	updateHash(hash, _semantic.texS);
	updateHash(hash, _semantic.texT);
	updateHash(hash, _semantic.texDSDX);
	updateHash(hash, _semantic.texDTDY);
	updateHash(hash, _semantic.triangleLMajor);
	updateHash(hash, _semantic.triangleLevel);
	updateHash(hash, _semantic.triangleYL);
	updateHash(hash, _semantic.triangleYM);
	updateHash(hash, _semantic.triangleYH);
	updateHash(hash, _semantic.triangleXL);
	updateHash(hash, _semantic.triangleXH);
	updateHash(hash, _semantic.triangleXM);
	updateHash(hash, _semantic.triangleDxLDY);
	updateHash(hash, _semantic.triangleDxHDY);
	updateHash(hash, _semantic.triangleDxMDY);
	updateHash(hash, _semantic.triangleShadeEnable);
	updateHash(hash, _semantic.triangleTextureEnable);
	updateHash(hash, _semantic.triangleZBufferEnable);
	updateHash(hash, _semantic.triangleShadeR);
	updateHash(hash, _semantic.triangleShadeG);
	updateHash(hash, _semantic.triangleShadeB);
	updateHash(hash, _semantic.triangleShadeA);
	updateHash(hash, _semantic.triangleShadeDRDX);
	updateHash(hash, _semantic.triangleShadeDGDX);
	updateHash(hash, _semantic.triangleShadeDBDX);
	updateHash(hash, _semantic.triangleShadeDADX);
	updateHash(hash, _semantic.triangleShadeDRDE);
	updateHash(hash, _semantic.triangleShadeDGDE);
	updateHash(hash, _semantic.triangleShadeDBDE);
	updateHash(hash, _semantic.triangleShadeDADE);
	updateHash(hash, _semantic.triangleShadeDRDY);
	updateHash(hash, _semantic.triangleShadeDGDY);
	updateHash(hash, _semantic.triangleShadeDBDY);
	updateHash(hash, _semantic.triangleShadeDADY);
	updateHash(hash, _semantic.triangleTexS);
	updateHash(hash, _semantic.triangleTexT);
	updateHash(hash, _semantic.triangleTexW);
	updateHash(hash, _semantic.triangleTexDSDX);
	updateHash(hash, _semantic.triangleTexDTDX);
	updateHash(hash, _semantic.triangleTexDWDX);
	updateHash(hash, _semantic.triangleTexDSDE);
	updateHash(hash, _semantic.triangleTexDTDE);
	updateHash(hash, _semantic.triangleTexDWDE);
	updateHash(hash, _semantic.triangleTexDSDY);
	updateHash(hash, _semantic.triangleTexDTDY);
	updateHash(hash, _semantic.triangleTexDWDY);
	updateHash(hash, _semantic.triangleZ);
	updateHash(hash, _semantic.triangleDZDX);
	updateHash(hash, _semantic.triangleDZDE);
	updateHash(hash, _semantic.triangleDZDY);
	updateHash(hash, _semantic.textured);
	updateHash(hash, _semantic.depthTest);
	updateHash(hash, _semantic.syncEpoch);
	updateHash(hash, _semantic.loadSyncPacketId);
	updateHash(hash, _semantic.pipeSyncPacketId);
	updateHash(hash, _semantic.tileSyncPacketId);
	updateHash(hash, _semantic.fullSyncPacketId);
	return hash;
}

u64 hashDrawSemantics(const std::vector<rvk2::DrawSemanticPacket> & _semantics)
{
	u64 hash = kFnvOffset;
	for (const rvk2::DrawSemanticPacket & semantic : _semantics)
		updateHash(hash, hashDrawSemanticPacket(semantic));
	return hash;
}

u64 hashRasterOpPacket(const rvk2::RasterOpPacket & _op)
{
	u64 hash = kFnvOffset;
	updateHash(hash, _op.sourcePacketId);
	updateHash(hash, _op.sourceOpcode);
	updateHash(hash, _op.opKind);
	updateHash(hash, _op.cycleType);
	updateHash(hash, _op.tile);
	updateHash(hash, _op.texRectFlip);
	updateHash(hash, _op.textured);
	updateHash(hash, _op.depthTest);
	updateHash(hash, _op.rectULX);
	updateHash(hash, _op.rectULY);
	updateHash(hash, _op.rectLRX);
	updateHash(hash, _op.rectLRY);
	updateHash(hash, _op.texS);
	updateHash(hash, _op.texT);
	updateHash(hash, _op.texDSDX);
	updateHash(hash, _op.texDTDY);
	updateHash(hash, _op.triangleLMajor);
	updateHash(hash, _op.triangleLevel);
	updateHash(hash, _op.triangleYL);
	updateHash(hash, _op.triangleYM);
	updateHash(hash, _op.triangleYH);
	updateHash(hash, _op.triangleXL);
	updateHash(hash, _op.triangleXH);
	updateHash(hash, _op.triangleXM);
	updateHash(hash, _op.triangleDxLDY);
	updateHash(hash, _op.triangleDxHDY);
	updateHash(hash, _op.triangleDxMDY);
	updateHash(hash, _op.triangleShadeEnable);
	updateHash(hash, _op.triangleTextureEnable);
	updateHash(hash, _op.triangleZBufferEnable);
	updateHash(hash, _op.triangleShadeR);
	updateHash(hash, _op.triangleShadeG);
	updateHash(hash, _op.triangleShadeB);
	updateHash(hash, _op.triangleShadeA);
	updateHash(hash, _op.triangleShadeDRDX);
	updateHash(hash, _op.triangleShadeDGDX);
	updateHash(hash, _op.triangleShadeDBDX);
	updateHash(hash, _op.triangleShadeDADX);
	updateHash(hash, _op.triangleShadeDRDE);
	updateHash(hash, _op.triangleShadeDGDE);
	updateHash(hash, _op.triangleShadeDBDE);
	updateHash(hash, _op.triangleShadeDADE);
	updateHash(hash, _op.triangleShadeDRDY);
	updateHash(hash, _op.triangleShadeDGDY);
	updateHash(hash, _op.triangleShadeDBDY);
	updateHash(hash, _op.triangleShadeDADY);
	updateHash(hash, _op.triangleTexS);
	updateHash(hash, _op.triangleTexT);
	updateHash(hash, _op.triangleTexW);
	updateHash(hash, _op.triangleTexDSDX);
	updateHash(hash, _op.triangleTexDTDX);
	updateHash(hash, _op.triangleTexDWDX);
	updateHash(hash, _op.triangleTexDSDE);
	updateHash(hash, _op.triangleTexDTDE);
	updateHash(hash, _op.triangleTexDWDE);
	updateHash(hash, _op.triangleTexDSDY);
	updateHash(hash, _op.triangleTexDTDY);
	updateHash(hash, _op.triangleTexDWDY);
	updateHash(hash, _op.triangleZ);
	updateHash(hash, _op.triangleDZDX);
	updateHash(hash, _op.triangleDZDE);
	updateHash(hash, _op.triangleDZDY);
	updateHash(hash, _op.combineMux);
	updateHash(hash, _op.blendParams);
	updateHash(hash, _op.fillColor);
	updateHash(hash, _op.syncEpoch);
	updateHash(hash, _op.loadSyncPacketId);
	updateHash(hash, _op.pipeSyncPacketId);
	updateHash(hash, _op.tileSyncPacketId);
	updateHash(hash, _op.fullSyncPacketId);
	return hash;
}

u64 hashRasterOps(const std::vector<rvk2::RasterOpPacket> & _ops)
{
	u64 hash = kFnvOffset;
	for (const rvk2::RasterOpPacket & op : _ops)
		updateHash(hash, hashRasterOpPacket(op));
	return hash;
}

u64 hashRenderWorkPacket(const rvk2::RenderWorkPacket & _work)
{
	u64 hash = kFnvOffset;
	updateHash(hash, _work.sourcePacketId);
	updateHash(hash, _work.sourceOpcode);
	updateHash(hash, _work.opKind);
	updateHash(hash, _work.phase);
	updateHash(hash, _work.cycleType);
	updateHash(hash, _work.barrierMask);
	updateHash(hash, _work.tile);
	updateHash(hash, _work.texRectFlip);
	updateHash(hash, _work.textured);
	updateHash(hash, _work.depthTest);
	updateHash(hash, _work.depthCompareEnable);
	updateHash(hash, _work.depthUpdateEnable);
	updateHash(hash, _work.rectULX);
	updateHash(hash, _work.rectULY);
	updateHash(hash, _work.rectLRX);
	updateHash(hash, _work.rectLRY);
	updateHash(hash, _work.texS);
	updateHash(hash, _work.texT);
	updateHash(hash, _work.texDSDX);
	updateHash(hash, _work.texDTDY);
	updateHash(hash, _work.triangleLMajor);
	updateHash(hash, _work.triangleLevel);
	updateHash(hash, _work.triangleYL);
	updateHash(hash, _work.triangleYM);
	updateHash(hash, _work.triangleYH);
	updateHash(hash, _work.triangleXL);
	updateHash(hash, _work.triangleXH);
	updateHash(hash, _work.triangleXM);
	updateHash(hash, _work.triangleDxLDY);
	updateHash(hash, _work.triangleDxHDY);
	updateHash(hash, _work.triangleDxMDY);
	updateHash(hash, _work.triangleShadeEnable);
	updateHash(hash, _work.triangleTextureEnable);
	updateHash(hash, _work.triangleZBufferEnable);
	updateHash(hash, _work.triangleShadeR);
	updateHash(hash, _work.triangleShadeG);
	updateHash(hash, _work.triangleShadeB);
	updateHash(hash, _work.triangleShadeA);
	updateHash(hash, _work.triangleShadeDRDX);
	updateHash(hash, _work.triangleShadeDGDX);
	updateHash(hash, _work.triangleShadeDBDX);
	updateHash(hash, _work.triangleShadeDADX);
	updateHash(hash, _work.triangleShadeDRDE);
	updateHash(hash, _work.triangleShadeDGDE);
	updateHash(hash, _work.triangleShadeDBDE);
	updateHash(hash, _work.triangleShadeDADE);
	updateHash(hash, _work.triangleShadeDRDY);
	updateHash(hash, _work.triangleShadeDGDY);
	updateHash(hash, _work.triangleShadeDBDY);
	updateHash(hash, _work.triangleShadeDADY);
	updateHash(hash, _work.triangleTexS);
	updateHash(hash, _work.triangleTexT);
	updateHash(hash, _work.triangleTexW);
	updateHash(hash, _work.triangleTexDSDX);
	updateHash(hash, _work.triangleTexDTDX);
	updateHash(hash, _work.triangleTexDWDX);
	updateHash(hash, _work.triangleTexDSDE);
	updateHash(hash, _work.triangleTexDTDE);
	updateHash(hash, _work.triangleTexDWDE);
	updateHash(hash, _work.triangleTexDSDY);
	updateHash(hash, _work.triangleTexDTDY);
	updateHash(hash, _work.triangleTexDWDY);
	updateHash(hash, _work.triangleZ);
	updateHash(hash, _work.triangleDZDX);
	updateHash(hash, _work.triangleDZDE);
	updateHash(hash, _work.triangleDZDY);
	updateHash(hash, _work.colorImageFormat);
	updateHash(hash, _work.colorImageSize);
	updateHash(hash, _work.colorImageWidth);
	updateHash(hash, _work.colorImageAddress);
	updateHash(hash, _work.depthImageAddress);
	updateHash(hash, _work.alphaCompare);
	updateHash(hash, _work.cvgDest);
	updateHash(hash, _work.blendMask);
	updateHash(hash, _work.cvgXAlpha);
	updateHash(hash, _work.alphaCvgSel);
	updateHash(hash, _work.colorOnCvg);
	updateHash(hash, _work.forceBlender);
	updateHash(hash, _work.depthSource);
	updateHash(hash, _work.primDepthZ);
	updateHash(hash, _work.primDepthDelta);
	updateHash(hash, _work.otherModes);
	updateHash(hash, _work.primColor);
	updateHash(hash, _work.envColor);
	updateHash(hash, _work.blendColor);
	updateHash(hash, _work.fogColor);
	updateHash(hash, _work.primColorMinLevel);
	updateHash(hash, _work.primColorLodFrac);
	updateHash(hash, _work.convertK4);
	updateHash(hash, _work.convertK5);
	updateHash(hash, _work.keyCenterR);
	updateHash(hash, _work.keyScaleR);
	updateHash(hash, _work.keyCenterG);
	updateHash(hash, _work.keyScaleG);
	updateHash(hash, _work.keyCenterB);
	updateHash(hash, _work.keyScaleB);
	updateHash(hash, _work.keyState);
	updateHash(hash, _work.convertState);
	updateHash(hash, _work.scissorMode);
	updateHash(hash, _work.scissorXH);
	updateHash(hash, _work.scissorYH);
	updateHash(hash, _work.scissorXL);
	updateHash(hash, _work.scissorYL);
	updateHash(hash, _work.textureImageFormat);
	updateHash(hash, _work.textureImageSize);
	updateHash(hash, _work.textureImageWidth);
	updateHash(hash, _work.textureImageAddress);
	updateHash(hash, _work.tileFormat);
	updateHash(hash, _work.tileSize);
	updateHash(hash, _work.tileLine);
	updateHash(hash, _work.tileTmem);
	updateHash(hash, _work.tilePalette);
	updateHash(hash, _work.tileCmt);
	updateHash(hash, _work.tileCms);
	updateHash(hash, _work.tileMaskt);
	updateHash(hash, _work.tileMasks);
	updateHash(hash, _work.tileShiftt);
	updateHash(hash, _work.tileShifts);
	updateHash(hash, _work.tileULS);
	updateHash(hash, _work.tileULT);
	updateHash(hash, _work.tileLRS);
	updateHash(hash, _work.tileLRT);
	updateHash(hash, _work.tmemLoadKind);
	updateHash(hash, _work.tmemLoadTile);
	updateHash(hash, _work.tmemLoadULS);
	updateHash(hash, _work.tmemLoadULT);
	updateHash(hash, _work.tmemLoadLRS);
	updateHash(hash, _work.tmemLoadLRT);
	updateHash(hash, _work.tmemLoadDXT);
	updateHash(hash, _work.combineMux);
	updateHash(hash, _work.blendParams);
	updateHash(hash, _work.fillColor);
	updateHash(hash, _work.syncEpoch);
	updateHash(hash, _work.loadSyncPacketId);
	updateHash(hash, _work.pipeSyncPacketId);
	updateHash(hash, _work.tileSyncPacketId);
	updateHash(hash, _work.fullSyncPacketId);
	return hash;
}

u64 hashRenderPlan(const std::vector<rvk2::RenderWorkPacket> & _work)
{
	u64 hash = kFnvOffset;
	for (const rvk2::RenderWorkPacket & packet : _work)
		updateHash(hash, hashRenderWorkPacket(packet));
	return hash;
}

u64 hashSubmissionBatch(const rvk2::SubmissionBatchPacket & _batch)
{
	u64 hash = kFnvOffset;
	updateHash(hash, _batch.batchIndex);
	updateHash(hash, _batch.splitReason);
	updateHash(hash, _batch.splitBarrierMask);
	updateHash(hash, _batch.phase);
	updateHash(hash, _batch.cycleType);
	updateHash(hash, _batch.firstWorkIndex);
	updateHash(hash, _batch.lastWorkIndex);
	updateHash(hash, _batch.workCount);
	updateHash(hash, _batch.barrierMaskUnion);
	updateHash(hash, _batch.texturedWorkCount);
	updateHash(hash, _batch.depthTestWorkCount);
	updateHash(hash, _batch.firstSourcePacketId);
	updateHash(hash, _batch.lastSourcePacketId);
	updateHash(hash, _batch.colorImageFormat);
	updateHash(hash, _batch.colorImageSize);
	updateHash(hash, _batch.colorImageWidth);
	updateHash(hash, _batch.colorImageAddress);
	updateHash(hash, _batch.depthImageAddress);
	updateHash(hash, _batch.scissorMode);
	updateHash(hash, _batch.scissorXH);
	updateHash(hash, _batch.scissorYH);
	updateHash(hash, _batch.scissorXL);
	updateHash(hash, _batch.scissorYL);
	return hash;
}

u64 hashSubmissionPlan(const std::vector<rvk2::SubmissionBatchPacket> & _batches)
{
	u64 hash = kFnvOffset;
	for (const rvk2::SubmissionBatchPacket & batch : _batches)
		updateHash(hash, hashSubmissionBatch(batch));
	return hash;
}

bool isKnownRdpOpcode(u8 _opcode)
{
	switch (_opcode) {
	case 0x00U: // NoOp
	case 0x08U: // TriFill
	case 0x09U: // TriFillZ
	case 0x0AU: // TriTxtr
	case 0x0BU: // TriTxtrZ
	case 0x0CU: // TriShade
	case 0x0DU: // TriShadeZ
	case 0x0EU: // TriShadeTxtr
	case 0x0FU: // TriShadeTxtrZ
	case 0x24U: // TexRect
	case 0x25U: // TexRectFlip
	case 0x26U: // LoadSync
	case 0x27U: // PipeSync
	case 0x28U: // TileSync
	case 0x29U: // FullSync
	case 0x2AU: // SetKeyGB
	case 0x2BU: // SetKeyR
	case 0x2CU: // SetConvert
	case 0x2DU: // SetScissor
	case 0x2EU: // SetPrimDepth
	case 0x2FU: // SetOtherModes
	case 0x30U: // LoadTLUT
	case 0x32U: // SetTileSize
	case 0x33U: // LoadBlock
	case 0x34U: // LoadTile
	case 0x35U: // SetTile
	case 0x36U: // FillRect
	case 0x37U: // SetFillColor
	case 0x38U: // SetFogColor
	case 0x39U: // SetBlendColor
	case 0x3AU: // SetPrimColor
	case 0x3BU: // SetEnvColor
	case 0x3CU: // SetCombine
	case 0x3DU: // SetTextureImage
	case 0x3EU: // SetDepthImage
	case 0x3FU: // SetColorImage
		return true;
	default:
		break;
	}
	return false;
}

} // namespace

namespace rvk2 {

Runtime::Runtime()
	: m_rspFrontend()
	, m_commandStream()
	, m_drawSemantics()
	, m_rasterOps()
	, m_renderPlan()
	, m_submissionBatches()
	, m_renderPlanState()
	, m_rdpState()
	, m_tmemModel()
{
}

void Runtime::reset()
{
	m_rspFrontend.reset();
	m_commandStream.reset(0ULL);
	m_drawSemantics.clear();
	m_rasterOps.clear();
	m_renderPlan.clear();
	m_submissionBatches.clear();
	m_renderPlanState = RenderPlanState{};
	m_rdpState.reset();
	m_tmemModel.reset();
	m_unknownRdpOpcodeCount = 0U;
	m_firstUnknownRdpPacketId = 0ULL;
	m_firstUnknownRdpOpcode = 0U;
	m_truncatedPayloadCount = 0U;
	m_firstTruncatedPayloadPacketId = 0ULL;
	m_firstTruncatedPayloadOpcode = 0U;
}

void Runtime::beginFrame(u64 _frameId)
{
	m_commandStream.reset(_frameId);
	m_drawSemantics.clear();
	m_rasterOps.clear();
	m_renderPlan.clear();
	m_submissionBatches.clear();
	m_renderPlanState = RenderPlanState{};
	m_unknownRdpOpcodeCount = 0U;
	m_firstUnknownRdpPacketId = 0ULL;
	m_firstUnknownRdpOpcode = 0U;
	m_truncatedPayloadCount = 0U;
	m_firstTruncatedPayloadPacketId = 0ULL;
	m_firstTruncatedPayloadOpcode = 0U;
}

void Runtime::submitRDPWord(
	u32 _dlistAddress,
	u32 _w0,
	u32 _w1,
	const CommandProvenance & _provenance,
	u8 _extraWordCount,
	u32 _w2,
	u32 _w3,
	u32 _w4,
	u32 _w5,
	u32 _w6,
	u32 _w7,
	u16 _fullWordCount,
	u64 _tailHash,
	u8 _payloadWordCount,
	const u32 * _payloadWords)
{
	const CommandPacket packet =
		m_rspFrontend.ingestRDPCommand(
			_dlistAddress,
			_w0,
			_w1,
			_provenance,
			_extraWordCount,
			_w2,
			_w3,
			_w4,
			_w5,
			_w6,
			_w7,
			_fullWordCount,
			_tailHash,
			_payloadWordCount,
			_payloadWords);
	m_commandStream.push(packet);
	if (!isKnownRdpOpcode(packet.opcode)) {
		++m_unknownRdpOpcodeCount;
		if (m_firstUnknownRdpPacketId == 0ULL) {
			m_firstUnknownRdpPacketId = packet.id;
			m_firstUnknownRdpOpcode = packet.opcode;
		}
	}
	const u16 capturedPayloadWords = static_cast<u16>(
		packet.payloadWordCount > 0U ? packet.payloadWordCount : packet.extraWordCount);
	if (packet.fullWordCount > static_cast<u16>(2U + capturedPayloadWords)) {
		++m_truncatedPayloadCount;
		if (m_firstTruncatedPayloadPacketId == 0ULL) {
			m_firstTruncatedPayloadPacketId = packet.id;
			m_firstTruncatedPayloadOpcode = packet.opcode;
		}
	}
	m_rdpState.apply(packet);
	m_tmemModel.apply(packet);
	if (isDrawOpcode(packet.opcode)) {
		const RDPStateSnapshot & rdpSnapshot = m_rdpState.snapshot();
		const TMEMSnapshot & tmemSnapshot = m_tmemModel.snapshot();
		const DrawSemanticPacket semantic =
			buildDrawSemanticPacket(packet, rdpSnapshot, tmemSnapshot);
		m_drawSemantics.push_back(semantic);
		if (isRasterizableSemantic(semantic)) {
			const RasterOpPacket op = buildRasterOpPacket(semantic, rdpSnapshot);
			m_rasterOps.push_back(op);
			if (isRenderableRasterOp(op)) {
				const RenderWorkPacket work =
					buildRenderWorkPacket(op, rdpSnapshot, tmemSnapshot, m_renderPlanState);
				const u32 workIndex = static_cast<u32>(m_renderPlan.size());
				m_renderPlan.push_back(work);
				appendRenderWorkToSubmissionPlan(work, workIndex, m_submissionBatches);
			}
		}
	}
}

void Runtime::submitRSPWord(
	u32 _dlistAddress,
	u32 _w0,
	u32 _w1,
	const CommandProvenance & _provenance,
	u8 _extraWordCount,
	u32 _w2,
	u32 _w3,
	u32 _w4,
	u32 _w5,
	u32 _w6,
	u32 _w7,
	u16 _fullWordCount,
	u64 _tailHash,
	u8 _payloadWordCount,
	const u32 * _payloadWords)
{
	const CommandPacket packet =
		m_rspFrontend.ingestRSPCommand(
			_dlistAddress,
			_w0,
			_w1,
			_provenance,
			_extraWordCount,
			_w2,
			_w3,
			_w4,
			_w5,
			_w6,
			_w7,
			_fullWordCount,
			_tailHash,
			_payloadWordCount,
			_payloadWords);
	m_commandStream.push(packet);
}

const CommandStream & Runtime::commandStream() const
{
	return m_commandStream;
}

const std::vector<DrawSemanticPacket> & Runtime::drawSemantics() const
{
	return m_drawSemantics;
}

const std::vector<RasterOpPacket> & Runtime::rasterOps() const
{
	return m_rasterOps;
}

const std::vector<RenderWorkPacket> & Runtime::renderPlan() const
{
	return m_renderPlan;
}

const std::vector<SubmissionBatchPacket> & Runtime::submissionPlan() const
{
	return m_submissionBatches;
}

const RDPStateEngine & Runtime::rdpState() const
{
	return m_rdpState;
}

const TMEMModel & Runtime::tmemModel() const
{
	return m_tmemModel;
}

FrameTraceRecord Runtime::buildFrameTrace() const
{
	FrameTraceRecord record{};
	record.frameId = m_commandStream.frameId();
	record.commandCount = static_cast<u64>(m_commandStream.commands().size());
	record.drawSemanticCount = static_cast<u64>(m_drawSemantics.size());
	record.rasterOpCount = static_cast<u64>(m_rasterOps.size());
	record.renderWorkCount = static_cast<u64>(m_renderPlan.size());
	record.submissionBatchCount = static_cast<u64>(m_submissionBatches.size());
	record.lastPacketId = m_rdpState.snapshot().lastPacketId;
	record.commandHash = m_commandStream.commandHash();
	record.stateHash = combineStateHash(m_rdpState.snapshot(), m_tmemModel.snapshot());
	record.drawSemanticHash = hashDrawSemantics(m_drawSemantics);
	record.rasterOpHash = hashRasterOps(m_rasterOps);
	record.renderWorkHash = hashRenderPlan(m_renderPlan);
	record.submissionBatchHash = hashSubmissionPlan(m_submissionBatches);
	const ExecutorSummary execSummary =
		Executor(loadExecutorConfigFromEnv()).execute(m_renderPlan, m_submissionBatches);
	record.executorWorkCount = execSummary.executedWorkCount;
	record.executorBatchCount = execSummary.executedBatchCount;
	record.executorColorWriteCount = execSummary.colorWriteCount;
	record.executorSurfaceCount = execSummary.surfaceCount;
	record.executorPresentHash = execSummary.presentHash;
	record.executorPresentWidth = execSummary.presentWidth;
	record.executorPresentHeight = execSummary.presentHeight;
	record.executorPresentAspectX = execSummary.presentAspectX;
	record.executorPresentAspectY = execSummary.presentAspectY;
	record.unknownRdpOpcodeCount = m_unknownRdpOpcodeCount;
	record.firstUnknownRdpPacketId = m_firstUnknownRdpPacketId;
	record.firstUnknownRdpOpcode = m_firstUnknownRdpOpcode;
	record.truncatedPayloadCount = m_truncatedPayloadCount;
	record.firstTruncatedPayloadPacketId = m_firstTruncatedPayloadPacketId;
	record.firstTruncatedPayloadOpcode = m_firstTruncatedPayloadOpcode;
	return record;
}

Runtime & runtime()
{
	static Runtime s_runtime;
	return s_runtime;
}

} // namespace rvk2
