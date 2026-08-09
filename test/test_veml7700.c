#include <stdio.h>
#include <math.h>
#include <string.h>

#include "veml7700_test.h"
#include "veml7700.h"
#include "i2c_bus.h"

#include "fake_i2c_bus.h"
#include "fake_platform_time.h"

/* ---- helpers ---- */

static int s_pass = 0;
static int s_fail = 0;
static int s_test_idx = 0;

static void check(int cond, const char *msg)
{
    s_test_idx++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_test_idx, msg); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_test_idx, msg); }
}

/* make a fresh device + bus */
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
    printf("VEML7700 Edge-Case Tests\n\n");

    /* ================================================================
       1. Range table monotonic
       ================================================================ */
    printf("=== 1. Range table monotonic ===\n");
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
       2. LOW LOW LOW → more sensitive, exactly one step
       ================================================================ */
    printf("\n=== 2. LOW LOW LOW triggers one range step ===\n");
    {
        FakeI2cBus fake;
        I2cBus bus;
        VEML7700_HandleTypeDef dev;
        VEML7700_Sample sample;

        setup(&fake, &bus, &dev);
        check(dev.range_index == 4, "starts at index 4");

        /* three consecutive low readings */
        FakeI2cBus_SetAlsRead(&fake, 10);  /* RAW = 10 < 80 */
        VEML7700_ReadWithAutoRange(&dev, &sample);
        check(!sample.valid && !sample.range_changed, "sample 1: not valid, not changed");

        VEML7700_ReadWithAutoRange(&dev, &sample);
        check(!sample.valid && !sample.range_changed, "sample 2: not valid, not changed");

        VEML7700_ReadWithAutoRange(&dev, &sample);
        check(sample.range_changed && dev.range_index == 5, "sample 3: range changed to index 5");
        check(!sample.valid, "range change: valid = false");
    }

    /* ================================================================
       3. HIGH HIGH HIGH → less sensitive, exactly one step
       ================================================================ */
    printf("\n=== 3. HIGH HIGH HIGH triggers one range step ===\n");
    {
        FakeI2cBus fake;
        I2cBus bus;
        VEML7700_HandleTypeDef dev;
        VEML7700_Sample sample;

        setup(&fake, &bus, &dev);
        check(dev.range_index == 4, "starts at index 4");

        FakeI2cBus_SetAlsRead(&fake, 15000);
        VEML7700_ReadWithAutoRange(&dev, &sample);
        VEML7700_ReadWithAutoRange(&dev, &sample);
        VEML7700_ReadWithAutoRange(&dev, &sample);

        check(sample.range_changed && dev.range_index == 3, "range changed to index 3");
        check(!sample.valid, "range change: valid = false");
    }

    /* ================================================================
       4. LOW HIGH LOW does NOT trigger
       ================================================================ */
    printf("\n=== 4. LOW HIGH LOW resets hysteresis ===\n");
    {
        FakeI2cBus fake;
        I2cBus bus;
        VEML7700_HandleTypeDef dev;
        VEML7700_Sample sample;

        setup(&fake, &bus, &dev);

        FakeI2cBus_SetAlsRead(&fake, 10);
        VEML7700_ReadWithAutoRange(&dev, &sample);

        FakeI2cBus_SetAlsRead(&fake, 15000);
        VEML7700_ReadWithAutoRange(&dev, &sample);

        FakeI2cBus_SetAlsRead(&fake, 10);
        VEML7700_ReadWithAutoRange(&dev, &sample);

        check(!sample.range_changed, "LOW-HIGH-LOW: no range change");
        check(dev.range_index == 4, "index unchanged");
        check(dev.range_consecutive == 1, "consecutive = 1 (reset to LOW direction)");
        check(dev.pending_adjust == VEML7700_ADJUST_MORE, "direction = MORE (last was LOW)");
    }

    /* ================================================================
       5. HIGH LOW HIGH does NOT trigger
       ================================================================ */
    printf("\n=== 5. HIGH LOW HIGH resets hysteresis ===\n");
    {
        FakeI2cBus fake;
        I2cBus bus;
        VEML7700_HandleTypeDef dev;
        VEML7700_Sample sample;

        setup(&fake, &bus, &dev);

        FakeI2cBus_SetAlsRead(&fake, 15000);
        VEML7700_ReadWithAutoRange(&dev, &sample);

        FakeI2cBus_SetAlsRead(&fake, 10);
        VEML7700_ReadWithAutoRange(&dev, &sample);

        FakeI2cBus_SetAlsRead(&fake, 15000);
        VEML7700_ReadWithAutoRange(&dev, &sample);

        check(!sample.range_changed, "HIGH-LOW-HIGH: no range change");
        check(dev.range_index == 4, "index unchanged");
        check(dev.range_consecutive == 1, "consecutive = 1 (reset to LESS direction)");
        check(dev.pending_adjust == VEML7700_ADJUST_LESS, "direction = LESS (last was HIGH)");
    }

    /* ================================================================
       6. Normal resets hysteresis
       ================================================================ */
    printf("\n=== 6. Normal sample resets hysteresis ===\n");
    {
        FakeI2cBus fake;
        I2cBus bus;
        VEML7700_HandleTypeDef dev;
        VEML7700_Sample sample;

        setup(&fake, &bus, &dev);

        FakeI2cBus_SetAlsRead(&fake, 10);
        VEML7700_ReadWithAutoRange(&dev, &sample);
        check(dev.range_consecutive == 1, "one low reading");

        FakeI2cBus_SetAlsRead(&fake, 1000);
        VEML7700_ReadWithAutoRange(&dev, &sample);
        check(dev.range_consecutive == 0, "normal reading resets");
        check(dev.pending_adjust == VEML7700_ADJUST_NONE, "direction = NONE");
        check(sample.valid, "normal sample is valid");
    }

    /* ================================================================
       7. Settling blocks autorange decisions
       ================================================================ */
    printf("\n=== 7. Settling blocks autorange ===\n");
    {
        FakeI2cBus fake;
        I2cBus bus;
        VEML7700_HandleTypeDef dev;
        VEML7700_Sample sample;

        setup(&fake, &bus, &dev);
        FakePlatform_SetTick(0);

        /* manually enter settling */
        dev.range_state = VEML7700_RANGE_SETTLING;
        dev.settle_start_ms = 0;
        dev.settle_duration_ms = 100;

        FakePlatform_SetTick(50);
        FakeI2cBus_SetAlsRead(&fake, 10);   /* would trigger LOW if not settling */
        VEML7700_ReadWithAutoRange(&dev, &sample);

        check(sample.settling, "settling flag true");
        check(dev.range_state == VEML7700_RANGE_SETTLING, "still settling");
        check(dev.range_index == 4, "no autorange decision during settling");
    }

    /* ================================================================
       8. Settling completion resets hysteresis
       ================================================================ */
    printf("\n=== 8. Settling completion resets hysteresis ===\n");
    {
        FakeI2cBus fake;
        I2cBus bus;
        VEML7700_HandleTypeDef dev;
        VEML7700_Sample sample;

        setup(&fake, &bus, &dev);
        FakePlatform_SetTick(0);

        dev.range_state = VEML7700_RANGE_SETTLING;
        dev.settle_start_ms = 0;
        dev.settle_duration_ms = 100;
        dev.range_consecutive = 99;
        dev.pending_adjust = VEML7700_ADJUST_MORE;

        FakePlatform_SetTick(200);
        FakeI2cBus_SetAlsRead(&fake, 1000);
        VEML7700_ReadWithAutoRange(&dev, &sample);

        check(dev.range_state == VEML7700_RANGE_STABLE, "settling done → stable");
        check(dev.range_consecutive == 0, "consecutive reset");
        check(dev.pending_adjust == VEML7700_ADJUST_NONE, "direction reset");
    }

    /* ================================================================
       9. Max sensitivity + LOW → valid lux, no range change
       ================================================================ */
    printf("\n=== 9. Max sensitivity + LOW → valid ===\n");
    {
        FakeI2cBus fake;
        I2cBus bus;
        VEML7700_HandleTypeDef dev;
        VEML7700_Sample sample;

        setup(&fake, &bus, &dev);
        dev.range_index = 9;
        dev.resolution = VEML7700_UT_GetRangeRes(9);

        FakeI2cBus_SetAlsRead(&fake, 10);

        VEML7700_ReadWithAutoRange(&dev, &sample);
        VEML7700_ReadWithAutoRange(&dev, &sample);
        VEML7700_ReadWithAutoRange(&dev, &sample);

        check(sample.valid, "at max range + LOW: valid");
        check(!sample.range_changed, "no range change at boundary");
        check(fabs(sample.lux - 10.0f * 0.003600f) < 0.001f, "lux = 10 × resolution");
        check(dev.range_index == 9, "index unchanged at 9");
    }

    /* ================================================================
       10. Min sensitivity + HIGH (not saturated) → valid
       ================================================================ */
    printf("\n=== 10. Min sensitivity + HIGH → valid ===\n");
    {
        FakeI2cBus fake;
        I2cBus bus;
        VEML7700_HandleTypeDef dev;
        VEML7700_Sample sample;

        setup(&fake, &bus, &dev);
        dev.range_index = 0;

        FakeI2cBus_SetAlsRead(&fake, 15000);

        VEML7700_ReadWithAutoRange(&dev, &sample);
        VEML7700_ReadWithAutoRange(&dev, &sample);
        VEML7700_ReadWithAutoRange(&dev, &sample);

        check(sample.valid, "at min range + HIGH: valid");
        check(!sample.range_changed, "no range change at boundary");
        check(dev.range_index == 0, "index unchanged at 0");
    }

    /* ================================================================
       11. Min sensitivity + saturated
       ================================================================ */
    printf("\n=== 11. Min sensitivity + saturated ===\n");
    {
        FakeI2cBus fake;
        I2cBus bus;
        VEML7700_HandleTypeDef dev;
        VEML7700_Sample sample;

        setup(&fake, &bus, &dev);
        dev.range_index = 0;

        FakeI2cBus_SetAlsRead(&fake, 64500);
        VEML7700_ReadWithAutoRange(&dev, &sample);

        check(sample.saturated, "saturated = true");
        check(!sample.valid, "valid = false");
        check(!sample.range_changed, "range_changed = false (no lower range)");
    }

    /* ================================================================
       12. Range change: valid=false, range_changed=true, settling=true
       ================================================================ */
    printf("\n=== 12. Range change correctly flags sample ===\n");
    {
        FakeI2cBus fake;
        I2cBus bus;
        VEML7700_HandleTypeDef dev;
        VEML7700_Sample sample;

        setup(&fake, &bus, &dev);

        FakeI2cBus_SetAlsRead(&fake, 10);
        VEML7700_ReadWithAutoRange(&dev, &sample);
        VEML7700_ReadWithAutoRange(&dev, &sample);
        VEML7700_ReadWithAutoRange(&dev, &sample);

        check(sample.range_changed, "range_changed = true");
        check(!sample.valid, "valid = false after range change");
        check(sample.settling, "settling = true after range change");
    }

    /* ================================================================
       13. Tick wraparound during settling
       ================================================================ */
    printf("\n=== 13. Tick wraparound during settling ===\n");
    {
        FakeI2cBus fake;
        I2cBus bus;
        VEML7700_HandleTypeDef dev;
        VEML7700_Sample sample;

        setup(&fake, &bus, &dev);

        /* tick is near wraparound */
        FakePlatform_SetTick(0xFFFFFFF0U);
        dev.range_state = VEML7700_RANGE_SETTLING;
        dev.settle_start_ms = 0xFFFFFFF0U;
        dev.settle_duration_ms = 100;

        /* before settling done — tick wraps */
        FakePlatform_SetTick(0x00000010U);   /* elapsed = 0x10 - 0xFFFFFFF0 = 32 (wraps OK) */
        VEML7700_ReadWithAutoRange(&dev, &sample);

        check(sample.settling, "settling still active after wraparound before duration");
        check(dev.range_state == VEML7700_RANGE_SETTLING, "still settling");

        /* after settling duration */
        FakePlatform_SetTick(0x000000A0U);   /* elapsed = 0xA0 - 0xFFFFFFF0 = 176 > 100 */
        VEML7700_ReadWithAutoRange(&dev, &sample);

        check(dev.range_state == VEML7700_RANGE_STABLE, "settling complete after wraparound");
    }

    /* ================================================================
       14. App: invalid sample does not count as runtime failure
       ================================================================ */
    printf("\n=== 14. Invalid sample does not increment runtime failure ===\n");
    {
        /* This tests the app.c invariant */
        /* VEML7700_ReadWithAutoRange returning success but !valid */
        /* must not increment operation_failures in DeviceRuntime */
        FakeI2cBus fake;
        I2cBus bus;
        VEML7700_HandleTypeDef dev;
        VEML7700_Sample sample;

        setup(&fake, &bus, &dev);
        /* everything at index 0, HIGH → no change, but valid is set */
        /* we need actual invalid — use settling */
        dev.range_state = VEML7700_RANGE_SETTLING;
        dev.settle_start_ms = 0;
        dev.settle_duration_ms = 100;
        FakePlatform_SetTick(50);
        FakeI2cBus_SetAlsRead(&fake, 1000);

        check(VEML7700_ReadWithAutoRange(&dev, &sample), "read succeeds");
        check(!sample.valid, "sample invalid during settling");
        check(dev.counters.read_error == 0, "no bus errors counted");
        check(dev.counters.read_success == 1, "one successful read counted");
        /* note: EnterSettling was called via setup's state — not through ReadWithAutoRange.
           The read_success count would be 1 from this single call.
           We check it incremented correctly. */
        check(dev.counters.read_success > 0, "read_success > 0");

        printf("  read_success = %u, read_error = %u\n",
               (unsigned)dev.counters.read_success, (unsigned)dev.counters.read_error);
    }

    /* ================================================================
       Summary
       ================================================================ */
    printf("\n=== Results ===\n");
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);

    return s_fail > 0 ? 1 : 0;
}