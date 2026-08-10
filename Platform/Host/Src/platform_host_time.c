/* Host Platform — Time implementation (deterministic virtual monotonic tick) */

#include "host_platform.h"
#include "platform_time.h"
#include "platform_delay.h"
#include <stddef.h>

static uint32_t s_tick = 0;

void HostTime_Set(uint32_t ms) { s_tick = ms; }
void HostTime_Advance(uint32_t delta) { s_tick += delta; }
uint32_t HostTime_Get(void) { return s_tick; }

uint32_t Platform_GetTickMs(void)
{
    return s_tick;
}

void Platform_DelayMs(uint32_t ms)
{
    /* Do not sleep real time. Deterministic tests advance virtual tick. */
    s_tick += ms;
}