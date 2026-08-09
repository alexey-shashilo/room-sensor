#ifndef PLATFORM_TIME_H
#define PLATFORM_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t Platform_GetTickMs(void);

void Platform_DelayMs(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif