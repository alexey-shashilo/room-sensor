#include <stdio.h>
#include <math.h>
#include <string.h>

#include "room_state.h"
#include "config.h"
#include "platform_time.h"

#include "fake_platform_time.h"

static int s_pass = 0;
static int s_fail = 0;
static int s_case = 0;

static void check(int cond, const char *msg)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, msg); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, msg); }
}

int main(void)
{
    printf("RoomState / Config Host Tests\n");

    /* --- RoomState defaults --- */
    printf("\n=== RoomState defaults ===\n");
    {
        RoomState rs;
        RoomState_Init(&rs);
        check(!rs.illuminance_valid, "illuminance_valid = false");
        check(fabsf(rs.illuminance_lux) < 0.001f, "illuminance_lux = 0.0");
        check(rs.timestamp_ms == 0, "timestamp_ms = 0");
    }

    /* --- RoomState_UpdateIlluminance --- */
    printf("\n=== RoomState_UpdateIlluminance ===\n");
    {
        RoomState rs;
        RoomState_Init(&rs);
        FakePlatform_SetTick(1000);

        RoomState_UpdateIlluminance(&rs, 123.4f, true);
        check(rs.illuminance_valid, "valid = true after update");
        check(fabsf(rs.illuminance_lux - 123.4f) < 0.001f, "lux = 123.4");
        check(rs.timestamp_ms == 1000, "timestamp updated to 1000");

        RoomState_UpdateIlluminance(&rs, 0.0f, false);
        check(!rs.illuminance_valid, "valid = false after invalid update");
    }

    /* --- Config defaults --- */
    printf("\n=== Config defaults ===\n");
    {
        Config_LoadDefaults();
        const RoomSensorConfig *cfg = Config_Get();
        check(cfg->storage.light_period_ms == 500U, "light_period_ms = 500");
        check(cfg->storage.display_period_ms == 500U, "display_period_ms = 500");
        check(cfg->storage.retry_period_ms == 5000U, "retry_period_ms = 5000");
        check(cfg->storage.diagnostics_period_ms == 10000U, "diagnostics_period_ms = 10000");
    }

    /* --- Summary --- */
    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);

    return s_fail > 0 ? 1 : 0;
}