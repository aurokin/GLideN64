#include "rvk2_TMEMModel.h"

#include "rvk2_Env.h"

namespace {

constexpr u8 kCmdLoadTLUT = 0x30U;
constexpr u8 kCmdSetTileSize = 0x32U;
constexpr u8 kCmdLoadBlock = 0x33U;
constexpr u8 kCmdLoadTile = 0x34U;
constexpr u8 kCmdSetTile = 0x35U;
constexpr u8 kCmdSetTextureImage = 0x3DU;
// SetTextureImage exposes dramAddress[23:0] in the command word.
constexpr u32 kRdramAddressMask = 0x00FFFFFFU;

inline u32 bitRange(u32 _value, u32 _shift, u32 _width)
{
	return (_value >> _shift) & ((1U << _width) - 1U);
}

inline bool decodeTileIndex(u32 _w1, u8 & _tileIndex)
{
	const u32 tile = bitRange(_w1, 24, 3);
	if (tile >= rvk2::kTileCount)
		return false;
	_tileIndex = static_cast<u8>(tile);
	return true;
}

bool normalizeTileMasks()
{
	static const bool enabled = []() -> bool {
		return rvk2::envFlagEnabled("REALITYVK_RVK2_DEBUG_SETTILE_MASK_NORMALIZE", false);
	}();
	return enabled;
}

} // namespace

