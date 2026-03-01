#include <Revision.h>
char pluginName[] = "RealityVK";
#ifdef PLUGIN_REVISION
char pluginNameWithRevision[] = "RealityVK rev." PLUGIN_REVISION;
#else // PLUGIN_REVISION
char pluginNameWithRevision[] = "RealityVK";
#endif // PLUGIN_REVISION
wchar_t pluginNameW[] = L"RealityVK";
void (*CheckInterrupts)( void );
