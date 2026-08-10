#include "device_lifecycle.h"
#include "platform_time.h"
#include <stdio.h>

typedef struct
{
    LifecycleState from;
    LifecycleState to;
} LifecycleEdge;

static const LifecycleEdge s_edges[] = {
    { LIFECYCLE_POWER_ON,          LIFECYCLE_BOOT },
    { LIFECYCLE_BOOT,              LIFECYCLE_LOAD_CONFIGURATION },
    { LIFECYCLE_LOAD_CONFIGURATION, LIFECYCLE_LOAD_IDENTITY },
    { LIFECYCLE_LOAD_IDENTITY,     LIFECYCLE_CREATE_BOOT_SESSION },
    { LIFECYCLE_CREATE_BOOT_SESSION, LIFECYCLE_SELF_TEST },
    { LIFECYCLE_SELF_TEST,         LIFECYCLE_PROBE_PERIPHERALS },
    { LIFECYCLE_PROBE_PERIPHERALS, LIFECYCLE_INITIALIZE_DRIVERS },
    { LIFECYCLE_INITIALIZE_DRIVERS, LIFECYCLE_READY },
    { LIFECYCLE_READY,             LIFECYCLE_OPERATIONAL },

    /* Post-operational bidirectional transitions */
    { LIFECYCLE_OPERATIONAL,       LIFECYCLE_DEGRADED },
    { LIFECYCLE_DEGRADED,          LIFECYCLE_OPERATIONAL },

    /* Future safe-mode entry/exit */
    { LIFECYCLE_OPERATIONAL,       LIFECYCLE_SAFE_MODE },
    { LIFECYCLE_DEGRADED,          LIFECYCLE_SAFE_MODE },
    { LIFECYCLE_SAFE_MODE,         LIFECYCLE_OPERATIONAL },
    { LIFECYCLE_SAFE_MODE,         LIFECYCLE_DEGRADED },
};

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

bool DeviceLifecycle_CanTransition(LifecycleState from, LifecycleState to)
{
    if (from == to) return true;
    for (size_t i = 0; i < sizeof(s_edges) / sizeof(s_edges[0]); i++)
    {
        if (s_edges[i].from == from && s_edges[i].to == to)
            return true;
    }
    return false;
}

bool DeviceLifecycle_TransitionTo(LifecycleState next)
{
    if (next == s_state) return true;
    if (!DeviceLifecycle_CanTransition(s_state, next))
        return false;  /* illegal — state unchanged */

    LogTransition(s_state, next);
    s_state = next;
    return true;
}

bool DeviceLifecycle_IsOperational(void)
{
    return (s_state == LIFECYCLE_OPERATIONAL || s_state == LIFECYCLE_DEGRADED);
}