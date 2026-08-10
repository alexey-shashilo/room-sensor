#include "device_lifecycle.h"
#include "platform_time.h"
#include <stdio.h>

static LifecycleState s_state = LIFECYCLE_POWER_ON;
static uint32_t s_last_transition_ms = 0;

static const char *StateStrTable[] = {
    "POWER_ON", "BOOT", "PLATFORM_INIT", "LOAD_CONFIG",
    "LOAD_IDENTITY", "BOOT_SESSION", "SELF_TEST", "PROBE",
    "INIT_DRIVERS", "RESTORE", "READY", "OPERATIONAL",
    "DEGRADED", "SAFE_MODE"
};

const char *DeviceLifecycle_StateStr(LifecycleState state)
{
    unsigned idx = (unsigned)state;
    if (idx >= sizeof(StateStrTable) / sizeof(StateStrTable[0]))
        return "?";
    return StateStrTable[idx];
}

static void LogTransition(LifecycleState from, LifecycleState to)
{
    uint32_t now = Platform_GetTickMs();
    printf("LIFECYCLE %s -> %s (%lu ms)\r\n",
           DeviceLifecycle_StateStr(from),
           DeviceLifecycle_StateStr(to),
           (unsigned long)(now - s_last_transition_ms));
    s_last_transition_ms = now;
}

void DeviceLifecycle_Init(LifecycleState initial)
{
    s_state = initial;
    s_last_transition_ms = Platform_GetTickMs();
    printf("LIFECYCLE start=%s\r\n", DeviceLifecycle_StateStr(initial));
}

LifecycleState DeviceLifecycle_GetState(void)
{
    return s_state;
}

bool DeviceLifecycle_IsOperational(void)
{
    return (s_state == LIFECYCLE_OPERATIONAL || s_state == LIFECYCLE_DEGRADED);
}

void DeviceLifecycle_TransitionTo(LifecycleState next)
{
    if (next == s_state) return;
    LogTransition(s_state, next);
    s_state = next;
}