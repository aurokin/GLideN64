#pragma once

#include <vector>

#include "rvk2_Types.h"

namespace rvk2 {

class CommandStream
{
public:
	CommandStream();

	void reset(u64 _frameId);
	void push(const CommandPacket & _packet);

	const std::vector<CommandPacket> & commands() const;
	u64 frameId() const;
	u64 commandHash() const;

private:
	std::vector<CommandPacket> m_commands;
	u64 m_frameId;
	u64 m_commandHash;
};

} // namespace rvk2