namespace rvk2 {

TMEMModel::TMEMModel()
	: m_snapshot()
{
}

void TMEMModel::reset()
{
	m_snapshot = TMEMSnapshot{};
}

void TMEMModel::apply(const CommandPacket & _packet)
{
	if (_packet.domain != CommandDomain::kRDP)
		return;

	switch (_packet.opcode) {
	case kCmdSetTextureImage:
		applySetTextureImage(_packet);
		break;
	case kCmdSetTile:
		applySetTile(_packet);
		break;
	case kCmdSetTileSize:
		applySetTileSize(_packet);
		break;
	case kCmdLoadTile:
		applyLoadTile(_packet);
		break;
	case kCmdLoadBlock:
		applyLoadBlock(_packet);
		break;
	case kCmdLoadTLUT:
		applyLoadTLUT(_packet);
		break;
	default:
		break;
	}

	m_snapshot.lastPacketId = _packet.id;
}

const TMEMSnapshot & TMEMModel::snapshot() const
{
	return m_snapshot;
}

void TMEMModel::applySetTextureImage(const CommandPacket & _packet)
{
	m_snapshot.textureImage.format = static_cast<u8>(bitRange(_packet.w0, 21, 3));
	m_snapshot.textureImage.size = static_cast<u8>(bitRange(_packet.w0, 19, 2));
	m_snapshot.textureImage.width = static_cast<u16>(bitRange(_packet.w0, 0, 12) + 1U);
	m_snapshot.textureImage.address = _packet.w1 & kRdramAddressMask;
	m_snapshot.changedMask |= tmem_state_changed::kTextureImage;
}

void TMEMModel::applySetTile(const CommandPacket & _packet)
{
	u8 tileIndex = 0U;
	if (!decodeTileIndex(_packet.w1, tileIndex))
		return;

	TileDescriptorState & tile = m_snapshot.tiles[tileIndex];
	tile.format = static_cast<u8>(bitRange(_packet.w0, 21, 3));
	tile.size = static_cast<u8>(bitRange(_packet.w0, 19, 2));
	tile.line = static_cast<u16>(bitRange(_packet.w0, 9, 9));
	tile.tmem = static_cast<u16>(bitRange(_packet.w0, 0, 9));
	tile.palette = static_cast<u8>(bitRange(_packet.w1, 20, 4));
	tile.cmt = static_cast<u8>(bitRange(_packet.w1, 18, 2));
	tile.cms = static_cast<u8>(bitRange(_packet.w1, 8, 2));
	tile.maskt = static_cast<u8>(bitRange(_packet.w1, 14, 4));
	tile.masks = static_cast<u8>(bitRange(_packet.w1, 4, 4));
	tile.shiftt = static_cast<u8>(bitRange(_packet.w1, 10, 4));
	tile.shifts = static_cast<u8>(bitRange(_packet.w1, 0, 4));
	if (normalizeTileMasks()) {
		// Match legacy RDP tile normalization used by reference implementations:
		// masks above 10 clamp to 10, and mask 0 implies clamp mode.
		if (tile.masks > 10U)
			tile.masks = 10U;
		else if (tile.masks == 0U)
			tile.cms = static_cast<u8>(tile.cms | 0x2U);
		if (tile.maskt > 10U)
			tile.maskt = 10U;
		else if (tile.maskt == 0U)
			tile.cmt = static_cast<u8>(tile.cmt | 0x2U);
	}
	m_snapshot.changedMask |= tmem_state_changed::kTileDescriptor;
}

void TMEMModel::applySetTileSize(const CommandPacket & _packet)
{
	u8 tileIndex = 0U;
	if (!decodeTileIndex(_packet.w1, tileIndex))
		return;

	TileDescriptorState & tile = m_snapshot.tiles[tileIndex];
	tile.uls = static_cast<u16>(bitRange(_packet.w0, 12, 12));
	tile.ult = static_cast<u16>(bitRange(_packet.w0, 0, 12));
	tile.lrs = static_cast<u16>(bitRange(_packet.w1, 12, 12));
	tile.lrt = static_cast<u16>(bitRange(_packet.w1, 0, 12));
	m_snapshot.changedMask |= tmem_state_changed::kTileSize;
}

void TMEMModel::applyLoadTile(const CommandPacket & _packet)
{
	u8 tileIndex = 0U;
	if (!decodeTileIndex(_packet.w1, tileIndex))
		return;

	TMEMLoadRecord & load = m_snapshot.lastLoad;
	load.sourcePacketId = _packet.id;
	load.kind = TmemLoadKind::kTile;
	load.tile = tileIndex;
	load.uls = static_cast<u16>(bitRange(_packet.w0, 12, 12));
	load.ult = static_cast<u16>(bitRange(_packet.w0, 0, 12));
	load.lrs = static_cast<u16>(bitRange(_packet.w1, 12, 12));
	load.lrt = static_cast<u16>(bitRange(_packet.w1, 0, 12));
	load.dxt = 0U;
	m_snapshot.changedMask |= tmem_state_changed::kLoadTile;
}

void TMEMModel::applyLoadBlock(const CommandPacket & _packet)
{
	u8 tileIndex = 0U;
	if (!decodeTileIndex(_packet.w1, tileIndex))
		return;

	TMEMLoadRecord & load = m_snapshot.lastLoad;
	load.sourcePacketId = _packet.id;
	load.kind = TmemLoadKind::kBlock;
	load.tile = tileIndex;
	load.uls = static_cast<u16>(bitRange(_packet.w0, 12, 12));
	load.ult = static_cast<u16>(bitRange(_packet.w0, 0, 12));
	load.lrs = static_cast<u16>(bitRange(_packet.w1, 12, 12));
	load.lrt = 0U;
	load.dxt = static_cast<u16>(bitRange(_packet.w1, 0, 12));
	m_snapshot.changedMask |= tmem_state_changed::kLoadBlock;
}

void TMEMModel::applyLoadTLUT(const CommandPacket & _packet)
{
	u8 tileIndex = 0U;
	if (!decodeTileIndex(_packet.w1, tileIndex))
		return;

	TMEMLoadRecord & load = m_snapshot.lastLoad;
	load.sourcePacketId = _packet.id;
	load.kind = TmemLoadKind::kTLUT;
	load.tile = tileIndex;
	load.uls = static_cast<u16>(bitRange(_packet.w0, 12, 12));
	load.ult = static_cast<u16>(bitRange(_packet.w0, 0, 12));
	load.lrs = static_cast<u16>(bitRange(_packet.w1, 12, 12));
	load.lrt = static_cast<u16>(bitRange(_packet.w1, 0, 12));
	load.dxt = 0U;
	m_snapshot.changedMask |= tmem_state_changed::kLoadTLUT;
}

} // namespace rvk2
