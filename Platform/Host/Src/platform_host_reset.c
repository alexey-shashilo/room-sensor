/* Host Platform — Reset cause */

#include "host_platform.h"
#include "platform_reset.h"

static bool s_reset_requested = false;

bool HostReset_WasRequested(void) { return s_reset_requested; }
bool HostReset_Clear(void) { bool r = s_reset_requested; s_reset_requested = false; return r; }

ResetCause Platform_GetResetCause(void)
{
    return RESET_CAUSE_POWER_ON;
}

void Platform_ClearResetFlags(void)
{
    s_reset_requested = false;
}