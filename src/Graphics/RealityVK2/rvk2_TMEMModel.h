#pragma once

#include "rvk2_Types.h"

namespace rvk2 {

class TMEMModel
{
public:
	TMEMModel();

	void reset();
	void apply(const CommandPacket & _packet);

	const TMEMSnapshot & snapshot() const;

private:
	void applySetTextureImage(const CommandPacket & _packet);
	void applySetTile(const CommandPacket & _packet);
	void applySetTileSize(const CommandPacket & _packet);
	void applyLoadTile(const CommandPacket & _packet);
	void applyLoadBlock(const CommandPacket & _packet);
	void applyLoadTLUT(const CommandPacket & _packet);

	TMEMSnapshot m_snapshot;
};

} // namespace rvk2
