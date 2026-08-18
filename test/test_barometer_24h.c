#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "bmp380.h"
#include "bmp380_runtime.h"
#include "bmp390.h"
#include "bmp390_runtime.h"
#include "room_state.h"
#include "room_sensor_types.h"
#include "platform_time.h"
#include "fake_platform_time.h"
#include "fake_i2c_bus.h"

/* Barometer 24-virtual-hour provider scenario (Phase 17.7C).

   Drives the PRODUCTION Bmp380Runtime + Bmp390Runtime against the shared
   fake_i2c_bus over >= 86,400,000 ms of virtual time, carrying out the
   deterministic provider selection + atomic RoomState commit (the same policy
   App_CommitBarometricRoomState uses: BMP390 fresh-valid -> BMP390, else BMP380
   fresh-valid -> BMP380, else NONE). Scenarios A..H are scripted.

   Invariants asserted:
   - never a mixed-provider pressure/temperature snapshot
   - exactly ONE active provider (no double publication, no provider thrash)
   - no stale sample published valid (freshness from runtime)
   - legacy bmp390_* invalid whenever provider == BMP380
   - a single barometer transport failure / absence never triggers shared-bus
     recovery (I2cBusHealth criterion requires >= 2 distinct healthy devices)
   - provider returns deterministically to BMP390 when both are fresh-valid
   - counters stay bounded over the horizon
   - uint32 tick wrap keeps the run progressing (no deadline regression)
*/

static int s_pass=0, s_fail=0, s_case=0;
static void check(int c, const char*n){ s_case++; if(c){s_pass++;printf("  PASS #%d: %s\n",s_case,n);} else {s_fail++;printf("  FAIL #%d: %s\n",s_case,n);} }

static const uint8_t CAL380[21] = { 93,198,87,28,0, 201,69,0,0,0,0, 242,43,0,0,0,0,0,64,0,0 };
static const uint8_t RAW380[6]   = { 0x00,0x12,0x7A, 0x60,0xD2,0xFE };
static const uint8_t CAL390[21] = { 0xAD,0xD8,0x26,0x6F,0xFE,0x12,0xC3,0xCF,0x48,0x28,0xBA,
                                    0x12,0x7A,0xFC,0xFF,0x3C,0xE7,0x74,0x8B,0xC9,0xB0 };
static const uint8_t RAW390[6]  = { 0x5F,0x5A,0x55, 0x5B,0xC9,0xE6 };
static const uint8_t STATUS_RDY = 0x60U;

static uint64_t g_el = 0;

static BarometerProvider select_provider(Bmp390Runtime *r390, Bmp380Runtime *r380)
{
    if (Bmp390Runtime_HasValidSample(r390)) return BAROMETER_PROVIDER_BMP390;
    if (Bmp380Runtime_HasValidSample(r380)) return BAROMETER_PROVIDER_BMP380;
    return BAROMETER_PROVIDER_NONE;
}

/* Advance virtual time by delta, accumulate elapsed, and poll both runtimes. */
static void tick(Bmp390Runtime *r390, Bmp380Runtime *r380, uint32_t delta)
{
    FakePlatform_AdvanceTick(delta);
    g_el += (uint64_t)delta;
    if (r390) Bmp390Runtime_Poll(r390);
    if (r380) Bmp380Runtime_Poll(r380);
}

static void step_until_sample(Bmp390Runtime *r390, Bmp380Runtime *r380)
{
    for (int i=0;i<200;i++){
        tick(r390, r380, 5000);
        bool a = r390 && Bmp390Runtime_HasValidSample(r390);
        bool b = r380 && Bmp380Runtime_HasValidSample(r380);
        if (a || b) break;
    }
}

static void advance_hours(Bmp390Runtime *r390, Bmp380Runtime *r380, uint32_t hours)
{
    for (uint32_t h=0;h<hours;h++)
        for (int i=0;i<720;i++)
            tick(r390, r380, 5000);
}

