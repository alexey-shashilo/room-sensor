#include "platform_reset.h"
#include "stm32g4xx_hal.h"

ResetCause Platform_GetResetCause(void)
{
    uint32_t flags = RCC->CSR;

    if (flags & RCC_CSR_SFTRSTF)  return RESET_CAUSE_SOFTWARE;
    if (flags & RCC_CSR_IWDGRSTF) return RESET_CAUSE_WATCHDOG;
    if (flags & RCC_CSR_WWDGRSTF) return RESET_CAUSE_WATCHDOG;
    if (flags & RCC_CSR_BORRSTF)  return RESET_CAUSE_BROWNOUT;
    if (flags & RCC_CSR_PINRSTF)  return RESET_CAUSE_EXTERNAL;
    if (flags & RCC_CSR_LPWRRSTF) return RESET_CAUSE_BROWNOUT;

    return RESET_CAUSE_POWER_ON;
}

void Platform_ClearResetFlags(void)
{
    __HAL_RCC_CLEAR_RESET_FLAGS();
}