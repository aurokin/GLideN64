#pragma once

#include <Types.h>

namespace rvk2 {

constexpr u32 kSchemaVersion = 1U;
constexpr const char * kSchemaName = "rvk2_schema_v1";
constexpr u32 kMaxCommandPayloadWords = 42U;

using PacketId = u64;

enum class CommandDomain : u8 {
	kUnknown = 0U,
	kRSP,
	kRDP,
	kVI
};

struct CommandProvenance {
	u32 taskId = 0U;
	u32 dlistAddress = 0U;
	u16 microcode = 0U;
	u16 reserved = 0U;
};

struct CommandPacket {
	PacketId id = 0U;
	CommandDomain domain = CommandDomain::kUnknown;
	u8 opcode = 0U;
	u8 flags = 0U;
	u8 extraWordCount = 0U;
	u8 payloadWordCount = 0U;
	u16 fullWordCount = 0U;
	u32 w0 = 0U;
	u32 w1 = 0U;
	u32 w2 = 0U;
	u32 w3 = 0U;
	u32 w4 = 0U;
	u32 w5 = 0U;
	u32 w6 = 0U;
	u32 w7 = 0U;
	u32 payloadWords[kMaxCommandPayloadWords]{};
	u64 tailHash = 1469598103934665603ULL;
	CommandProvenance provenance{};
};

namespace rdp_state_changed {
	constexpr u32 kNone = 0U;
	constexpr u32 kCombine = 1U << 0;
	constexpr u32 kOtherModes = 1U << 1;
	constexpr u32 kScissor = 1U << 2;
	constexpr u32 kColorImage = 1U << 3;
	constexpr u32 kDepthImage = 1U << 4;
	constexpr u32 kLoadSync = 1U << 5;
	constexpr u32 kPipeSync = 1U << 6;
	constexpr u32 kTileSync = 1U << 7;
	constexpr u32 kFullSync = 1U << 8;
	constexpr u32 kPrimColor = 1U << 9;
	constexpr u32 kEnvColor = 1U << 10;
	constexpr u32 kBlendColor = 1U << 11;
	constexpr u32 kFogColor = 1U << 12;
	constexpr u32 kFillColor = 1U << 13;
	constexpr u32 kPrimDepth = 1U << 14;
	constexpr u32 kConvert = 1U << 15;
	constexpr u32 kKeyR = 1U << 16;
	constexpr u32 kKeyGB = 1U << 17;
}

constexpr u32 kTileCount = 8U;

namespace tmem_state_changed {
	constexpr u32 kNone = 0U;
	constexpr u32 kTextureImage = 1U << 0;
	constexpr u32 kTileDescriptor = 1U << 1;
	constexpr u32 kTileSize = 1U << 2;
	constexpr u32 kLoadTile = 1U << 3;
	constexpr u32 kLoadBlock = 1U << 4;
	constexpr u32 kLoadTLUT = 1U << 5;
}

enum class TmemLoadKind : u8 {
	kNone = 0U,
	kTile,
	kBlock,
	kTLUT
};

struct TextureImageState {
	u8 format = 0U;
	u8 size = 0U;
	u16 width = 0U;
	u32 address = 0U;
};

struct TileDescriptorState {
	u8 format = 0U;
	u8 size = 0U;
	u16 line = 0U;
	u16 tmem = 0U;
	u8 palette = 0U;
	u8 cmt = 0U;
	u8 cms = 0U;
	u8 maskt = 0U;
	u8 masks = 0U;
	u8 shiftt = 0U;
	u8 shifts = 0U;
	u16 uls = 0U;
	u16 ult = 0U;
	u16 lrs = 0U;
	u16 lrt = 0U;
};

struct TMEMLoadRecord {
	PacketId sourcePacketId = 0ULL;
	TmemLoadKind kind = TmemLoadKind::kNone;
	u8 tile = 0U;
	u16 uls = 0U;
	u16 ult = 0U;
	u16 lrs = 0U;
	u16 lrt = 0U;
	u16 dxt = 0U;
};

struct TMEMSnapshot {
	PacketId lastPacketId = 0ULL;
	TextureImageState textureImage{};
	TileDescriptorState tiles[kTileCount]{};
	TMEMLoadRecord lastLoad{};
	u32 changedMask = tmem_state_changed::kNone;
};

struct OtherModesDecoded {
	u8 alphaCompare = 0U;
	u8 depthSource = 0U;
	u8 cvgDest = 0U;
	u8 depthMode = 0U;
	u8 c2_m2b = 0U;
	u8 c1_m2b = 0U;
	u8 c2_m2a = 0U;
	u8 c1_m2a = 0U;
	u8 c2_m1b = 0U;
	u8 c1_m1b = 0U;
	u8 c2_m1a = 0U;
	u8 c1_m1a = 0U;
	u8 blendMask = 0U;
	u8 alphaDither = 0U;
	u8 colorDither = 0U;
	u8 textureFilter = 0U;
	u8 textureLUT = 0U;
	u8 textureDetail = 0U;
	bool aaEnable = false;
	bool depthCompare = false;
	bool depthUpdate = false;
	bool imageRead = false;
	bool colorOnCvg = false;
	bool cvgXAlpha = false;
	bool alphaCvgSel = false;
	bool forceBlender = false;
	bool textureEdge = false;
	bool combineKey = false;
	bool convertOne = false;
	bool biLerp1 = false;
	bool biLerp0 = false;
	bool textureLOD = false;
	bool texturePersp = false;
	bool unusedColorDither = false;
	bool pipelineMode = false;
};

struct ColorRGBA8 {
	u8 r = 0U;
	u8 g = 0U;
	u8 b = 0U;
	u8 a = 0U;
};

struct RDPStateSnapshot {
	PacketId lastPacketId = 0U;
	u64 combineMux = 0ULL;
	u64 otherModes = 0ULL;
	OtherModesDecoded otherModesDecoded{};
	u8 cycleType = 0U;
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
	ColorRGBA8 primColor{};
	ColorRGBA8 envColor{};
	ColorRGBA8 blendColor{};
	ColorRGBA8 fogColor{};
	u8 primColorMinLevel = 0U;
	u8 primColorLodFrac = 0U;
	u32 fillColor = 0U;
	u16 primDepthZ = 0U;
	u16 primDepthDelta = 0U;
	s16 convertK0 = 0;
	s16 convertK1 = 0;
	s16 convertK2 = 0;
	s16 convertK3 = 0;
	s16 convertK4 = 0;
	s16 convertK5 = 0;
	u8 keyCenterR = 0U;
	u8 keyScaleR = 0U;
	u8 keyCenterG = 0U;
	u8 keyScaleG = 0U;
	u8 keyCenterB = 0U;
	u8 keyScaleB = 0U;
	u16 keyWidthR = 0U;
	u16 keyWidthG = 0U;
	u16 keyWidthB = 0U;
	u32 syncEpoch = 0U;
	u32 loadSyncCount = 0U;
	u32 pipeSyncCount = 0U;
	u32 tileSyncCount = 0U;
	u32 fullSyncCount = 0U;
	PacketId loadSyncPacketId = 0ULL;
	PacketId pipeSyncPacketId = 0ULL;
	PacketId tileSyncPacketId = 0ULL;
	PacketId fullSyncPacketId = 0ULL;
	u32 changedMask = rdp_state_changed::kNone;
};

struct DrawSemanticPacket {
	PacketId sourcePacketId = 0U;
	u8 sourceOpcode = 0U;
	u8 drawType = 0U;
	u8 tile = 0U;
	bool texRectFlip = false;
	u8 cycleType = 0U;
	u64 combineMux = 0ULL;
	u32 blendMux1 = 0U;
	u32 blendMux2 = 0U;
	u32 blendParams = 0U;
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
	bool textured = false;
	bool depthTest = false;
	u32 syncEpoch = 0U;
	PacketId loadSyncPacketId = 0ULL;
	PacketId pipeSyncPacketId = 0ULL;
	PacketId tileSyncPacketId = 0ULL;
	PacketId fullSyncPacketId = 0ULL;
};

struct VIScanoutState {
	PacketId sourcePacketId = 0U;
	u32 status = 0U;
	u32 origin = 0U;
	u32 width = 0U;
	u32 xScale = 0U;
	u32 yScale = 0U;
	bool interlaced = false;
	bool gammaEnabled = false;
};

struct FrameTraceRecord {
	u64 frameId = 0ULL;
	u64 commandCount = 0ULL;
	u64 drawSemanticCount = 0ULL;
	u64 rasterOpCount = 0ULL;
	u64 renderWorkCount = 0ULL;
	u64 submissionBatchCount = 0ULL;
	PacketId lastPacketId = 0ULL;
	u64 commandHash = 1469598103934665603ULL;
	u64 stateHash = 1469598103934665603ULL;
	u64 drawSemanticHash = 1469598103934665603ULL;
	u64 rasterOpHash = 1469598103934665603ULL;
	u64 renderWorkHash = 1469598103934665603ULL;
	u64 submissionBatchHash = 1469598103934665603ULL;
	u64 executorWorkCount = 0ULL;
	u64 executorBatchCount = 0ULL;
	u64 executorColorWriteCount = 0ULL;
	u64 executorSurfaceCount = 0ULL;
	u64 executorPresentHash = 1469598103934665603ULL;
	u32 executorPresentWidth = 0U;
	u32 executorPresentHeight = 0U;
	u8 executorPresentAspectX = 4U;
	u8 executorPresentAspectY = 3U;
	u32 unknownRdpOpcodeCount = 0U;
	PacketId firstUnknownRdpPacketId = 0ULL;
	u8 firstUnknownRdpOpcode = 0U;
	u32 truncatedPayloadCount = 0U;
	PacketId firstTruncatedPayloadPacketId = 0ULL;
	u8 firstTruncatedPayloadOpcode = 0U;
};

} // namespace rvk2
