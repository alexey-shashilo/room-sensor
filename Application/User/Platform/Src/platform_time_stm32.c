#include "platform_time.h"

void Platform_DelayMs(uint32_t ms)
{
    extern void HAL_Delay(uint32_t Delay);
    HAL_Delay(ms);
}

uint32_t Platform_GetTickMs(void)
{
    extern uint32_t HAL_GetTick(void);
    return HAL_GetTick();
}