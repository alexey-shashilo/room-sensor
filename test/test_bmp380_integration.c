#include <stdio.h>
#include <string.h>
#include <math.h>

#include "bmp380.h"
#include "bmp380_runtime.h"
#include "platform_time.h"
#include "fake_platform_time.h"
#include "bmp380_test.h"

/* BMP380 driver + runtime integration regression (Phase 17.7).
 *
 * Links the PRODUCTION BMP380 driver + runtime (with BMP380_UNIT_TEST so the
 * STATIC ParseCalib/Raw24/Compensate internals are exposed), the Platform time
 * fake, and a SELF-CONTAINED address-aware fake I2C bus (independent of the
 * shared fake_i2c_bus.c) so per-address probe / chip-id (0x50) / data behavior is
 * scriptable without perturbing other suites.
 */

static int s_pass = 0, s_fail = 0, s_case = 0;
static void check(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

/* Golden BMP380 calibration + raw (from test_bmp380.c frozen Bosch oracle). */
static const uint8_t CAL1[21] = {
    93,198,87,28,0, 201,69,0,0,0,0, 242,43,0,0,0,0,0,64,0,0
};
static const uint8_t RAW_OK[6] = { 0x00,0x12,0x7A, 0x60,0xD2,0xFE }; /* P=0x7A1200, T=0xFED260 */

/* Realistic full BMP3 calibration (identical register layout for BMP380/390,
   taken from the BMP390 golden fixture) whose raw extremes genuinely leave the
   operating range (30000..125000 Pa), exercising the defensive range gate. */
static const uint8_t CAL_REAL[21] = {
    0xAD,0xD8,0x26,0x6F,0xFE, 0x12,0xC3,0xCF,0x48,0x28,0xBA,
    0x12,0x7A,0xFC,0xFF,0x3C,0xE7, 0x74,0x8B,0xC9,0xB0
};
static const uint8_t RAW_REAL_OK[6] = { 0x5F,0x5A,0x55, 0x5B,0xC9,0xE6 }; /* P=0x555A5F,T=0xE6C95B */

enum { BMP380_WIRE_PRIM = 0xECU, BMP380_WIRE_SEC = 0xEEU };

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
    DriverStatus read_sample_status;

    uint16_t last_addr;
    int probe_count, read_mem_count, write_count;
} BmpFake;

static BmpFake g_fake;

static void bmp_fake_reset(void)
{
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.prim_present = g_fake.sec_present = false;
    g_fake.prim_chip = g_fake.sec_chip = 0x50U;
    g_fake.read_mem_status = g_fake.write_status = g_fake.probe_status = DRIVER_STATUS_OK;
    g_fake.read_sample_status = DRIVER_STATUS_OK;
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
    uint8_t *regs = (addr == BMP380_WIRE_PRIM) ? f->prim_regs : f->sec_regs;
    regs[reg] = val;
    return DRIVER_STATUS_OK;
}

static DriverStatus bmp_f_read_mem(void *ctx, uint16_t addr, uint8_t reg, uint8_t *data, size_t size)
{
    BmpFake *f = (BmpFake *)ctx;
    f->last_addr = addr; f->read_mem_count++;
    uint8_t *regs = (addr == BMP380_WIRE_PRIM) ? f->prim_regs : f->sec_regs;

    if (reg == 0x04U && size == 6U)
    {
        if (f->read_sample_status != DRIVER_STATUS_OK) return f->read_sample_status;
        memcpy(data, f->sample, 6);
        return DRIVER_STATUS_OK;
    }
    if (f->read_mem_status != DRIVER_STATUS_OK) return f->read_mem_status;

    if (reg == 0x00U) { data[0] = (addr == BMP380_WIRE_PRIM) ? f->prim_chip : f->sec_chip; return DRIVER_STATUS_OK; }
    if (reg == 0x31U) { if (size > 21U) size = 21U; memcpy(data, &regs[0x31], size); return DRIVER_STATUS_OK; }
    if (reg == 0x02U) { data[0] = f->err; return DRIVER_STATUS_OK; }
    if (reg == 0x03U)
    {
        data[0] = 0;
        if (f->drdy_press) data[0] |= (uint8_t)BMP380_STATUS_DRDY_PRESS;
        if (f->drdy_temp)  data[0] |= (uint8_t)BMP380_STATUS_DRDY_TEMP;
        return DRIVER_STATUS_OK;
    }
    if (size > 0) { memcpy(data, &regs[reg], size); return DRIVER_STATUS_OK; }
    return DRIVER_STATUS_OK;
}

