#pragma once

#include "rvk2_Types.h"

namespace rvk2 {

enum class RasterOpKind : u8 {
	kUnknown = 0U,
	kTriangle = 1U,
	kTexRect = 2U,
	kFillRect = 3U
};

struct RasterOpPacket {
	PacketId sourcePacketId = 0ULL;
	u8 sourceOpcode = 0U;
	u8 opKind = static_cast<u8>(RasterOpKind::kUnknown);
	u8 cycleType = 0U;
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
	bool triangleShadeEnable = false;
	bool triangleTextureEnable = false;
	bool triangleZBufferEnable = false;
	s32 triangleShadeR = 0;
	s32 triangleShadeG = 0;
	s32 triangleShadeB = 0;
	s32 triangleShadeA = 0;
	s32 triangleShadeDRDX = 0;
	s32 triangleShadeDGDX = 0;
	s32 triangleShadeDBDX = 0;
	s32 triangleShadeDADX = 0;
	s32 triangleShadeDRDE = 0;
	s32 triangleShadeDGDE = 0;
	s32 triangleShadeDBDE = 0;
	s32 triangleShadeDADE = 0;
	s32 triangleShadeDRDY = 0;
	s32 triangleShadeDGDY = 0;
	s32 triangleShadeDBDY = 0;
	s32 triangleShadeDADY = 0;
	s32 triangleTexS = 0;
	s32 triangleTexT = 0;
	s32 triangleTexW = 0;
	s32 triangleTexDSDX = 0;
	s32 triangleTexDTDX = 0;
	s32 triangleTexDWDX = 0;
	s32 triangleTexDSDE = 0;
	s32 triangleTexDTDE = 0;
	s32 triangleTexDWDE = 0;
	s32 triangleTexDSDY = 0;
	s32 triangleTexDTDY = 0;
	s32 triangleTexDWDY = 0;
	s32 triangleZ = 0;
	s32 triangleDZDX = 0;
	s32 triangleDZDE = 0;
	s32 triangleDZDY = 0;
	u64 combineMux = 0ULL;
	u32 blendParams = 0U;
	u32 fillColor = 0U;
	u32 syncEpoch = 0U;
	PacketId loadSyncPacketId = 0ULL;
	PacketId pipeSyncPacketId = 0ULL;
	PacketId tileSyncPacketId = 0ULL;
	PacketId fullSyncPacketId = 0ULL;
};

bool isRasterizableSemantic(const DrawSemanticPacket & _semantic);

RasterOpPacket buildRasterOpPacket(
	const DrawSemanticPacket & _semantic,
	const RDPStateSnapshot & _rdpState);

} // namespace rvk2
