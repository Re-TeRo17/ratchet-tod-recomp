/* Non-Windows pieces of the port's platform layer (src/platform/tod_platform.h). */
#ifndef _WIN32
#include "platform/tod_platform.h"

/* Installed by the host window layer; NULL = no keyboard (headless). */
extern "C" int (*tod_host_key_down)(int vk) = nullptr;
#endif
