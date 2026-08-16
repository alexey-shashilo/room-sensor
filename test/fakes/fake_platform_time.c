#include "fake_platform_time.h"

static uint32_t s_fake_tick = 0;

void FakePlatform_SetTick(uint32_t ms)
{
    s_fake_tick = ms;
}

void FakePlatform_AdvanceTick(uint32_t delta)
{
    s_fake_tick += delta;
}

uint32_t FakePlatform_GetTick(void)
{
    return s_fake_tick;
}

uint32_t Platform_GetTickMs(void)
{
    return s_fake_tick;
}

void Platform_DelayMs(uint32_t ms)
{
    s_fake_tick += ms;
}