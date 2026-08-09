#include <stdio.h>
#include <math.h>
#include <string.h>

#include "veml7700_test.h"
#include "veml7700.h"
#include "i2c_bus.h"

#include "fake_i2c_bus.h"
#include "fake_platform_time.h"

static int s_pass = 0;
static int s_fail = 0;
static int s_scenario = 0;

static void scenario(const char *name)
{
    s_scenario++;
    printf("\n[Scenario %d] %s\n", s_scenario, name);
}

static void check(int cond, const char *msg)
{
    if (cond) { s_pass++; printf("  PASS: %s\n", msg); }
    else      { s_fail++; printf("  FAIL: %s\n", msg); }
}

static void setup(FakeI2cBus *fake, I2cBus *bus, VEML7700_HandleTypeDef *dev)
{
    FakeI2cBus_Init(fake);
    FakeI2cBus_GetBus(bus, fake);
    memset(dev, 0, sizeof(*dev));
    dev->bus = bus;
    dev->initialized = 1;
    dev->persistence = VEML7700_PERS_1;
    dev->range_index = 4;
    dev->gain = VEML7700_GAIN_1;
    dev->integration_time = VEML7700_IT_50_MS;
    dev->resolution = 0.115200f;
    dev->range_state = VEML7700_RANGE_STABLE;
}

