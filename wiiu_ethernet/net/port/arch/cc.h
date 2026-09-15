#ifndef AX_LWIP_CC_H
#define AX_LWIP_CC_H
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#undef BYTE_ORDER
#define BYTE_ORDER BIG_ENDIAN
#define LWIP_RAND() ((uint32_t)rand())
#endif
