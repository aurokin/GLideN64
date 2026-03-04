#pragma once

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace rvk2 {

inline bool parseEnvBoolValue(const char * _value, bool & _out)
{
	if (_value == nullptr || _value[0] == '\0')
		return false;

	std::string token(_value);
	size_t begin = 0U;
	while (begin < token.size()
		&& (token[begin] == ' '
			|| token[begin] == '\t'
			|| token[begin] == '\r'
			|| token[begin] == '\n')) {
		++begin;
	}
	size_t end = token.size();
	while (end > begin
		&& (token[end - 1U] == ' '
			|| token[end - 1U] == '\t'
			|| token[end - 1U] == '\r'
			|| token[end - 1U] == '\n')) {
		--end;
	}
	if (begin >= end)
		return false;

	token = token.substr(begin, end - begin);
	for (char & c : token)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

	if (token == "1" || token == "true" || token == "yes" || token == "on"
		|| token == "y" || token == "t") {
		_out = true;
		return true;
	}
	if (token == "0" || token == "false" || token == "no" || token == "off"
		|| token == "n" || token == "f") {
		_out = false;
		return true;
	}
	return false;
}

inline bool envFlagEnabled(const char * _key, bool _defaultValue)
{
	const char * raw = std::getenv(_key);
	if (raw == nullptr || raw[0] == '\0')
		return _defaultValue;

	bool parsed = _defaultValue;
	return parseEnvBoolValue(raw, parsed) ? parsed : _defaultValue;
}

inline const char * envStringOrNull(const char * _key)
{
	const char * raw = std::getenv(_key);
	if (raw == nullptr || raw[0] == '\0')
		return nullptr;
	return raw;
}

inline bool parseEnvUnsignedValue(const char * _value, uint64_t & _out)
{
	if (_value == nullptr || _value[0] == '\0')
		return false;
	char * end = nullptr;
	errno = 0;
	const unsigned long long value = std::strtoull(_value, &end, 0);
	if (errno != 0 || end == nullptr || *end != '\0')
		return false;
	_out = static_cast<uint64_t>(value);
	return true;
}

inline bool envUnsigned(const char * _key, uint64_t & _out)
{
	return parseEnvUnsignedValue(std::getenv(_key), _out);
}

inline uint32_t envU32Clamped(
	const char * _key,
	uint32_t _defaultValue,
	uint32_t _maxValue,
	int _base = 10)
{
	const char * raw = std::getenv(_key);
	if (raw == nullptr || raw[0] == '\0')
		return _defaultValue;
	char * end = nullptr;
	errno = 0;
	const unsigned long long parsed = std::strtoull(raw, &end, _base);
	if (errno != 0 || end == raw || end == nullptr || *end != '\0')
		return _defaultValue;
	const uint64_t clamped = static_cast<uint64_t>(parsed) > static_cast<uint64_t>(_maxValue)
		? static_cast<uint64_t>(_maxValue)
		: static_cast<uint64_t>(parsed);
	return static_cast<uint32_t>(clamped);
}

} // namespace rvk2
