#ifndef AX_LWIP_CC_H
#define AX_LWIP_CC_H
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <whb/log.h>
/* Route lwIP diagnostics/asserts to the UDP log so a failing assert names
 * itself on the PC instead of just aborting the title. */
#define LWIP_PLATFORM_DIAG(x)  do { WHBLogPrintf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { \
    WHBLogPrintf("lwIP ASSERT: %s at %s:%d", x, __FILE__, __LINE__); \
    abort(); \
} while (0)
/* Route lwIP errno through newlib's per-thread errno instead of lwIP's
 * own global. */
#define LWIP_ERRNO_STDINCLUDE
#undef BYTE_ORDER
#define BYTE_ORDER BIG_ENDIAN
#define LWIP_RAND() ((uint32_t)rand())
#endif
