#include "rvk2_RSPFrontend.h"

namespace rvk2 {

RSPFrontend::RSPFrontend()
	: m_nextPacketId(1ULL)
{
}

void RSPFrontend::reset()
{
	m_nextPacketId = 1ULL;
}

CommandPacket RSPFrontend::ingestRDPCommand(
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
	u64 _tailHash)
{
	CommandPacket packet{};
	packet.id = m_nextPacketId++;
	packet.domain = CommandDomain::kRDP;
	packet.opcode = static_cast<u8>((_w0 >> 24) & 0xFFU);
	packet.extraWordCount = _extraWordCount;
	packet.fullWordCount = _fullWordCount;
	packet.w0 = _w0;
	packet.w1 = _w1;
	packet.w2 = _w2;
	packet.w3 = _w3;
	packet.w4 = _w4;
	packet.w5 = _w5;
	packet.w6 = _w6;
	packet.w7 = _w7;
	packet.tailHash = _tailHash;
	packet.provenance = _provenance;
	packet.provenance.dlistAddress = _dlistAddress;
	return packet;
}

CommandPacket RSPFrontend::ingestRSPCommand(
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
	u64 _tailHash)
{
	CommandPacket packet{};
	packet.id = m_nextPacketId++;
	packet.domain = CommandDomain::kRSP;
	packet.opcode = static_cast<u8>((_w0 >> 24) & 0xFFU);
	packet.extraWordCount = _extraWordCount;
	packet.fullWordCount = _fullWordCount;
	packet.w0 = _w0;
	packet.w1 = _w1;
	packet.w2 = _w2;
	packet.w3 = _w3;
	packet.w4 = _w4;
	packet.w5 = _w5;
	packet.w6 = _w6;
	packet.w7 = _w7;
	packet.tailHash = _tailHash;
	packet.provenance = _provenance;
	packet.provenance.dlistAddress = _dlistAddress;
	return packet;
}

} // namespace rvk2