int main(void)
{
    printf("VEML7700 Sample-Validity and Diagnostics Tests\n");

    /* ================================================================
       A-C: LOW conditions
       ================================================================ */
    scenario("A: LOW #1 — valid, not changed");
    {
        FakeI2cBus fake; I2cBus bus; VEML7700_HandleTypeDef dev; VEML7700_Sample s;
        setup(&fake, &bus, &dev);
        FakeI2cBus_SetAlsRead(&fake, 10);

        VEML7700_ReadWithAutoRange(&dev, &s);
        check(s.valid, "LOW #1: valid = true");
        check(!s.range_changed, "LOW #1: range_changed = false");
        check(fabs(s.lux - 10.0f * 0.115200f) < 0.001f, "LOW #1: lux = 10 * resolution");
        check(dev.range_index == 4, "LOW #1: index unchanged");
    }

    scenario("B: LOW #2 — valid, not changed");
    {
        FakeI2cBus fake; I2cBus bus; VEML7700_HandleTypeDef dev; VEML7700_Sample s;
        setup(&fake, &bus, &dev);
        FakeI2cBus_SetAlsRead(&fake, 10);

        VEML7700_ReadWithAutoRange(&dev, &s);
        VEML7700_ReadWithAutoRange(&dev, &s);
        check(s.valid, "LOW #2: valid = true");
        check(!s.range_changed, "LOW #2: range_changed = false");
        check(dev.range_index == 4, "LOW #2: index unchanged");
    }

    scenario("C: LOW #3 converges — range change, invalid");
    {
        FakeI2cBus fake; I2cBus bus; VEML7700_HandleTypeDef dev; VEML7700_Sample s;
        setup(&fake, &bus, &dev);
        FakeI2cBus_SetAlsRead(&fake, 10);

        VEML7700_ReadWithAutoRange(&dev, &s);
        VEML7700_ReadWithAutoRange(&dev, &s);
        VEML7700_ReadWithAutoRange(&dev, &s);
        check(!s.valid, "LOW #3: valid = false (range change)");
        check(s.range_changed, "LOW #3: range_changed = true");
        check(s.settling, "LOW #3: settling = true");
        check(dev.range_index == 5, "LOW #3: index = 5");
    }

    /* ================================================================
       D-F: HIGH conditions
       ================================================================ */
    scenario("D: HIGH #1 — valid, not changed");
    {
        FakeI2cBus fake; I2cBus bus; VEML7700_HandleTypeDef dev; VEML7700_Sample s;
        setup(&fake, &bus, &dev);
        FakeI2cBus_SetAlsRead(&fake, 15000);

        VEML7700_ReadWithAutoRange(&dev, &s);
        check(s.valid, "HIGH #1: valid = true");
        check(!s.range_changed, "HIGH #1: range_changed = false");
        check(dev.range_index == 4, "HIGH #1: index unchanged");
    }

    scenario("E: HIGH #2 — valid, not changed");
    {
        FakeI2cBus fake; I2cBus bus; VEML7700_HandleTypeDef dev; VEML7700_Sample s;
        setup(&fake, &bus, &dev);
        FakeI2cBus_SetAlsRead(&fake, 15000);

        VEML7700_ReadWithAutoRange(&dev, &s);
        VEML7700_ReadWithAutoRange(&dev, &s);
        check(s.valid, "HIGH #2: valid = true");
        check(!s.range_changed, "HIGH #2: range_changed = false");
        check(dev.range_index == 4, "HIGH #2: index unchanged");
    }

    scenario("F: HIGH #3 converges — range change, invalid");
    {
        FakeI2cBus fake; I2cBus bus; VEML7700_HandleTypeDef dev; VEML7700_Sample s;
        setup(&fake, &bus, &dev);
        FakeI2cBus_SetAlsRead(&fake, 15000);

        VEML7700_ReadWithAutoRange(&dev, &s);
        VEML7700_ReadWithAutoRange(&dev, &s);
        VEML7700_ReadWithAutoRange(&dev, &s);
        check(!s.valid, "HIGH #3: valid = false (range change)");
        check(s.range_changed, "HIGH #3: range_changed = true");
        check(s.settling, "HIGH #3: settling = true");
        check(dev.range_index == 3, "HIGH #3: index = 3");
    }

    /* ================================================================
       G: last_attempt after normal measurement
       ================================================================ */
    scenario("G: last_attempt after normal valid measurement");
    {
        FakeI2cBus fake; I2cBus bus; VEML7700_HandleTypeDef dev; VEML7700_Sample s;
        setup(&fake, &bus, &dev);
        FakeI2cBus_SetAlsRead(&fake, 1000);

        VEML7700_ReadWithAutoRange(&dev, &s);
        check(dev.last_attempt.valid, "last_attempt.valid = true");
        check(dev.last_attempt.als_raw == 1000, "last_attempt.raw = 1000");
        check(fabsf(dev.last_attempt.lux - 1000.0f * 0.115200f) < 0.001f, "last_attempt.lux computed");
        check(dev.last_valid.valid, "last_valid also valid");
        check(dev.last_valid.als_raw == 1000, "last_valid.raw = 1000");
    }

    /* ================================================================
       H: last_attempt during settling
       ================================================================ */
    scenario("H: last_attempt during settling");
    {
        FakeI2cBus fake; I2cBus bus; VEML7700_HandleTypeDef dev; VEML7700_Sample s;
        setup(&fake, &bus, &dev);
        FakePlatform_SetTick(0);
        dev.range_state = VEML7700_RANGE_SETTLING;
        dev.settle_start_ms = 0;
        dev.settle_duration_ms = 100;
        /* set a valid sample first */
        dev.last_valid.valid = true;
        dev.last_valid.lux = 100.0f;
        dev.last_valid.als_raw = 500;

        FakePlatform_SetTick(50);
        FakeI2cBus_SetAlsRead(&fake, 1000);
        VEML7700_ReadWithAutoRange(&dev, &s);

        check(dev.last_attempt.settling, "last_attempt.settling = true");
        check(!dev.last_attempt.valid, "last_attempt.valid = false");
        check(dev.last_attempt.als_raw == 1000, "last_attempt.raw = 1000");
        check(dev.last_valid.valid, "last_valid unchanged (still valid)");
        check(!dev.last_valid.settling, "last_valid does not have settling flag");
        check(fabsf(dev.last_valid.lux - 100.0f) < 0.001f, "last_valid.lux preserves old value");
    }

    /* ================================================================
       I: last_attempt after range change
       ================================================================ */
    scenario("I: last_attempt after range change");
    {
        FakeI2cBus fake; I2cBus bus; VEML7700_HandleTypeDef dev; VEML7700_Sample s;
        setup(&fake, &bus, &dev);
        dev.last_valid.valid = true;
        dev.last_valid.lux = 100.0f;

        FakeI2cBus_SetAlsRead(&fake, 10);
        VEML7700_ReadWithAutoRange(&dev, &s);
        VEML7700_ReadWithAutoRange(&dev, &s);
        VEML7700_ReadWithAutoRange(&dev, &s);

        check(dev.last_attempt.range_changed, "last_attempt.range_changed = true");
        check(!dev.last_attempt.valid, "last_attempt.valid = false");
        check(dev.last_attempt.als_raw == 10, "last_attempt.raw = 10");
        check(dev.last_valid.valid, "last_valid still true (unchanged)");
        check(fabsf(dev.last_valid.lux - 10.0f * 0.115200f) < 0.001f, "last_valid preserves last-known valid lux (from #2)");
    }

    /* ================================================================
       J: last_attempt after saturation
       ================================================================ */
    scenario("J: last_attempt after saturation at min range");
    {
        FakeI2cBus fake; I2cBus bus; VEML7700_HandleTypeDef dev; VEML7700_Sample s;
        setup(&fake, &bus, &dev);
        dev.range_index = 0;
        dev.last_valid.valid = true;
        dev.last_valid.lux = 100.0f;

        FakeI2cBus_SetAlsRead(&fake, 64500);
        VEML7700_ReadWithAutoRange(&dev, &s);

        check(dev.last_attempt.saturated, "last_attempt.saturated = true");
        check(!dev.last_attempt.valid, "last_attempt.valid = false");
        check(!dev.last_attempt.range_changed, "last_attempt.range_changed = false");
        check(dev.last_attempt.als_raw == 64500, "last_attempt.raw = 64500");
        check(dev.last_valid.valid, "last_valid preserved");
        check(fabsf(dev.last_valid.lux - 100.0f) < 0.001f, "last_valid maintains old lux");
    }

    /* ================================================================
       K: last_valid not overwritten by invalid / settling
       ================================================================ */
    scenario("K: last_valid not overwritten by invalid attempt");
    {
        FakeI2cBus fake; I2cBus bus; VEML7700_HandleTypeDef dev; VEML7700_Sample s;
        setup(&fake, &bus, &dev);
        FakePlatform_SetTick(0);
        dev.range_state = VEML7700_RANGE_SETTLING;
        dev.settle_start_ms = 0;
        dev.settle_duration_ms = 100;
        dev.last_valid.valid = true;
        dev.last_valid.lux = 42.0f;

        FakePlatform_SetTick(50);
        FakeI2cBus_SetAlsRead(&fake, 999);
        VEML7700_ReadWithAutoRange(&dev, &s);

        check(dev.last_valid.valid, "last_valid still valid");
        check(fabsf(dev.last_valid.lux - 42.0f) < 0.001f, "last_valid lux = 42 (unchanged)");
        check(!dev.last_valid.settling, "last_valid has no settling flag");
        check(dev.last_attempt.als_raw == 999, "last_attempt captures latest raw");
        check(dev.last_attempt.settling, "last_attempt.settling = true");

        /* also verify a later valid sample overwrites both */
        FakePlatform_SetTick(200);
        FakeI2cBus_SetAlsRead(&fake, 1234);
        VEML7700_ReadWithAutoRange(&dev, &s);
        check(s.valid, "post-settling sample valid");
        check(dev.last_valid.valid, "last_valid updated after valid sample");
        check(dev.last_valid.als_raw == 1234, "last_valid raw = 1234");
        check(dev.last_attempt.valid, "last_attempt also updated");
    }

    /* ================================================================
       L: diagnostics never hybrid
       ================================================================ */
    scenario("L: VEML7700_GetDiagnostics returns clean last_attempt");
    {
        FakeI2cBus fake; I2cBus bus; VEML7700_HandleTypeDef dev; VEML7700_Sample s, diag;
        setup(&fake, &bus, &dev);
        FakePlatform_SetTick(0);
        dev.range_state = VEML7700_RANGE_SETTLING;
        dev.settle_start_ms = 0;
        dev.settle_duration_ms = 100;
        dev.last_valid.valid = true;
        dev.last_valid.lux = 42.0f;
        dev.last_valid.als_raw = 42;

        FakePlatform_SetTick(50);
        FakeI2cBus_SetAlsRead(&fake, 999);
        VEML7700_ReadWithAutoRange(&dev, &s);

        VEML7700_GetDiagnostics(&dev, &diag);
        check(!diag.valid, "diagnostics.valid = false (settling)");
        check(diag.settling, "diagnostics.settling = true");
        check(diag.als_raw == 999, "diagnostics.raw = 999 (not 42)");
        check(diag.lux == 0.0f, "diagnostics.lux = 0 (not hybrid from last_valid)");

        /* after valid sample, diagnostics returns valid attempt */
        FakePlatform_SetTick(200);
        FakeI2cBus_SetAlsRead(&fake, 2000);
        VEML7700_ReadWithAutoRange(&dev, &s);
        VEML7700_GetDiagnostics(&dev, &diag);
        check(diag.valid, "diagnostics.valid after good read");
        check(diag.als_raw == 2000, "diagnostics.raw = 2000");
        check(fabsf(diag.lux - 2000.0f * 0.115200f) < 0.001f, "diagnostics.lux computed correctly");
    }

    /* ================================================================
       M: config_error counted once per failure
       ================================================================ */
    scenario("M: config_error counted once per config failure");
    {
        FakeI2cBus fake; I2cBus bus; VEML7700_HandleTypeDef dev;
        FakeI2cBus_Init(&fake);
        fake.write_result = DRIVER_STATUS_BUS_ERROR;  /* make ApplyIndex fail */
        FakeI2cBus_GetBus(&bus, &fake);

        memset(&dev, 0, sizeof(dev));
        dev.bus = &bus;
        dev.persistence = VEML7700_PERS_1;

        /* try Init — ApplyIndex fails due to bus error, config_error must be 1 */
        bool ok = VEML7700_Init(&dev, &bus);
        check(!ok, "VEML7700_Init fails when ApplyIndex fails");
        check(dev.counters.config_error == 1, "config_error = 1 (not 2)");
    }

    /* ================================================================
       Preserve existing tests (regression)
       ================================================================ */
    scenario("Regression: LOW-HIGH-LOW resets hysteresis");
    {
        FakeI2cBus fake; I2cBus bus; VEML7700_HandleTypeDef dev; VEML7700_Sample s;
        setup(&fake, &bus, &dev);
        FakeI2cBus_SetAlsRead(&fake, 10);
        VEML7700_ReadWithAutoRange(&dev, &s);
        FakeI2cBus_SetAlsRead(&fake, 15000);
        VEML7700_ReadWithAutoRange(&dev, &s);
        FakeI2cBus_SetAlsRead(&fake, 10);
        VEML7700_ReadWithAutoRange(&dev, &s);
        check(!s.range_changed, "L-H-L: no range change");
        check(dev.range_index == 4, "L-H-L: index unchanged");
    }

    scenario("Regression: Settling blocks autorange");
    {
        FakeI2cBus fake; I2cBus bus; VEML7700_HandleTypeDef dev; VEML7700_Sample s;
        setup(&fake, &bus, &dev);
        FakePlatform_SetTick(0);
        dev.range_state = VEML7700_RANGE_SETTLING;
        dev.settle_start_ms = 0;
        dev.settle_duration_ms = 100;
        FakePlatform_SetTick(50);
        FakeI2cBus_SetAlsRead(&fake, 10);
        VEML7700_ReadWithAutoRange(&dev, &s);
        check(s.settling, "settling flag true");
        check(dev.range_index == 4, "no autorange during settling");
    }

    scenario("Regression: Settling completion resets hysteresis");
    {
        FakeI2cBus fake; I2cBus bus; VEML7700_HandleTypeDef dev; VEML7700_Sample s;
        setup(&fake, &bus, &dev);
        FakePlatform_SetTick(0);
        dev.range_state = VEML7700_RANGE_SETTLING;
        dev.settle_start_ms = 0;
        dev.settle_duration_ms = 100;
        dev.range_consecutive = 99;
        dev.pending_adjust = VEML7700_ADJUST_MORE;
        FakePlatform_SetTick(200);
        FakeI2cBus_SetAlsRead(&fake, 1000);
        VEML7700_ReadWithAutoRange(&dev, &s);
        check(dev.range_state == VEML7700_RANGE_STABLE, "settling done -> stable");
        check(dev.range_consecutive == 0, "consecutive reset");
        check(dev.pending_adjust == VEML7700_ADJUST_NONE, "direction reset");
    }

    scenario("Regression: Tick wraparound settling");
    {
        FakeI2cBus fake; I2cBus bus; VEML7700_HandleTypeDef dev; VEML7700_Sample s;
        setup(&fake, &bus, &dev);
        FakePlatform_SetTick(0xFFFFFFF0U);
        dev.range_state = VEML7700_RANGE_SETTLING;
        dev.settle_start_ms = 0xFFFFFFF0U;
        dev.settle_duration_ms = 100;
        FakePlatform_SetTick(0x00000010U);
        VEML7700_ReadWithAutoRange(&dev, &s);
        check(s.settling, "before duration: still settling");
        FakePlatform_SetTick(0x000000A0U);
        VEML7700_ReadWithAutoRange(&dev, &s);
        check(dev.range_state == VEML7700_RANGE_STABLE, "after duration: settling done");
    }

    scenario("Regression: Range table monotonic");
    {
        uint8_t n = VEML7700_UT_GetRangeCount();
        check(n == 10, "range count == 10");
        float prev = 0.0f;
        for (uint8_t i = 0; i < n; i++)
        {
            float s = VEML7700_UT_GetRangeSens(i);
            char buf[128];
            sprintf(buf, "range[%u] sens=%.2f > prev=%.2f", (unsigned)i, (double)s, (double)prev);
            check(s > prev, buf);
            prev = s;
        }
    }

    /* ================================================================
       Summary
       ================================================================ */
    printf("\n=== Summary ===\n");
    printf("  Scenarios: %d\n", s_scenario);
    printf("  Assertions: %d\n", (s_pass + s_fail));
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);

    return s_fail > 0 ? 1 : 0;
}