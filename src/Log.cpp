#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "Log.h"
#include <sstream>
#include <vector>
#include "mupenplus/RealityVK_mupenplus.h"

void LogDebug(const char* _fileName, int _line, u16 _type, const char* _format, ...)
{
	static const int logLevel[] = {
		M64MSG_INFO,
		M64MSG_ERROR,
		M64MSG_INFO,
		M64MSG_WARNING,
		M64MSG_VERBOSE,
		M64MSG_VERBOSE
	};

	if (CoreDebugCallback == nullptr ||
		_type > LOG_LEVEL)
	{
		return;
	}

	// initialize use of the variable argument array
	va_list vaArgs;
	va_start(vaArgs, _format);

	// reliably acquire the size from a copy of
	// the variable argument array
	// and a functionally reliable call
	// to mock the formatting
	va_list vaCopy;
	va_copy(vaCopy, vaArgs);
	const int iLen = std::vsnprintf(NULL, 0, _format, vaCopy);
	va_end(vaCopy);

	// return a formatted string without
	// risking memory mismanagement
	// and without assuming any compiler
	// or platform specific behavior
	std::vector<char> zc(iLen + 1);
	std::vsnprintf(zc.data(), zc.size(), _format, vaArgs);
	va_end(vaArgs);

	std::stringstream formatString;
	formatString << _fileName << ":" << _line << ", \"" << zc.data() << "\"";

	CoreDebugCallback(CoreDebugCallbackContext, logLevel[_type], formatString.str().c_str());
}

#if defined(OS_WINDOWS) && !defined(MINGW)
#include <windows.h>
void debugPrint(const char * format, ...) {
	char text[256];
	wchar_t wtext[256];
	va_list va;
	va_start(va, format);
	vsprintf(text, format, va);
	mbstowcs(wtext, text, 256);
	OutputDebugStringW(wtext);
	va_end(va);
}
#endif