static DriverStatus bmp_f_probe(void *ctx, uint16_t addr)
{
    BmpFake *f = (BmpFake *)ctx;
    f->last_addr = addr; f->probe_count++;
    if (f->probe_status != DRIVER_STATUS_OK) return f->probe_status;
    if (addr == BMP380_WIRE_PRIM) return f->prim_present ? DRIVER_STATUS_OK : DRIVER_STATUS_NOT_FOUND;
    if (addr == BMP380_WIRE_SEC) return f->sec_present  ? DRIVER_STATUS_OK : DRIVER_STATUS_NOT_FOUND;
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

/* ---- Section A: discovery (0x50 accepted; 0x60 rejected) ---- */
static void test_discovery(void)
{
    printf("== A. BMP380 discovery (0x76 / 0x77, chip 0x50) ==\n");
    I2cBus bus; Bmp380 dev;

    bmp_fake_reset(); g_fake.prim_present = true; g_fake.prim_chip = 0x50;
    bmp_f_bus(&bus); BMP380_Init(&dev, &bus);
    check(BMP380_Detect(&dev) == DRIVER_STATUS_OK, "0x76 + 0x50 accepted");
    check(dev.address == BMP380_WIRE_PRIM, "detected at primary 0x76");

    bmp_fake_reset(); g_fake.prim_present = false; g_fake.sec_present = true; g_fake.sec_chip = 0x50;
    bmp_f_bus(&bus); BMP380_Init(&dev, &bus);
    check(BMP380_Detect(&dev) == DRIVER_STATUS_OK, "0x77 accepted when 0x76 absent");
    check(dev.address == BMP380_WIRE_SEC, "detected at 0x77");

    bmp_fake_reset(); g_fake.prim_present = true; g_fake.prim_chip = 0x50;
    g_fake.sec_present = true; g_fake.sec_chip = 0x60;
    bmp_f_bus(&bus); BMP380_Init(&dev, &bus);
    check(BMP380_Detect(&dev) == DRIVER_STATUS_OK, "0x50 at 0x76, 0x60 at 0x77 -> 0x76 wins");
    check(dev.address == BMP380_WIRE_PRIM, "accepted 0x76 (valid BMP380)");

    bmp_fake_reset(); g_fake.prim_present = true; g_fake.prim_chip = 0x60;
    g_fake.sec_present = true; g_fake.sec_chip = 0x60;
    bmp_f_bus(&bus); BMP380_Init(&dev, &bus);
    check(BMP380_Detect(&dev) == DRIVER_STATUS_NOT_FOUND, "BMP380 rejects 0x60 identities");

    bmp_fake_reset();
    bmp_f_bus(&bus); BMP380_Init(&dev, &bus);
    check(BMP380_Detect(&dev) == DRIVER_STATUS_NOT_FOUND, "both absent -> NOT_FOUND");
}

/* ---- Section B: ERR / calibration / config / data ---- */
static void test_err_calib_config(void)
{
    printf("\n== B. ERR bits, calibration, config registers, data ==\n");
    I2cBus bus; Bmp380 dev;
    bmp_fake_reset(); g_fake.prim_present = true; g_fake.prim_chip = 0x50;
    bmp_f_bus(&bus); BMP380_Init(&dev, &bus); BMP380_Detect(&dev);

    uint8_t e = 0;
    g_fake.err = 0x00U;
    check(BMP380_ReadError(&dev, &e) == DRIVER_STATUS_OK, "ERR: no bits -> OK");
    g_fake.err = 0x01U;
    check(BMP380_ReadError(&dev, &e) == DRIVER_STATUS_DEVICE_ERROR, "ERR: fatal -> DEVICE_ERROR");
    g_fake.err = 0x02U;
    check(BMP380_ReadError(&dev, &e) == DRIVER_STATUS_DEVICE_ERROR, "ERR: command -> DEVICE_ERROR");
    g_fake.err = 0x04U;
    check(BMP380_ReadError(&dev, &e) == DRIVER_STATUS_DEVICE_ERROR, "ERR: config -> DEVICE_ERROR");
    g_fake.err = 0x00U;

    BMP380_InitCalibration(&dev);
    check(dev.calib.calibrated == 1U, "calibration flag set");

    check(BMP380_ConfigureRoomProfile(&dev) == DRIVER_STATUS_OK, "configure profile OK");
    check(g_fake.prim_regs[0x1CU] == (uint8_t)(0x03U | (0x02U << 3)), "OSR press x8 temp x4");
    check(g_fake.prim_regs[0x1FU] == (uint8_t)(0x02U << 1), "CONFIG IIR x3");
}

/* ---- Section C: defensive range ---- */
static void test_range(void)
{
    printf("\n== C. Defensive pressure range ==\n");
    I2cBus bus; Bmp380 dev;
    bmp_fake_reset(); g_fake.prim_present = true; g_fake.prim_chip = 0x50;
    /* Use the realistic calibration whose extremes leave range. */
    memcpy(&g_fake.prim_regs[0x31], CAL_REAL, 21);
    bmp_f_bus(&bus); BMP380_Init(&dev, &bus); BMP380_Detect(&dev); BMP380_InitCalibration(&dev);
    Bmp380Sample s;
    /* raw P=0, raw T=0 -> degenerate -> invalid (not clamped to 30000). */
    memset(g_fake.sample, 0, 6);
    BMP380_ReadSample(&dev, &s);
    check(s.valid == false, "implausible low -> invalid");
    memset(g_fake.sample, 0xFF, 6);
    BMP380_ReadSample(&dev, &s);
    check(s.valid == false, "implausible high -> invalid");
    memcpy(g_fake.sample, RAW_REAL_OK, 6);
    BMP380_ReadSample(&dev, &s);
    check(s.valid == true, "realistic in-range sample valid");
    check(fabs((double)s.pressure_pa - 101324.9846281795) < 1.0, "sample pressure ~101325 Pa");
    check(fabs((double)s.temperature_c - 24.5000066664) < 0.05, "sample temp ~24.500 C");
}

/* ---- Runtime + recovery ---- */
static void test_runtime_recovery(void)
{
    printf("\n== D/E/F. Runtime validity + recovery ==\n");
    I2cBus bus; bmp_f_bus(&bus);

    bmp_fake_reset(); g_fake.prim_present = true; g_fake.prim_chip = 0x50;
    FakePlatform_SetTick(0);
    Bmp380Runtime rt; Bmp380Runtime_Init(&rt, &bus);
    check(Bmp380Runtime_Start(&rt) == DRIVER_STATUS_OK, "runtime start OK");
    FakePlatform_AdvanceTick(3000);
    Bmp380Runtime_Poll(&rt);
    check(rt.state == DEVICE_STATE_READY, "runtime reached READY");
    check(Bmp380Runtime_HasValidSample(&rt), "runtime has valid sample");

    /* Validity continuity across in-flight trigger. */
    FakePlatform_AdvanceTick(BMP380_RUNTIME_MEASUREMENT_INTERVAL_MS);
    Bmp380Runtime_Poll(&rt);
    check(rt.state == DEVICE_STATE_STARTING, "triggered next -> STARTING");
    check(Bmp380Runtime_HasValidSample(&rt), "valid sample survives in-flight trigger");

    /* Stale timeout. */
    g_fake.drdy_press = g_fake.drdy_temp = false;
    FakePlatform_AdvanceTick(BMP380_RUNTIME_STALE_MS + 1000);
    Bmp380Runtime_Poll(&rt);
    check(Bmp380Runtime_HasValidSample(&rt) == false, "validity cleared on stale timeout");

    /* Transient error bounded. */
    bmp_fake_reset(); g_fake.prim_present = true; g_fake.prim_chip = 0x50;
    FakePlatform_SetTick(0); Bmp380Runtime_Init(&rt, &bus);
    Bmp380Runtime_Start(&rt);
    FakePlatform_AdvanceTick(BMP380_RUNTIME_MEASUREMENT_DEADLINE_MS + 100);
    g_fake.read_mem_status = DRIVER_STATUS_BUS_ERROR;
    Bmp380Runtime_Poll(&rt);
    g_fake.read_mem_status = DRIVER_STATUS_OK;
    check(rt.consecutive_errors == 1, "transient error counted once");
    check(rt.state != DEVICE_STATE_ERROR, "single transient error not ERROR");

    /* Durable error -> ERROR. */
    bmp_fake_reset(); g_fake.prim_present = true; g_fake.prim_chip = 0x50;
    FakePlatform_SetTick(0); Bmp380Runtime_Init(&rt, &bus);
    Bmp380Runtime_Start(&rt);
    g_fake.read_mem_status = DRIVER_STATUS_BUS_ERROR;
    int guard = 0;
    while (rt.state != DEVICE_STATE_ERROR && guard < 40) { FakePlatform_AdvanceTick(1000); Bmp380Runtime_Poll(&rt); guard++; }
    check(rt.state == DEVICE_STATE_ERROR, "durable errors -> ERROR");
    check(Bmp380Runtime_HasValidSample(&rt) == false, "ERROR invalidates sample");

    /* Recovery. */
    g_fake.read_mem_status = DRIVER_STATUS_OK;
    Bmp380Runtime_Recover(&rt);
    check(rt.state == DEVICE_STATE_RECOVERING, "recover -> RECOVERING");
    check(rt.recovery_count == 1, "recovery_count == 1");
    check(Bmp380Runtime_Start(&rt) == DRIVER_STATUS_OK, "recovery start OK");
    FakePlatform_AdvanceTick(3000); Bmp380Runtime_Poll(&rt);
    check(rt.state == DEVICE_STATE_READY, "recovery -> READY");

    /* Failed recovery (sensor gone). */
    g_fake.prim_present = false; g_fake.sec_present = false;
    check(Bmp380Runtime_Start(&rt) == DRIVER_STATUS_NOT_FOUND, "failed recovery -> NOT_FOUND");
}

/* ---- Long run / tick wrap ---- */
static void test_longrun_wrap(void)
{
    printf("\n== Long-run / uint32 tick wrap ==\n");
    I2cBus bus; bmp_f_bus(&bus);
    bmp_fake_reset(); g_fake.prim_present = true; g_fake.prim_chip = 0x50;
    g_fake.drdy_press = g_fake.drdy_temp = true;

    FakePlatform_SetTick(0);
    Bmp380Runtime rt; Bmp380Runtime_Init(&rt, &bus);
    check(Bmp380Runtime_Start(&rt) == DRIVER_STATUS_OK, "wrap: start OK");

    uint32_t samples = 0;
    uint32_t t = 0;
    for (int i = 0; i < 2600; i++)
    {
        t = (uint32_t)(t + 5000U);
        FakePlatform_SetTick(t);
        Bmp380Runtime_Poll(&rt);
        if (rt.state == DEVICE_STATE_STARTING)
        {
            FakePlatform_AdvanceTick(BMP380_RUNTIME_MEASUREMENT_DEADLINE_MS + 1);
            Bmp380Runtime_Poll(&rt);
            if (rt.state == DEVICE_STATE_READY && Bmp380Runtime_HasValidSample(&rt))
                samples++;
        }
        else if (rt.state == DEVICE_STATE_READY)
            samples++;
        if (rt.state == DEVICE_STATE_ERROR)
            break;
    }
    check(samples > 100, "continuous samples over long run (no stall)");
    check(rt.state == DEVICE_STATE_READY, "still READY at end of long run");

    FakePlatform_SetTick(0xFFFFFFFEU);
    Bmp380Runtime_Poll(&rt);
    FakePlatform_SetTick(0x00000005U);
    FakePlatform_AdvanceTick(BMP380_RUNTIME_MEASUREMENT_INTERVAL_MS);
    Bmp380Runtime_Poll(&rt);
    check(rt.state == DEVICE_STATE_STARTING || rt.state == DEVICE_STATE_READY,
          "tick wrap keeps runtime progressing (no deadline regression)");
}

int main(void)
{
    printf("BMP380 driver & runtime integration tests\n");
    test_discovery();
    test_err_calib_config();
    test_range();
    test_runtime_recovery();
    test_longrun_wrap();

    printf("\n%d pass, %d fail\n", s_pass, s_fail);
    return (s_fail == 0) ? 0 : 1;
}