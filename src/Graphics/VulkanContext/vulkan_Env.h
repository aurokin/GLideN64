#pragma once

#include <cctype>
#include <cstdlib>
#include <string>

namespace vulkan {
namespace env {

inline bool parseBoolValue(const char * _value, bool & _out)
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

inline const char * stringOrNull(const char * _key)
{
	const char * raw = std::getenv(_key);
	if (raw == nullptr || raw[0] == '\0')
		return nullptr;
	return raw;
}

inline bool flagEnabled(const char * _key, bool _defaultValue = false)
{
	const char * raw = stringOrNull(_key);
	if (raw == nullptr)
		return _defaultValue;
	bool parsed = _defaultValue;
	return parseBoolValue(raw, parsed) ? parsed : _defaultValue;
}

} // namespace env
} // namespace vulkan
