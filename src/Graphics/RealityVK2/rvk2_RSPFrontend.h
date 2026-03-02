#pragma once

#include "rvk2_Types.h"

namespace rvk2 {

class RSPFrontend
{
public:
	RSPFrontend();

	void reset();
	CommandPacket ingestRDPCommand(
		u32 _dlistAddress,
		u32 _w0,
		u32 _w1,
		const CommandProvenance & _provenance,
		u8 _extraWordCount = 0U,
		u32 _w2 = 0U,
		u32 _w3 = 0U,
		u32 _w4 = 0U,
		u32 _w5 = 0U,
		u32 _w6 = 0U,
		u32 _w7 = 0U,
		u16 _fullWordCount = 0U,
		u64 _tailHash = 1469598103934665603ULL);
	CommandPacket ingestRSPCommand(
		u32 _dlistAddress,
		u32 _w0,
		u32 _w1,
		const CommandProvenance & _provenance,
		u8 _extraWordCount = 0U,
		u32 _w2 = 0U,
		u32 _w3 = 0U,
		u32 _w4 = 0U,
		u32 _w5 = 0U,
		u32 _w6 = 0U,
		u32 _w7 = 0U,
		u16 _fullWordCount = 0U,
		u64 _tailHash = 1469598103934665603ULL);

private:
	PacketId m_nextPacketId;
};

} // namespace rvk2
