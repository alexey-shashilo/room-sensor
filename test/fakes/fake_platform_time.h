#ifndef FAKE_PLATFORM_TIME_H
#define FAKE_PLATFORM_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void FakePlatform_SetTick(uint32_t ms);
void FakePlatform_AdvanceTick(uint32_t delta);
uint32_t FakePlatform_GetTick(void);
uint32_t Platform_GetTickMs(void);
void Platform_DelayMs(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif