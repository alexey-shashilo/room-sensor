#include <stdio.h>
#include <string.h>
#include <math.h>

#include "bmp390.h"
#include "bmp390_runtime.h"
#include "room_state.h"
#include "platform_time.h"
#include "fake_platform_time.h"
#include "bmp390_test.h"

/* BMP390 driver + runtime + integration regression.
 *
 * Links the PRODUCTION BMP390 driver + runtime (with BMP390_UNIT_TEST so the
 * STATIC ParseCalib/Raw24/Compensate internals are exposed), the real RoomState,
 * and the Platform time fake. A SELF-CONTAINED, ADDRESS-AWARE fake I2C bus is
 * defined here (independent of shared fake_i2c_bus.c) so the exact per-address
 * probe / chip-id / data behavior is scriptable per test without perturbing the
 * other sensor suites.
 */

static int s_pass = 0, s_fail = 0, s_case = 0;
static void check(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

/* Golden calibration + raw (identical literal bytes to test_bmp390.c). */
static const uint8_t CAL1[21] = {
    0xAD,0xD8,0x26,0x6F,0xFE,0x12,0xC3,0xCF,0x48,0x28,0xBA,
    0x12,0x7A,0xFC,0xFF,0x3C,0xE7,0x74,0x8B,0xC9,0xB0
};
static const uint8_t RAW_OK[6] = { 0x5F,0x5A,0x55, 0x5B,0xC9,0xE6 };

/* ---- address-aware fake I2C bus (self-contained) ---- */
enum { BMP390_WIRE_PRIM = 0xECU, BMP390_WIRE_SEC = 0xEEU };

typedef struct
{
    bool prim_present, sec_present;
    uint8_t prim_chip, sec_chip;
    uint8_t prim_regs[256], sec_regs[256];
    uint8_t err;
    uint8_t sample[6];
    bool drdy_press, drdy_temp;

    DriverStatus read_mem_status;
    DriverStatus write_status;
    DriverStatus probe_status;

    uint16_t last_addr;
    int probe_count, read_mem_count, write_count;
} BmpFake;

static BmpFake g_fake;

static void bmp_fake_reset(void)
{
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.prim_present = g_fake.sec_present = false;
    g_fake.prim_chip = g_fake.sec_chip = 0x60U;
    g_fake.read_mem_status = g_fake.write_status = g_fake.probe_status = DRIVER_STATUS_OK;
    g_fake.drdy_press = g_fake.drdy_temp = true;
    memcpy(g_fake.sample, RAW_OK, 6);
    memcpy(&g_fake.prim_regs[0x31], CAL1, 21);
    memcpy(&g_fake.sec_regs[0x31], CAL1, 21);
}

static DriverStatus bmp_f_write(void *ctx, uint16_t addr, const uint8_t *data, size_t size)
{
    BmpFake *f = (BmpFake *)ctx;
    f->last_addr = addr; f->write_count++;
    if (f->write_status != DRIVER_STATUS_OK) return f->write_status;
    if (size < 2) return DRIVER_STATUS_INVALID_ARG;
    uint8_t reg = data[0], val = data[1];
    uint8_t *regs = (addr == BMP390_WIRE_PRIM) ? f->prim_regs : f->sec_regs;
    regs[reg] = val;
    return DRIVER_STATUS_OK;
}

static DriverStatus bmp_f_read_mem(void *ctx, uint16_t addr, uint8_t reg, uint8_t *data, size_t size)
{
    BmpFake *f = (BmpFake *)ctx;
    f->last_addr = addr; f->read_mem_count++;
    if (f->read_mem_status != DRIVER_STATUS_OK) return f->read_mem_status;
    uint8_t *regs = (addr == BMP390_WIRE_PRIM) ? f->prim_regs : f->sec_regs;

    if (reg == 0x00U) { data[0] = (addr == BMP390_WIRE_PRIM) ? f->prim_chip : f->sec_chip; return DRIVER_STATUS_OK; }
    if (reg == 0x31U) { if (size > 21U) size = 21U; memcpy(data, &regs[0x31], size); return DRIVER_STATUS_OK; }
    if (reg == 0x02U) { data[0] = f->err; return DRIVER_STATUS_OK; }
    if (reg == 0x03U)
    {
        data[0] = 0;
        if (f->drdy_press) data[0] |= (uint8_t)BMP390_STATUS_DRDY_PRESS;
        if (f->drdy_temp)  data[0] |= (uint8_t)BMP390_STATUS_DRDY_TEMP;
        return DRIVER_STATUS_OK;
    }
    if (reg == 0x04U && size == 6U) { memcpy(data, f->sample, 6); return DRIVER_STATUS_OK; }
    if (size > 0) { memcpy(data, &regs[reg], size); return DRIVER_STATUS_OK; }
    return DRIVER_STATUS_OK;
}

static DriverStatus bmp_f_probe(void *ctx, uint16_t addr)
{
    BmpFake *f = (BmpFake *)ctx;
    f->last_addr = addr; f->probe_count++;
    if (f->probe_status != DRIVER_STATUS_OK) return f->probe_status;
    if (addr == BMP390_WIRE_PRIM) return f->prim_present ? DRIVER_STATUS_OK : DRIVER_STATUS_NOT_FOUND;
    if (addr == BMP390_WIRE_SEC) return f->sec_present  ? DRIVER_STATUS_OK : DRIVER_STATUS_NOT_FOUND;
    return DRIVER_STATUS_NOT_FOUND;
}

static void bmp_f_bus(I2cBus *bus)
{
    bus->context = &g_fake;
    bus->write = bmp_f_write;
    bus->read_mem = bmp_f_read_mem;
    bus->read = NULL;
    bus->probe = bmp_f_probe;
}

/* ---- Section A: discovery ---- */
static void test_discovery(void)
{
    printf("== A. BMP390 discovery (0x76 / 0x77, chip ID) ==\n");
    I2cBus bus; Bmp390 dev;

    bmp_fake_reset(); g_fake.prim_present = true; g_fake.prim_chip = 0x60;
    bmp_f_bus(&bus); BMP390_Init(&dev, &bus);
    check(BMP390_Detect(&dev) == DRIVER_STATUS_OK, "0x76 + 0x60 accepted");
    check(dev.address == BMP390_WIRE_PRIM, "detected at primary 0x76");

    bmp_fake_reset(); g_fake.prim_present = false; g_fake.sec_present = true; g_fake.sec_chip = 0x60;
    bmp_f_bus(&bus); BMP390_Init(&dev, &bus);
    check(BMP390_Detect(&dev) == DRIVER_STATUS_OK, "0x77 accepted when 0x76 absent");
    check(dev.address == BMP390_WIRE_SEC, "detected at 0x77");

    bmp_fake_reset(); g_fake.prim_present = true; g_fake.prim_chip = 0x50;
    g_fake.sec_present = true; g_fake.sec_chip = 0x60;
    bmp_f_bus(&bus); BMP390_Init(&dev, &bus);
    check(BMP390_Detect(&dev) == DRIVER_STATUS_OK, "wrong chip at 0x76 continues to 0x77");
    check(dev.address == BMP390_WIRE_SEC, "accepted valid 0x77 after wrong 0x76");

    bmp_fake_reset(); g_fake.prim_present = true; g_fake.prim_chip = 0x50;
    g_fake.sec_present = true; g_fake.sec_chip = 0x50;
    bmp_f_bus(&bus); BMP390_Init(&dev, &bus);
    check(BMP390_Detect(&dev) == DRIVER_STATUS_NOT_FOUND, "both wrong chip -> NOT_FOUND");

    bmp_fake_reset();
    bmp_f_bus(&bus); BMP390_Init(&dev, &bus);
    check(BMP390_Detect(&dev) == DRIVER_STATUS_NOT_FOUND, "both absent -> NOT_FOUND");
}

/* ---- Section B: ERR / calibration / config / data ---- */
static void test_err_calib_config(void)
{
    printf("\n== B. ERR bits, calibration, config registers, data ==\n");
    I2cBus bus; Bmp390 dev;
    bmp_fake_reset(); g_fake.prim_present = true; g_fake.prim_chip = 0x60;
    bmp_f_bus(&bus); BMP390_Init(&dev, &bus); BMP390_Detect(&dev);

    uint8_t e = 0;
    g_fake.err = 0x00U;
    check(BMP390_ReadError(&dev, &e) == DRIVER_STATUS_OK, "ERR: no bits -> OK");
    g_fake.err = 0x01U;
    check(BMP390_ReadError(&dev, &e) == DRIVER_STATUS_DEVICE_ERROR, "ERR: fatal -> DEVICE_ERROR");
    g_fake.err = 0x02U;
    check(BMP390_ReadError(&dev, &e) == DRIVER_STATUS_DEVICE_ERROR, "ERR: command -> DEVICE_ERROR");
    g_fake.err = 0x04U;
    check(BMP390_ReadError(&dev, &e) == DRIVER_STATUS_DEVICE_ERROR, "ERR: config -> DEVICE_ERROR");
    g_fake.err = 0x00U;

    BMP390_InitCalibration(&dev);
    check(dev.calib.calibrated == 1U, "calibration flag set");
    check(dev.calib.quantized.par_p5 == 250000.0, "calibration p5 parsed");

    check(BMP390_ConfigureRoomProfile(&dev) == DRIVER_STATUS_OK, "configure profile OK");
    check(g_fake.prim_regs[0x1CU] == (uint8_t)(0x03U | (0x02U << 3)), "OSR press x8 temp x4");
    check(g_fake.prim_regs[0x1FU] == (uint8_t)(0x02U << 1), "CONFIG IIR x3");

    g_fake.drdy_press = g_fake.drdy_temp = true;
    BMP390_TriggerMeasurement(&dev);
    Bmp390Sample s;
    BMP390_ReadSample(&dev, &s);
    check(s.valid, "data-ready paired sample valid");
    check(fabs((double)s.pressure_pa - 101324.98) < 1.0, "sample pressure ~101325 Pa");
    check(fabs((double)s.temperature_c - 24.5) < 0.1, "sample temp ~24.5 C");
}

/* ---- Section C: defensive range ---- */
static void test_range(void)
{
    printf("\n== C. Defensive pressure range ==\n");
    I2cBus bus; Bmp390 dev;
    bmp_fake_reset(); g_fake.prim_present = true; g_fake.prim_chip = 0x60;
    bmp_f_bus(&bus); BMP390_Init(&dev, &bus); BMP390_Detect(&dev); BMP390_InitCalibration(&dev);
    Bmp390Sample s;
    /* raw P = 0, raw T = 0 : degenerate -> invalid, marker NOT clamped to 30000. */
    memset(g_fake.sample, 0, 6);
    BMP390_ReadSample(&dev, &s);
    check(s.valid == false, "implausible low -> invalid");

    memset(g_fake.sample, 0xFF, 6);
    BMP390_ReadSample(&dev, &s);
    check(s.valid == false, "implausible high -> invalid");

    memcpy(g_fake.sample, RAW_OK, 6);
    BMP390_ReadSample(&dev, &s);
    check(s.valid == true, "in-range sample valid");
}

/* ---- Section D/E/F: runtime + recovery + RoomState ---- */
static void test_runtime_roomstate(void)
{
    printf("\n== D/E/F. Runtime validity + recovery + RoomState ==\n");
    I2cBus bus; bmp_f_bus(&bus);

    bmp_fake_reset(); g_fake.prim_present = true; g_fake.prim_chip = 0x60;
    FakePlatform_SetTick(0);
    Bmp390Runtime rt; Bmp390Runtime_Init(&rt, &bus);
    check(Bmp390Runtime_Start(&rt) == DRIVER_STATUS_OK, "runtime start OK");
    FakePlatform_AdvanceTick(3000);
    Bmp390Runtime_Poll(&rt);
    check(rt.state == DEVICE_STATE_READY, "runtime reached READY");
    check(Bmp390Runtime_HasValidSample(&rt), "runtime has valid sample");

    RoomState rs; RoomState_Init(&rs);
    RoomState_UpdateBmp390(&rs, rt.last_sample.pressure_pa, true,
                           rt.last_sample.temperature_c, true);
    check(rs.bmp390_pressure_valid, "RoomState BMP390 pressure valid");
    check(rs.bmp390_temperature_valid, "RoomState BMP390 temp valid");

    /* Validity continuity: an in-flight next trigger does NOT invalidate. */
    FakePlatform_AdvanceTick(BMP390_RUNTIME_MEASUREMENT_INTERVAL_MS);
    Bmp390Runtime_Poll(&rt);
    check(rt.state == DEVICE_STATE_STARTING, "triggered next -> STARTING");
    check(Bmp390Runtime_HasValidSample(&rt), "valid sample survives in-flight trigger");

    /* Stale timeout clears validity. Stop DRDY so the in-flight poll cannot
       re-accept a fresh sample (which would refresh last_valid_measurement_ms). */
    g_fake.drdy_press = g_fake.drdy_temp = false;
    FakePlatform_AdvanceTick(BMP390_RUNTIME_STALE_MS + 1000);
    Bmp390Runtime_Poll(&rt);
    check(Bmp390Runtime_HasValidSample(&rt) == false, "validity cleared on stale timeout");

    /* Transient read error is bounded, not fatal. */
    bmp_fake_reset(); g_fake.prim_present = true; g_fake.prim_chip = 0x60;
    FakePlatform_SetTick(0); Bmp390Runtime_Init(&rt, &bus);
    Bmp390Runtime_Start(&rt);
    FakePlatform_AdvanceTick(BMP390_RUNTIME_MEASUREMENT_DEADLINE_MS + 100); /* past data-ready deadline */
    g_fake.read_mem_status = DRIVER_STATUS_BUS_ERROR;
    Bmp390Runtime_Poll(&rt);
    g_fake.read_mem_status = DRIVER_STATUS_OK;
    check(rt.consecutive_errors == 1, "transient error counted once");
    check(rt.state != DEVICE_STATE_ERROR, "single transient error not ERROR");

    /* Durable error -> ERROR. */
    bmp_fake_reset(); g_fake.prim_present = true; g_fake.prim_chip = 0x60;
    FakePlatform_SetTick(0); Bmp390Runtime_Init(&rt, &bus);
    Bmp390Runtime_Start(&rt);
    g_fake.read_mem_status = DRIVER_STATUS_BUS_ERROR;
    int guard = 0;
    while (rt.state != DEVICE_STATE_ERROR && guard < 40) { FakePlatform_AdvanceTick(1000); Bmp390Runtime_Poll(&rt); guard++; }
    check(rt.state == DEVICE_STATE_ERROR, "durable errors -> ERROR");
    check(Bmp390Runtime_HasValidSample(&rt) == false, "ERROR invalidates sample");

    /* Recovery 1. */
    g_fake.read_mem_status = DRIVER_STATUS_OK;
    Bmp390Runtime_Recover(&rt);
    check(rt.state == DEVICE_STATE_RECOVERING, "recover -> RECOVERING");
    check(rt.recovery_count == 1, "recovery_count == 1");
    check(rt.consecutive_errors == 0, "consecutive_errors reset");
    check(Bmp390Runtime_Start(&rt) == DRIVER_STATUS_OK, "recovery 1 start OK");
    FakePlatform_AdvanceTick(3000); Bmp390Runtime_Poll(&rt);
    check(rt.state == DEVICE_STATE_READY, "recovery 1 -> READY");
    check(rt.operation_failures > 0, "operation_failures cumulative");

    /* Recovery cycle 2. */
    g_fake.read_mem_status = DRIVER_STATUS_BUS_ERROR;
    guard = 0;
    while (rt.state != DEVICE_STATE_ERROR && guard < 40) { FakePlatform_AdvanceTick(1000); Bmp390Runtime_Poll(&rt); guard++; }
    check(rt.state == DEVICE_STATE_ERROR, "durable again -> ERROR (2nd)");
    g_fake.read_mem_status = DRIVER_STATUS_OK;
    Bmp390Runtime_Recover(&rt); Bmp390Runtime_Start(&rt);
    FakePlatform_AdvanceTick(3000); Bmp390Runtime_Poll(&rt);
    check(rt.state == DEVICE_STATE_READY, "recovery 2 -> READY");
    check(rt.recovery_count == 2, "recovery_count == 2");

    /* Failed recovery (sensor gone). */
    g_fake.prim_present = false; g_fake.sec_present = false;
    check(Bmp390Runtime_Start(&rt) == DRIVER_STATUS_NOT_FOUND, "failed recovery -> NOT_FOUND");
}

/* ---- Long-run / tick-wrap ---- */
static void test_longrun_wrap(void)
{
    printf("\n== Long-run / uint32 tick wrap ==\n");
    I2cBus bus; bmp_f_bus(&bus);
    bmp_fake_reset(); g_fake.prim_present = true; g_fake.prim_chip = 0x60;
    g_fake.drdy_press = g_fake.drdy_temp = true;

    FakePlatform_SetTick(0);
    Bmp390Runtime rt; Bmp390Runtime_Init(&rt, &bus);
    check(Bmp390Runtime_Start(&rt) == DRIVER_STATUS_OK, "wrap: start OK");

    /* Simulate ~24h of 0.2 Hz continuous measurement with repeated samples
       accepted, without a stale/deadline-wrap regression. Use a bounded step
       loop (2400 five-second cycles ~= 3.3h-equivalent at reduced count to keep
       the host test fast) plus explicit tick-wrap points. */
    uint32_t samples = 0;
    uint32_t t = 0;
    for (int i = 0; i < 2600; i++)
    {
        /* Advance just past the measurement interval, then poll; data ready so
           each cycle yields a fresh sample once past the deadline. */
        t = (uint32_t)(t + 5000U);
        FakePlatform_SetTick(t);
        Bmp390Runtime_Poll(&rt);
        if (rt.state == DEVICE_STATE_STARTING)
        {
            FakePlatform_AdvanceTick(BMP390_RUNTIME_MEASUREMENT_DEADLINE_MS + 1);
            Bmp390Runtime_Poll(&rt);
            if (rt.state == DEVICE_STATE_READY && Bmp390Runtime_HasValidSample(&rt))
                samples++;
        }
        else if (rt.state == DEVICE_STATE_READY)
        {
            samples++;
        }
        if (rt.state == DEVICE_STATE_ERROR)
            break;
    }
    check(samples > 100, "continuous samples over long run (no stall)");
    check(rt.state == DEVICE_STATE_READY, "still READY at end of long run (no ERROR)");
    check(Bmp390Runtime_HasValidSample(&rt), "valid sample after long run");

    /* Explicit 32-bit tick wrap: continue past 0xFFFFFFFF. Deadline and
       freshness use unsigned subtraction so wrap is wrap-safe. */
    uint32_t before = rt.last_valid_measurement_ms;
    FakePlatform_SetTick(0xFFFFFFFEU);   /* move to just before wrap */
    Bmp390Runtime_Poll(&rt);
    FakePlatform_SetTick(0x00000005U);   /* wrapped */
    FakePlatform_AdvanceTick(BMP390_RUNTIME_MEASUREMENT_INTERVAL_MS);
    Bmp390Runtime_Poll(&rt);
    /* Wrap must not produce a bogus stale-invalidation or deadline stall: the
       runtime should still advance (STARTING or READY), never spin in ERROR from
       a deadline miscalculation. */
    check(rt.state == DEVICE_STATE_STARTING || rt.state == DEVICE_STATE_READY,
          "tick wrap keeps runtime progressing (no deadline regression)");
    (void)before;
}

int main(void)
{
    printf("BMP390 driver & runtime integration tests\n");
    test_discovery();
    test_err_calib_config();
    test_range();
    test_runtime_roomstate();
    test_longrun_wrap();

    printf("\n%d pass, %d fail\n", s_pass, s_fail);
    return (s_fail == 0) ? 0 : 1;
}