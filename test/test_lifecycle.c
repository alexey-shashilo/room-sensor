#include <stdio.h>
#include <string.h>

#include "room_sensor_types.h"
#include "device_lifecycle.h"
#include "fake_platform_time.h"

static int s_pass = 0, s_fail = 0, s_case = 0;

static void check(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

int main(void)
{
    printf("Device Lifecycle Host Tests\n");

    FakePlatform_SetTick(0);

    /* 1. Normal boot progression */
    printf("\n=== Normal boot ===\n");
    DeviceLifecycle_Init(LIFECYCLE_BOOT);
    check(DeviceLifecycle_GetState() == LIFECYCLE_BOOT, "init -> BOOT");
    check(!DeviceLifecycle_IsOperational(), "BOOT not operational");

    DeviceLifecycle_TransitionTo(LIFECYCLE_LOAD_CONFIGURATION);
    DeviceLifecycle_TransitionTo(LIFECYCLE_LOAD_IDENTITY);
    DeviceLifecycle_TransitionTo(LIFECYCLE_CREATE_BOOT_SESSION);
    DeviceLifecycle_TransitionTo(LIFECYCLE_SELF_TEST);
    DeviceLifecycle_TransitionTo(LIFECYCLE_PROBE_PERIPHERALS);
    DeviceLifecycle_TransitionTo(LIFECYCLE_INITIALIZE_DRIVERS);
    DeviceLifecycle_TransitionTo(LIFECYCLE_READY);
    DeviceLifecycle_TransitionTo(LIFECYCLE_OPERATIONAL);
    check(DeviceLifecycle_GetState() == LIFECYCLE_OPERATIONAL, "reached OPERATIONAL");
    check(DeviceLifecycle_IsOperational(), "OPERATIONAL is operational");

    /* 2. Self-transition is no-op */
    printf("\n=== Self-transition ===\n");
    DeviceLifecycle_TransitionTo(LIFECYCLE_OPERATIONAL);
    check(DeviceLifecycle_GetState() == LIFECYCLE_OPERATIONAL, "self-transition keeps state");

    /* 3. Degraded is operational */
    printf("\n=== Degraded ===\n");
    DeviceLifecycle_TransitionTo(LIFECYCLE_DEGRADED);
    check(DeviceLifecycle_IsOperational(), "DEGRADED still operational");
    check(DeviceLifecycle_GetState() == LIFECYCLE_DEGRADED, "state = DEGRADED");

    /* 4. Safe mode not operational */
    printf("\n=== Safe mode ===\n");
    DeviceLifecycle_TransitionTo(LIFECYCLE_SAFE_MODE);
    check(!DeviceLifecycle_IsOperational(), "SAFE_MODE not operational");

    /* 5. State strings */
    printf("\n=== State strings ===\n");
    check(strcmp(DeviceLifecycle_StateStr(LIFECYCLE_POWER_ON), "POWER_ON") == 0, "POWER_ON str");
    check(strcmp(DeviceLifecycle_StateStr(LIFECYCLE_OPERATIONAL), "OPERATIONAL") == 0, "OPERATIONAL str");
    check(strcmp(DeviceLifecycle_StateStr(LIFECYCLE_DEGRADED), "DEGRADED") == 0, "DEGRADED str");

    /* 6. Recovery: DEGRADED -> OPERATIONAL */
    printf("\n=== Recovery ===\n");
    DeviceLifecycle_TransitionTo(LIFECYCLE_OPERATIONAL);
    check(DeviceLifecycle_GetState() == LIFECYCLE_OPERATIONAL, "recovered to OPERATIONAL");

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}