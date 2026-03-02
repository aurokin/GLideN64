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
