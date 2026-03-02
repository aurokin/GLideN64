#pragma once

#include "rvk2_RasterPipeline.h"
#include "rvk2_Types.h"

namespace rvk2 {

enum class RenderPhase : u8 {
	kUnknown = 0U,
	kCycle1 = 1U,
	kCycle2 = 2U,
	kCopy = 3U,
	kFill = 4U
};

namespace render_barrier {
	constexpr u8 kNone = 0U;
	constexpr u8 kLoadSync = 1U << 0;
	constexpr u8 kPipeSync = 1U << 1;
	constexpr u8 kTileSync = 1U << 2;
	constexpr u8 kFullSync = 1U << 3;
}

struct RenderWorkPacket {
	PacketId sourcePacketId = 0ULL;
	u8 sourceOpcode = 0U;
	u8 opKind = static_cast<u8>(RasterOpKind::kUnknown);
	u8 phase = static_cast<u8>(RenderPhase::kUnknown);
	u8 cycleType = 0U;
	u8 barrierMask = render_barrier::kNone;
	u8 tile = 0U;
	bool texRectFlip = false;
	bool textured = false;
	bool depthTest = false;
	u16 rectULX = 0U;
	u16 rectULY = 0U;
	u16 rectLRX = 0U;
	u16 rectLRY = 0U;
	s16 texS = 0;
	s16 texT = 0;
	s16 texDSDX = 0;
	s16 texDTDY = 0;
	bool triangleLMajor = false;
	u8 triangleLevel = 0U;
	u16 triangleYL = 0U;
	u16 triangleYM = 0U;
	u16 triangleYH = 0U;
	s32 triangleXL = 0;
	s32 triangleXH = 0;
	s32 triangleXM = 0;
	s32 triangleDxLDY = 0;
	s32 triangleDxHDY = 0;
	s32 triangleDxMDY = 0;
	u8 colorImageFormat = 0U;
	u8 colorImageSize = 0U;
	u16 colorImageWidth = 0U;
	u32 colorImageAddress = 0U;
	u32 depthImageAddress = 0U;
	u8 scissorMode = 0U;
	u16 scissorXH = 0U;
	u16 scissorYH = 0U;
	u16 scissorXL = 0U;
	u16 scissorYL = 0U;
	u8 textureImageFormat = 0U;
	u8 textureImageSize = 0U;
	u16 textureImageWidth = 0U;
	u32 textureImageAddress = 0U;
	u8 tileFormat = 0U;
	u8 tileSize = 0U;
	u16 tileLine = 0U;
	u16 tileTmem = 0U;
	u8 tilePalette = 0U;
	u8 tileCmt = 0U;
	u8 tileCms = 0U;
	u8 tileMaskt = 0U;
	u8 tileMasks = 0U;
	u8 tileShiftt = 0U;
	u8 tileShifts = 0U;
	u16 tileULS = 0U;
	u16 tileULT = 0U;
	u16 tileLRS = 0U;
	u16 tileLRT = 0U;
	u8 tmemLoadKind = static_cast<u8>(TmemLoadKind::kNone);
	u8 tmemLoadTile = 0U;
	u16 tmemLoadULS = 0U;
	u16 tmemLoadULT = 0U;
	u16 tmemLoadLRS = 0U;
	u16 tmemLoadLRT = 0U;
	u16 tmemLoadDXT = 0U;
	u64 combineMux = 0ULL;
	u32 blendParams = 0U;
	u32 fillColor = 0U;
	u32 syncEpoch = 0U;
	PacketId loadSyncPacketId = 0ULL;
	PacketId pipeSyncPacketId = 0ULL;
	PacketId tileSyncPacketId = 0ULL;
	PacketId fullSyncPacketId = 0ULL;
};

struct RenderPlanState {
	PacketId lastLoadSyncPacketId = 0ULL;
	PacketId lastPipeSyncPacketId = 0ULL;
	PacketId lastTileSyncPacketId = 0ULL;
	PacketId lastFullSyncPacketId = 0ULL;
};

bool isRenderableRasterOp(const RasterOpPacket & _op);

RenderWorkPacket buildRenderWorkPacket(
	const RasterOpPacket & _op,
	const RDPStateSnapshot & _rdpState,
	const TMEMSnapshot & _tmemState,
	RenderPlanState & _planState);

} // namespace rvk2
