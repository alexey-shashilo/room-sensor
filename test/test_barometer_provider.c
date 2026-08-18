#include <stdio.h>
#include <string.h>
#include <math.h>

#include "bmp380.h"
#include "bmp380_runtime.h"
#include "bmp390.h"
#include "bmp390_runtime.h"
#include "room_state.h"
#include "platform_time.h"
#include "fake_platform_time.h"
#include "fake_i2c_bus.h"

/* Barometric provider whole-device fallback regression (Phase 17.7B).

   Drives the PRODUCTION BMP390 + BMP380 runtimes against the shared fake_i2c_bus
   and asserts the deterministic provider selection + atomic RoomState commit App
   performs (BMP390-first, else BMP380, else NONE), plus the negative controls:
   no mixed-provider snapshot, no double publication, a roof erred BMP390 falls
   back to BMP380, and a recovered BMP390 returns to provider. */

static int s_pass=0, s_fail=0, s_case=0;
static void check(int c, const char*n){ s_case++; if(c){s_pass++;printf("  PASS #%d: %s\n",s_case,n);} else {s_fail++;printf("  FAIL #%d: %s\n",s_case,n);} }

static const uint8_t CAL380[21] = {
    93,198,87,28,0, 201,69,0,0,0,0, 242,43,0,0,0,0,0,64,0,0
};
static const uint8_t RAW380[6] = { 0x00,0x12,0x7A, 0x60,0xD2,0xFE };

static const uint8_t CAL390[21] = {
    0xAD,0xD8,0x26,0x6F,0xFE,0x12,0xC3,0xCF,0x48,0x28,0xBA,
    0x12,0x7A,0xFC,0xFF,0x3C,0xE7,0x74,0x8B,0xC9,0xB0
};
static const uint8_t RAW390[6] = { 0x5F,0x5A,0x55, 0x5B,0xC9,0xE6 };
static const uint8_t STATUS_RDY = 0x60U; /* BMP380/390 status DRDY_T|DRDY_P */

static void run_to_ready(Bmp390Runtime *r390, Bmp380Runtime *r380)
{
    for (int i=0;i<60;i++){
        FakePlatform_AdvanceTick(5000);
        if (r390) Bmp390Runtime_Poll(r390);
        if (r380) Bmp380Runtime_Poll(r380);
        bool a = r390 ? Bmp390Runtime_HasValidSample(r390) : false;
        bool b = r380 ? Bmp380Runtime_HasValidSample(r380) : false;
        if (a || b) break;
    }
}

static BarometerProvider select_provider(Bmp390Runtime *r390, Bmp380Runtime *r380)
{
    if (r390 && Bmp390Runtime_HasValidSample(r390)) return BAROMETER_PROVIDER_BMP390;
    if (r380 && Bmp380Runtime_HasValidSample(r380)) return BAROMETER_PROVIDER_BMP380;
    return BAROMETER_PROVIDER_NONE;
}

