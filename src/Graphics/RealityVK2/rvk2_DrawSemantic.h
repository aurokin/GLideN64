#pragma once

#include "rvk2_Types.h"

namespace rvk2 {

bool isDrawOpcode(u8 _opcode);

DrawSemanticPacket buildDrawSemanticPacket(
	const CommandPacket & _packet,
	const RDPStateSnapshot & _rdpState,
	const TMEMSnapshot & _tmemState);

} // namespace rvk2
