#ifndef AX_LWIP_CC_H
#define AX_LWIP_CC_H
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
/* Route lwIP errno through newlib's per-thread errno instead of lwIP's
 * own global. */
#define LWIP_ERRNO_STDINCLUDE
#undef BYTE_ORDER
#define BYTE_ORDER BIG_ENDIAN
#define LWIP_RAND() ((uint32_t)rand())
#endif
