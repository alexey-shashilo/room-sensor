#include "platform_watchdog.h"
#include "stm32g4xx_hal.h"

static bool s_watchdog_initialized = false;

bool Platform_WatchdogInit(uint32_t timeout_ms)
{
    if (timeout_ms == 0) return false;

    uint32_t reload = (uint32_t)((timeout_ms * 32000U) / (64U * 1000U));
    if (reload > 0xFFF) reload = 0xFFF;
    if (reload < 1) return false;

    IWDG->KR = 0x5555U;
    WRITE_REG(IWDG->PR, 4U);          /* /64 — IWDG_PRESCALER_64 = 4 */
    WRITE_REG(IWDG->RLR, reload);
    IWDG->KR = 0xCCCCU;

    s_watchdog_initialized = true;
    return true;
}

void Platform_WatchdogRefresh(void)
{
    if (s_watchdog_initialized)
        IWDG->KR = 0xAAAAU;
}