#ifndef PLATFORM_WATCHDOG_H
#define PLATFORM_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

bool Platform_WatchdogInit(uint32_t timeout_ms);
void Platform_WatchdogRefresh(void);

#endif