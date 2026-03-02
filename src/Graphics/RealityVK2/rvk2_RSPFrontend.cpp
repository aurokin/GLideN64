#include "rvk2_RSPFrontend.h"

#include <algorithm>

namespace {

inline u8 clampPayloadWordCount(u32 _count)
{
	return static_cast<u8>(std::min<u32>(_count, rvk2::kMaxCommandPayloadWords));
}

void populatePayload(
	rvk2::CommandPacket & _packet,
	u8 _extraWordCount,
	u32 _w2,
	u32 _w3,
	u32 _w4,
	u32 _w5,
	u32 _w6,
	u32 _w7,
	u8 _payloadWordCount,
	const u32 * _payloadWords)
{
	_packet.extraWordCount = _extraWordCount;
	_packet.w2 = _w2;
	_packet.w3 = _w3;
	_packet.w4 = _w4;
	_packet.w5 = _w5;
	_packet.w6 = _w6;
	_packet.w7 = _w7;

	if (_payloadWords != nullptr && _payloadWordCount > 0U) {
		const u8 count = clampPayloadWordCount(_payloadWordCount);
		_packet.payloadWordCount = count;
		for (u32 i = 0U; i < count; ++i)
			_packet.payloadWords[i] = _payloadWords[i];
	}
	else {
		const u8 count = static_cast<u8>(std::min<u32>(_extraWordCount, 6U));
		_packet.payloadWordCount = count;
		if (count > 0U)
			_packet.payloadWords[0] = _w2;
		if (count > 1U)
			_packet.payloadWords[1] = _w3;
		if (count > 2U)
			_packet.payloadWords[2] = _w4;
		if (count > 3U)
			_packet.payloadWords[3] = _w5;
		if (count > 4U)
			_packet.payloadWords[4] = _w6;
		if (count > 5U)
			_packet.payloadWords[5] = _w7;
	}

	if (_packet.payloadWordCount > 0U)
		_packet.w2 = _packet.payloadWords[0];
	if (_packet.payloadWordCount > 1U)
		_packet.w3 = _packet.payloadWords[1];
	if (_packet.payloadWordCount > 2U)
		_packet.w4 = _packet.payloadWords[2];
	if (_packet.payloadWordCount > 3U)
		_packet.w5 = _packet.payloadWords[3];
	if (_packet.payloadWordCount > 4U)
		_packet.w6 = _packet.payloadWords[4];
	if (_packet.payloadWordCount > 5U)
		_packet.w7 = _packet.payloadWords[5];
}

} // namespace

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
	u64 _tailHash,
	u8 _payloadWordCount,
	const u32 * _payloadWords)
{
	CommandPacket packet{};
	packet.id = m_nextPacketId++;
	packet.domain = CommandDomain::kRDP;
	packet.opcode = static_cast<u8>((_w0 >> 24) & 0xFFU);
	packet.fullWordCount = _fullWordCount;
	packet.w0 = _w0;
	packet.w1 = _w1;
	populatePayload(
		packet,
		_extraWordCount,
		_w2,
		_w3,
		_w4,
		_w5,
		_w6,
		_w7,
		_payloadWordCount,
		_payloadWords);
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
	u64 _tailHash,
	u8 _payloadWordCount,
	const u32 * _payloadWords)
{
	CommandPacket packet{};
	packet.id = m_nextPacketId++;
	packet.domain = CommandDomain::kRSP;
	packet.opcode = static_cast<u8>((_w0 >> 24) & 0xFFU);
	packet.fullWordCount = _fullWordCount;
	packet.w0 = _w0;
	packet.w1 = _w1;
	populatePayload(
		packet,
		_extraWordCount,
		_w2,
		_w3,
		_w4,
		_w5,
		_w6,
		_w7,
		_payloadWordCount,
		_payloadWords);
	packet.tailHash = _tailHash;
	packet.provenance = _provenance;
	packet.provenance.dlistAddress = _dlistAddress;
	return packet;
}

} // namespace rvk2
