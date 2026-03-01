#include "Log.h"
#include <Foundation/Foundation.h>

void LOG(u16 type, const char * format, ...) {
	if (type > LOG_LEVEL)
		return;

	va_list va;
	va_start(va, format);
	NSString *nsformat = [NSString stringWithFormat:@"RealityVK: %s", format];
	NSLogv(nsformat, va);
	va_end(va);
}