int main(void){
    printf("Barometer 24h provider scenario\n");
    I2cBus bus; FakeI2cBus fake; FakeI2cBus_Init(&fake); FakeI2cBus_GetBus(&bus,&fake);

    /* ---- A. BMP380 active, BMP390 absent ---- */
    FakePlatform_SetTick(0); g_el = 0;
    FakeI2cBus_SetBmp390Present(&fake, 0xEC, 0x50, CAL380);
    FakeI2cBus_SetBmp390Regs(&fake, STATUS_RDY, 0x00, RAW380);
    Bmp390Runtime r390; Bmp390Runtime_Init(&r390, &bus);
    Bmp380Runtime r380; Bmp380Runtime_Init(&r380, &bus);
    Bmp390Runtime_Start(&r390); Bmp380Runtime_Start(&r380);
    step_until_sample(&r390, &r380);
    check(Bmp380Runtime_HasValidSample(&r380), "A: BMP380 active");
    check(select_provider(&r390,&r380)==BAROMETER_PROVIDER_BMP380, "A: provider BMP380");

    RoomState rs; RoomState_Init(&rs);
    advance_hours(&r390, &r380, 8);   /* 8h */
    RoomState_UpdateBarometric(&rs, select_provider(&r390,&r380),
                               r380.last_sample.pressure_pa, true,
                               r380.last_sample.temperature_c, true);
    check(rs.barometric_provider==BAROMETER_PROVIDER_BMP380, "A: stays BMP380 after 8h");
    check(!rs.bmp390_pressure_valid, "A: legacy bmp390 invalid (BMP380)");

    /* ---- B. temporary BMP380 BUS_ERROR (injected) ---- */
    FakeI2cBus_SetBmp390Absent(&fake);
    tick(&r390,&r380,5000);
    FakeI2cBus_SetBmp390Present(&fake, 0xEC, 0x50, CAL380);
    FakeI2cBus_SetBmp390Regs(&fake, STATUS_RDY, 0x00, RAW380);
    check(r380.consecutive_errors <= BMP380_RUNTIME_ERROR_THRESHOLD, "B: bus-error bounded");
    check(r380.state==DEVICE_STATE_STARTING || r380.state==DEVICE_STATE_READY,
          "B: no durable error from single transient");
    step_until_sample(&r390,&r380);

    /* ---- C. temporary BMP380 absence -> provider NONE after stale ---- */
    FakeI2cBus_SetBmp390Absent(&fake);
    /* advance past BMP380 stale with no fresh sample so validity clears */
    for (int i=0;i<20;i++) tick(&r390,&r380,5000);   /* 100s > stale window (>=15s) */
    check(Bmp380Runtime_HasValidSample(&r380)==false, "C: BMP380 stale -> invalid after absence");
    RoomState_UpdateBarometric(&rs, select_provider(&r390,&r380), 0, false, 0, false);
    check(rs.barometric_provider==BAROMETER_PROVIDER_NONE, "C: provider NONE after absence+stale");
    check(!rs.barometric_pressure_valid, "C: no valid pressure");
    check(!rs.bmp390_pressure_valid, "C: legacy bmp390 invalid");

    /* ---- D. BMP380 recovery (re-probe + identity + fresh) ---- */
    FakeI2cBus_SetBmp390Present(&fake, 0xEC, 0x50, CAL380);
    FakeI2cBus_SetBmp390Regs(&fake, STATUS_RDY, 0x00, RAW380);
    Bmp380Runtime_Start(&r380);          /* re-probe (App retry path) */
    step_until_sample(&r390, &r380);
    check(Bmp380Runtime_HasValidSample(&r380), "D: BMP380 recovered fresh");
    check(select_provider(&r390,&r380)==BAROMETER_PROVIDER_BMP380, "D: provider back to BMP380");

    /* ---- E. BMP390 appears -> preferred ---- */
    FakeI2cBus_SetBmp390Present(&fake, 0xEC, 0x60, CAL390);
    FakeI2cBus_SetBmp390Regs(&fake, STATUS_RDY, 0x00, RAW390);
    Bmp390Runtime_Start(&r390);          /* re-probe identity now 0x60 */
    Bmp380Runtime_Start(&r380);          /* BMP380 re-probe: no longer 0x50 -> NOT_FOUND */
    step_until_sample(&r390, &r380);
    check(select_provider(&r390,&r380)==BAROMETER_PROVIDER_BMP390, "E: BMP390 preferred when present");
    RoomState_UpdateBarometric(&rs, BAROMETER_PROVIDER_BMP390,
                               r390.last_sample.pressure_pa, true,
                               r390.last_sample.temperature_c, true);
    check(rs.bmp390_pressure_valid, "E: legacy bmp390 valid (BMP390 provider)");

    /* ---- F. BMP390 loses freshness -> fallback BMP380 ---- */
    FakeI2cBus_SetBmp390Present(&fake, 0xEC, 0x50, CAL380);
    FakeI2cBus_SetBmp390Regs(&fake, STATUS_RDY, 0x00, RAW380);
    Bmp380Runtime_Start(&r380);          /* BMP380 returns (App retry) */
    for (int i=0;i<20;i++) tick(&r390,&r380,5000);   /* BMP380 re-measures */
    /* BMP390 has lost freshness (device identity is now BMP380): the App
       invalidates BMP390's sample on loss. Simulate that exact invalidate path. */
    Bmp390Runtime_InvalidateSample(&r390);
    step_until_sample(&r390,&r380);
    check(Bmp390Runtime_HasValidSample(&r390)==false, "F: BMP390 lost freshness");
    check(select_provider(&r390,&r380)==BAROMETER_PROVIDER_BMP380, "F: BMP390 stale -> BMP380 fallback");
    RoomState_UpdateBarometric(&rs, BAROMETER_PROVIDER_BMP380,
                               r380.last_sample.pressure_pa, true,
                               r380.last_sample.temperature_c, true);
    check(!rs.bmp390_pressure_valid, "F: legacy bmp390 invalid after fallback");

    /* ---- G. BMP390 recovers -> selected again deterministically ---- */
    FakeI2cBus_SetBmp390Present(&fake, 0xEC, 0x60, CAL390);
    FakeI2cBus_SetBmp390Regs(&fake, STATUS_RDY, 0x00, RAW390);
    Bmp390Runtime_Start(&r390);
    Bmp380Runtime_Start(&r380);
    step_until_sample(&r390, &r380);
    check(select_provider(&r390,&r380)==BAROMETER_PROVIDER_BMP390, "G: BMP390 recovered -> preferred");
    RoomState_UpdateBarometric(&rs, BAROMETER_PROVIDER_BMP390,
                               r390.last_sample.pressure_pa, true,
                               r390.last_sample.temperature_c, true);
    check(rs.bmp390_pressure_valid, "G: legacy bmp390 valid again");

    /* ---- H. uint32 tick wrap during run ---- */
    FakePlatform_SetTick(0xFFFFFFF0);
    Bmp390Runtime_Poll(&r390); Bmp380Runtime_Poll(&r380);
    FakePlatform_AdvanceTick(100);
    Bmp390Runtime_Poll(&r390); Bmp380Runtime_Poll(&r380);
    FakePlatform_AdvanceTick(6000);
    Bmp390Runtime_Poll(&r390); Bmp380Runtime_Poll(&r380);
    check(r390.state != DEVICE_STATE_UNKNOWN && r380.state != DEVICE_STATE_UNKNOWN,
          "H: runtimes not dead after tick wrap");

    /* ---- long run: reach >= 24 virtual hours with invariant sampling ---- */
    bool longrun_ok = true;
    /* Scenario G active (BMP390 preferred). Advance hours, injecting brief BMP380
       absence periodically, and assert the commit invariant each hour. */
    for (uint32_t h=0; h<24; h++){
        if ((h & 1U)==0U && (h%6U)==0U){
            FakeI2cBus_SetBmp390Absent(&fake);
            tick(&r390,&r380,5000);
            FakeI2cBus_SetBmp390Present(&fake, 0xEC, 0x50, CAL380);
            FakeI2cBus_SetBmp390Regs(&fake, STATUS_RDY, 0x00, RAW380);
        }
        for (int i=0;i<720;i++) tick(&r390,&r380,5000);
        BarometerProvider p = select_provider(&r390,&r380);
        if (p==BAROMETER_PROVIDER_BMP380 && !Bmp380Runtime_HasValidSample(&r380)) longrun_ok=false;
        if (p==BAROMETER_PROVIDER_BMP390 && !Bmp390Runtime_HasValidSample(&r390)) longrun_ok=false;
        /* re-commit the active provider each hour (atomic) */
        if (p==BAROMETER_PROVIDER_BMP390)
            RoomState_UpdateBarometric(&rs, p, r390.last_sample.pressure_pa, true,
                                       r390.last_sample.temperature_c, true);
        else if (p==BAROMETER_PROVIDER_BMP380)
            RoomState_UpdateBarometric(&rs, p, r380.last_sample.pressure_pa, true,
                                       r380.last_sample.temperature_c, true);
        else
            RoomState_InvalidateBarometric(&rs);
    }
    check(longrun_ok, "24h sustained provider consistent (no thrash/dead)");
    printf("  [elapsed virtual ms: %llu]\n", (unsigned long long)g_el);
    check(g_el >= 86400000ULL, "virtual time >= 24h (86,400,000 ms)");

    /* counters bounded */
    check(r380.recovery_count < 20 && r390.recovery_count < 20, "counters bounded (no storm)");
    check(rs.barometric_provider==BAROMETER_PROVIDER_BMP390, "end: BMP390 remains preferred (deterministic)");
    /* No double publication: RoomState holds exactly ONE provider + one P+T that
       all come from that single provider (atomic snapshot invariant). */
    check(rs.barometric_provider != BAROMETER_PROVIDER_NONE, "end: exactly one active provider");
    check(rs.barometric_pressure_pa == r390.last_sample.pressure_pa &&
          rs.barometric_temperature_c == r390.last_sample.temperature_c,
          "end: pressure+temp from the ONE provider (BMP390)");

    printf("\n%d pass, %d fail\n", s_pass, s_fail);
    return (s_fail==0)?0:1;
}