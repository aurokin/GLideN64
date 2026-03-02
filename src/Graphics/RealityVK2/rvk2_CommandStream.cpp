#include "rvk2_CommandStream.h"

#include <cstddef>

namespace {

constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;

inline void updateHashBytes(u64 & _hash, const void * _data, size_t _size)
{
	const u8 * bytes = static_cast<const u8 *>(_data);
	for (size_t i = 0; i < _size; ++i) {
		_hash ^= static_cast<u64>(bytes[i]);
		_hash *= kFnvPrime;
	}
}

template <typename T>
inline void updateHash(u64 & _hash, const T & _value)
{
	updateHashBytes(_hash, &_value, sizeof(T));
}

} // namespace

namespace rvk2 {

CommandStream::CommandStream()
	: m_commands()
	, m_frameId(0ULL)
	, m_commandHash(kFnvOffset)
{
}

void CommandStream::reset(u64 _frameId)
{
	m_commands.clear();
	m_frameId = _frameId;
	m_commandHash = kFnvOffset;
}

void CommandStream::push(const CommandPacket & _packet)
{
	updateHash(m_commandHash, _packet.id);
	updateHash(m_commandHash, _packet.domain);
	updateHash(m_commandHash, _packet.opcode);
	updateHash(m_commandHash, _packet.flags);
	updateHash(m_commandHash, _packet.w0);
	updateHash(m_commandHash, _packet.w1);
	if (_packet.extraWordCount > 0U) {
		updateHash(m_commandHash, _packet.extraWordCount);
		updateHash(m_commandHash, _packet.w2);
		if (_packet.extraWordCount > 1U) {
			updateHash(m_commandHash, _packet.w3);
			if (_packet.extraWordCount > 2U) {
				updateHash(m_commandHash, _packet.w4);
				if (_packet.extraWordCount > 3U) {
					updateHash(m_commandHash, _packet.w5);
					if (_packet.extraWordCount > 4U) {
						updateHash(m_commandHash, _packet.w6);
						if (_packet.extraWordCount > 5U)
							updateHash(m_commandHash, _packet.w7);
					}
				}
			}
		}
	}
	if (_packet.fullWordCount > 0U) {
		updateHash(m_commandHash, _packet.fullWordCount);
		updateHash(m_commandHash, _packet.tailHash);
	}
	updateHash(m_commandHash, _packet.provenance.taskId);
	updateHash(m_commandHash, _packet.provenance.dlistAddress);
	updateHash(m_commandHash, _packet.provenance.microcode);
	m_commands.push_back(_packet);
}

const std::vector<CommandPacket> & CommandStream::commands() const
{
	return m_commands;
}

u64 CommandStream::frameId() const
{
	return m_frameId;
}

u64 CommandStream::commandHash() const
{
	return m_commandHash;
}

} // namespace rvk2
