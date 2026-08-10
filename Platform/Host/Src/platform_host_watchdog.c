/* Host Platform — Watchdog */

#include "host_platform.h"
#include "platform_watchdog.h"
#include "platform_time.h"

static bool s_initialized = false;
static int s_refresh_count = 0;
static uint32_t s_last_refresh_ms = 0;

int  HostWdg_GetRefreshCount(void) { return s_refresh_count; }
uint32_t HostWdg_GetLastRefreshMs(void) { return s_last_refresh_ms; }

bool Platform_WatchdogInit(uint32_t timeout_ms)
{
    (void)timeout_ms;
    s_initialized = true;
    return true;
}

void Platform_WatchdogRefresh(void)
{
    if (!s_initialized) return;
    s_refresh_count++;
    s_last_refresh_ms = Platform_GetTickMs();
}