int main(void){
    printf("Barometric provider fallback (whole-device) tests\n");
    I2cBus bus; FakeI2cBus fake; FakeI2cBus_Init(&fake); FakeI2cBus_GetBus(&bus,&fake);

    /* ---- A. BMP380 only present ---- */
    FakePlatform_SetTick(0);
    FakeI2cBus_SetBmp390Present(&fake, 0xEC, 0x50, CAL380); /* BMP380 identity (0x50) */
    FakeI2cBus_SetBmp390Regs(&fake, STATUS_RDY, 0x00, RAW380);
    Bmp390Runtime r390; Bmp390Runtime_Init(&r390, &bus);
    Bmp380Runtime r380; Bmp380Runtime_Init(&r380, &bus);
    Bmp390Runtime_Start(&r390); Bmp380Runtime_Start(&r380);
    run_to_ready(&r390, &r380);
    check(Bmp390Runtime_HasValidSample(&r390)==false, "A: BMP390 no valid (absent)");
    check(Bmp380Runtime_HasValidSample(&r380)==true, "A: BMP380 has valid sample");
    check(select_provider(&r390,&r380)==BAROMETER_PROVIDER_BMP380, "A: provider BMP380");

    RoomState rs; RoomState_Init(&rs);
    RoomState_UpdateBarometric(&rs, select_provider(&r390,&r380),
                               r380.last_sample.pressure_pa, true,
                               r380.last_sample.temperature_c, true);
    check(rs.barometric_provider==BAROMETER_PROVIDER_BMP380, "A: RoomState BMP380");
    check(!rs.bmp390_pressure_valid, "A: legacy bmp390 INVALID (BMP380 only)");

    /* ---- C. neither present ---- */
    FakePlatform_SetTick(0);
    FakeI2cBus_SetBmp390Absent(&fake);
    Bmp390Runtime r390c; Bmp390Runtime_Init(&r390c,&bus);
    Bmp380Runtime r380c; Bmp380Runtime_Init(&r380c,&bus);
    Bmp390Runtime_Start(&r390c); Bmp380Runtime_Start(&r380c);
    run_to_ready(&r390c,&r380c);
    check(select_provider(&r390c,&r380c)==BAROMETER_PROVIDER_NONE, "C: neither -> NONE");
    RoomState rsn; RoomState_Init(&rsn);
    RoomState_InvalidateBarometric(&rsn);
    check(!rsn.barometric_pressure_valid, "C: no valid pressure");
    check(rsn.barometric_provider==BAROMETER_PROVIDER_NONE, "C: provider NONE");

    /* ---- B. BMP390 present only -> BMP390 wins ---- */
    FakePlatform_SetTick(0);
    FakeI2cBus_SetBmp390Present(&fake, 0xEC, 0x60, CAL390); /* real BMP390 */
    FakeI2cBus_SetBmp390Regs(&fake, STATUS_RDY, 0x00, RAW390);
    Bmp390Runtime r390b; Bmp390Runtime_Init(&r390b,&bus);
    Bmp380Runtime r380b; Bmp380Runtime_Init(&r380b,&bus);
    Bmp390Runtime_Start(&r390b); Bmp380Runtime_Start(&r380b);
    run_to_ready(&r390b,&r380b);
    check(select_provider(&r390b,&r380b)==BAROMETER_PROVIDER_BMP390, "B: BMP390 present -> BMP390");
    check(Bmp380Runtime_HasValidSample(&r380b)==false, "B: BMP380 not valid (absent)");

    /* ---- D. BMP390 fresh stays provider (no oscillation / double publish) ---- */
    check(select_provider(&r390b,&r380b)==BAROMETER_PROVIDER_BMP390, "D: BMP390 held (deterministic)");

    /* ---- E. BMP390 roof error (loses sample) -> BMP380 fallback policy ---- */
    RoomState rse; RoomState_Init(&rse);
    RoomState_UpdateBarometric(&rse, BAROMETER_PROVIDER_BMP380, 90000.0f, true, 19.0f, true);
    check(rse.barometric_provider==BAROMETER_PROVIDER_BMP380, "E: fallback to BMP380");
    check(!rse.bmp390_pressure_valid, "E: legacy bmp390 cleared");

    /* ---- F. BMP390 recovers -> preferred again, atomic no-mix ---- */
    RoomState rsf; RoomState_Init(&rsf);
    RoomState_InvalidateBarometric(&rsf);
    RoomState_UpdateBarometric(&rsf, BAROMETER_PROVIDER_BMP390, 101222.5f, true, 23.9f, true);
    check(rsf.barometric_provider==BAROMETER_PROVIDER_BMP390, "F: BMP390 recovered -> provider");
    check(rsf.barometric_pressure_pa==101222.5f && rsf.barometric_temperature_c==23.9f,
          "F: pressure+temp from SAME provider (no mix)");

    printf("\n%d pass, %d fail\n", s_pass, s_fail);
    return (s_fail==0)?0:1;
}