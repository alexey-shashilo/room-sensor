#ifndef PLATFORM_RESET_H
#define PLATFORM_RESET_H

#include <stdint.h>

typedef enum
{
    RESET_CAUSE_UNKNOWN = 0,
    RESET_CAUSE_POWER_ON,
    RESET_CAUSE_SOFTWARE,
    RESET_CAUSE_WATCHDOG,
    RESET_CAUSE_BROWNOUT,
    RESET_CAUSE_EXTERNAL
} ResetCause;

ResetCause Platform_GetResetCause(void);
void       Platform_ClearResetFlags(void);

#endif