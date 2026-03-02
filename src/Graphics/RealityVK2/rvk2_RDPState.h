#pragma once

#include "rvk2_Types.h"

namespace rvk2 {

class RDPStateEngine
{
public:
	RDPStateEngine();

	void reset();
	void apply(const CommandPacket & _packet);

	const RDPStateSnapshot & snapshot() const;

private:
	void applySetOtherModes(const CommandPacket & _packet);
	void applySetCombineMode(const CommandPacket & _packet);
	void applySetScissor(const CommandPacket & _packet);
	void applySetColorImage(const CommandPacket & _packet);
	void applySetDepthImage(const CommandPacket & _packet);
	void applySetPrimColor(const CommandPacket & _packet);
	void applySetEnvColor(const CommandPacket & _packet);
	void applySetBlendColor(const CommandPacket & _packet);
	void applySetFogColor(const CommandPacket & _packet);
	void applySetFillColor(const CommandPacket & _packet);
	void applySetPrimDepth(const CommandPacket & _packet);
	void applySetConvert(const CommandPacket & _packet);
	void applySetKeyR(const CommandPacket & _packet);
	void applySetKeyGB(const CommandPacket & _packet);
	void applyLoadSync(const CommandPacket & _packet);
	void applyPipeSync(const CommandPacket & _packet);
	void applyTileSync(const CommandPacket & _packet);
	void applyFullSync(const CommandPacket & _packet);

	RDPStateSnapshot m_snapshot;
};

} // namespace rvk2
