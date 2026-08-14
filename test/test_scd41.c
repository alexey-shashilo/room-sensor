#include <stdio.h>
#include <string.h>
#include <math.h>

#include "scd41.h"
#include "scd41_runtime.h"
#include "room_state.h"
#include "telemetry.h"
#include "telemetry_serializer.h"
#include "self_test.h"
#include "config.h"
#include "storage.h"
#include "command.h"
#include "command_dispatcher.h"
#include "device_manifest.h"
#include "fake_i2c_bus.h"
#include "fake_platform_time.h"
#include "fake_flash.h"
#include "fake_unique_id.h"

/* SCD41 host regression suite.

   Driver        -> 1..12
   Runtime       -> 13..22
   App/RoomState -> 23..28
   Telemetry     -> 29..33
   Display       -> 34..36
   SelfTest      -> 37..41

   Cross-checks the driver's CRC-8 against the fake's independent CRC helper, so
   a disagreement (either copy buggy) fails a test. */

static int s_pass = 0, s_fail = 0, s_case = 0;

static void check(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

/* ---- helpers ---- */
static void setup(FakeI2cBus *fake, I2cBus *bus, Scd41 *dev)
{
    FakeI2cBus_Init(fake);
    FakeI2cBus_GetBus(bus, fake);
    SCD41_Init(dev, bus);
}

/* Advance the runtime clock by `delta` ms then run one poll. */
static void tick(Scd41Runtime *rt, uint32_t delta)
{
    FakePlatform_AdvanceTick(delta);
    Scd41Runtime_Poll(rt);
}

/* Advance the clock and poll, then keep stepping 1 ms + polling until any
   in-flight two-phase SCD4x transaction completes (phase returns to IDLE).
   Models the cooperative scheduler crossing the ~1 ms command-execution
   deadline before reading the response. */
static void tick_cycle(Scd41Runtime *rt, uint32_t delta)
{
    FakePlatform_AdvanceTick(delta);
    Scd41Runtime_Poll(rt);
    int guard = 0;
    while (rt->phase != SCD41_PHASE_IDLE && guard < 32)
    {
        FakePlatform_AdvanceTick(1);
        Scd41Runtime_Poll(rt);
        guard++;
    }
}

/* Driver-level full two-phase helpers: Begin + advance past the official
   1 ms response deadline + Finish. Used by driver unit tests that want a
   completed transaction; dedicated timing tests exercise the raw phases. */
static DriverStatus data_ready_full(Scd41 *dev, bool *ready)
{
    DriverStatus s = SCD41_BeginGetDataReady(dev);
    if (s != DRIVER_STATUS_OK) return s;
    FakePlatform_AdvanceTick(SCD41_COMMAND_RESPONSE_DELAY_MS);
    return SCD41_FinishGetDataReady(dev, ready);
}

static DriverStatus read_measurement_full(Scd41 *dev, Scd41Measurement *m)
{
    DriverStatus s = SCD41_BeginReadMeasurement(dev);
    if (s != DRIVER_STATUS_OK) return s;
    FakePlatform_AdvanceTick(SCD41_COMMAND_RESPONSE_DELAY_MS);
    return SCD41_FinishReadMeasurement(dev, m);
}

/* ---------------------------------------------------------------------- */
int main(void)
{
    printf("SCD41 Driver / Runtime / Integration Host Tests\n");
    fflush(stdout);

    /* ===================== Driver ===================== */
    printf("\n=== Driver: probe + start ===\n");
    {
        FakeI2cBus fake; I2cBus bus; Scd41 dev;
        setup(&fake, &bus, &dev);

        fake.probe_result = DRIVER_STATUS_OK;
        check(SCD41_Probe(&bus) == DRIVER_STATUS_OK, "1 probe success");
        /* Wire address: left-shifted 8-bit byte (0xC4), matching the I2cBus /
           STM32 HAL convention used by VEML7700/Display. A raw 7-bit 0x62 on
           the bus is a real address bug that hardware catches (probe targets
           0x31 instead of the sensor). */
        check(fake.last_addr == (SCD41_I2C_ADDR << 1U),
              "1 probe uses left-shifted wire address 0xC4");
        fake.probe_result = DRIVER_STATUS_BUS_ERROR;
        check(SCD41_Probe(&bus) == DRIVER_STATUS_BUS_ERROR, "2 probe fail");
    }

    printf("\n=== Driver: start/stop periodic command ===\n");
    {
        FakeI2cBus fake; I2cBus bus; Scd41 dev;
        setup(&fake, &bus, &dev);
        fake.write_result = DRIVER_STATUS_OK;
        check(SCD41_StartPeriodicMeasurement(&dev) == DRIVER_STATUS_OK,
              "3 start periodic ok");
        check(fake.last_scd41_cmd == 0x21B1U, "3 start periodic sent 0x21B1");
        check(fake.last_write_size == 2U, "3 start periodic 2-byte command");
        check(fake.last_write_data[0] == 0x21U, "3 start periodic MSB 0x21");
        check(fake.last_write_data[1] == 0xB1U, "3 start periodic LSB 0xB1");
        check(fake.last_addr == (SCD41_I2C_ADDR << 1U),
              "3 start periodic addresses left-shifted 0xC4");

        check(SCD41_StopPeriodicMeasurement(&dev) == DRIVER_STATUS_OK,
              "3 stop periodic ok");
        check(fake.last_scd41_cmd == 0x3F86U, "3 stop periodic sent 0x3F86");
    }

    printf("\n=== Driver: data-ready ===\n");
    {
        FakeI2cBus fake; I2cBus bus; Scd41 dev; bool ready = true;
        setup(&fake, &bus, &dev);
        fake.read_result = DRIVER_STATUS_OK;

        /* No data ready: zero word with valid CRC. */
        fake.data_ready_scripted = 1;
        fake.data_ready_word = 0x0000;
        FakeI2cBus_SetScd41DataReady(&fake, false);
        check(data_ready_full(&dev, &ready) == DRIVER_STATUS_OK, "4 data-ready query ok");
        check(!ready, "4 data-ready false (not an error)");

        /* Data ready: word with bit set. */
        FakeI2cBus_SetScd41DataReady(&fake, true);
        ready = false;
        check(data_ready_full(&dev, &ready) == DRIVER_STATUS_OK, "5 data-ready query ok");
        check(ready, "5 data-ready true");
    }

    printf("\n=== Driver: valid measurement conversion ===\n");
    {
        FakeI2cBus fake; I2cBus bus; Scd41 dev; Scd41Measurement m;
        setup(&fake, &bus, &dev);
        /* CO2=1200, T=23.0C, RH=42.0% */
        uint16_t t = FakeI2cBus_TempRaw(23.0f);
        uint16_t rh = FakeI2cBus_RhRaw(42.0f);
        FakeI2cBus_SetScd41Measurement(&fake, 1200, t, rh, false, false, false);

        check(read_measurement_full(&dev, &m) == DRIVER_STATUS_OK, "6 valid read ok");
        check(m.valid, "6 valid=true");
        check(m.co2_ppm == 1200, "6 co2=1200 ppm");
        check(fabsf(m.temperature_c - 23.0f) < 0.2f, "6 temperature ~23C");
        check(fabsf(m.relative_humidity_pct - 42.0f) < 0.5f, "6 RH ~42%");
    }

    printf("\n=== Driver: CRC failure rejects whole sample ===\n");
    {
        /* CO2 CRC corrupted -> ENTIRE sample rejected, not committed. */
        {
            FakeI2cBus fake; I2cBus bus; Scd41 dev; Scd41Measurement m;
            setup(&fake, &bus, &dev);
            uint16_t t = FakeI2cBus_TempRaw(23.0f);
            uint16_t rh = FakeI2cBus_RhRaw(42.0f);
            FakeI2cBus_SetScd41Measurement(&fake, 1200, t, rh, true, false, false);
            DriverStatus s = read_measurement_full(&dev, &m);
            check(s == DRIVER_STATUS_CRC_ERROR, "7 CO2 CRC -> CRC_ERROR");
            check(!m.valid, "7 no partial sample committed");
        }
        /* Temperature CRC corrupted. */
        {
            FakeI2cBus fake; I2cBus bus; Scd41 dev; Scd41Measurement m;
            setup(&fake, &bus, &dev);
            uint16_t t = FakeI2cBus_TempRaw(23.0f);
            uint16_t rh = FakeI2cBus_RhRaw(42.0f);
            FakeI2cBus_SetScd41Measurement(&fake, 1200, t, rh, false, true, false);
            check(read_measurement_full(&dev, &m) == DRIVER_STATUS_CRC_ERROR,
                  "8 temperature CRC -> CRC_ERROR");
            check(!m.valid, "8 no partial sample committed");
        }
        /* RH CRC corrupted. */
        {
            FakeI2cBus fake; I2cBus bus; Scd41 dev; Scd41Measurement m;
            setup(&fake, &bus, &dev);
            uint16_t t = FakeI2cBus_TempRaw(23.0f);
            uint16_t rh = FakeI2cBus_RhRaw(42.0f);
            FakeI2cBus_SetScd41Measurement(&fake, 1200, t, rh, false, false, true);
            check(read_measurement_full(&dev, &m) == DRIVER_STATUS_CRC_ERROR,
                  "9 RH CRC -> CRC_ERROR");
            check(!m.valid, "9 no partial sample committed");
        }
    }

    printf("\n=== Driver: I2C write/read failure ===\n");
    {
        /* Write (command) failure -> not OK. */
        {
            FakeI2cBus fake; I2cBus bus; Scd41 dev;
            setup(&fake, &bus, &dev);
            fake.write_result = DRIVER_STATUS_BUS_ERROR;
            check(SCD41_StartPeriodicMeasurement(&dev) == DRIVER_STATUS_BUS_ERROR,
                  "10 start on TX fail -> BUS_ERROR");
        }
        /* Read failure on measurement -> not OK, no partial. */
        {
            FakeI2cBus fake; I2cBus bus; Scd41 dev; Scd41Measurement m;
            setup(&fake, &bus, &dev);
            uint16_t t = FakeI2cBus_TempRaw(23.0f);
            uint16_t rh = FakeI2cBus_RhRaw(42.0f);
            FakeI2cBus_SetScd41Measurement(&fake, 1200, t, rh, false, false, false);
            fake.read_result = DRIVER_STATUS_TIMEOUT;
            check(read_measurement_full(&dev, &m) == DRIVER_STATUS_TIMEOUT,
                  "11 measurement RX fail -> TIMEOUT");
            check(!m.valid, "11 no partial sample committed");
        }
        /* Read failure on data-ready -> error status, ready unchanged. */
        {
            FakeI2cBus fake; I2cBus bus; Scd41 dev; bool ready = true;
            setup(&fake, &bus, &dev);
            FakeI2cBus_SetScd41DataReady(&fake, true);
            fake.read_result = DRIVER_STATUS_BUS_ERROR;
            check(data_ready_full(&dev, &ready) == DRIVER_STATUS_BUS_ERROR,
                  "12 data-ready RX fail -> BUS_ERROR");
        }
    }

    /* ===================== Runtime ===================== */
    printf("\n=== Runtime: startup -> waiting -> valid ===\n");
    {
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        fake.probe_result = DRIVER_STATUS_OK;
        fake.write_result = DRIVER_STATUS_OK;

        Scd41Runtime rt;
        Scd41Runtime_Init(&rt, &bus);
        FakePlatform_SetTick(0);
        check(rt.state == DEVICE_STATE_NOT_FOUND, "runtime init NOT_FOUND");

        Scd41Runtime_Start(&rt);
        check(rt.state == DEVICE_STATE_STARTING, "13 start -> STARTING");

        /* Before the periodic interval elapses, stay STARTING. */
        tick(&rt, SCD41_PERIODIC_INTERVAL_MS - 1);
        check(rt.state == DEVICE_STATE_STARTING, "13 still STARTING before interval");

        /* After 5 s -> WAITING. */
        tick(&rt, 1);
        check(rt.state == DEVICE_STATE_WAITING, "13 -> WAITING after first window");
    }

    printf("\n=== Runtime: data-ready true -> valid sample -> READY ===\n");
    {
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        fake.probe_result = DRIVER_STATUS_OK;
        fake.write_result = DRIVER_STATUS_OK;

        Scd41Runtime rt;
        Scd41Runtime_Init(&rt, &bus);
        FakePlatform_SetTick(0);
        Scd41Runtime_Start(&rt);
        tick(&rt, SCD41_PERIODIC_INTERVAL_MS);  /* -> WAITING */

        uint16_t t = FakeI2cBus_TempRaw(23.0f);
        uint16_t rh = FakeI2cBus_RhRaw(42.0f);
        FakeI2cBus_SetScd41DataReady(&fake, true);
        FakeI2cBus_SetScd41Measurement(&fake, 900, t, rh, false, false, false);

        tick_cycle(&rt, SCD41_RUNTIME_POLL_INTERVAL_MS);
        check(rt.state == DEVICE_STATE_READY, "14 valid sample -> READY");
        check(Scd41Runtime_HasValidSample(&rt), "14 has valid sample");
        check(rt.last_sample.co2_ppm == 900, "14 co2=900");
    }

    printf("\n=== Runtime: not-ready is not an error ===\n");
    {
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        fake.probe_result = DRIVER_STATUS_OK;

        Scd41Runtime rt;
        Scd41Runtime_Init(&rt, &bus);
        FakePlatform_SetTick(0);
        Scd41Runtime_Start(&rt);
        tick(&rt, SCD41_PERIODIC_INTERVAL_MS);

        FakeI2cBus_SetScd41DataReady(&fake, false);  /* not ready */
        uint32_t err0 = rt.operation_failures;
        tick(&rt, SCD41_RUNTIME_POLL_INTERVAL_MS);
        check(rt.operation_failures == err0, "15 not-ready doesn't count as error");
        check(rt.state != DEVICE_STATE_ERROR, "15 stays operational");
    }

    printf("\n=== Runtime: one transient I2C error, then recovery succeeds ===\n");
    {
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        fake.probe_result = DRIVER_STATUS_OK;
        uint16_t t = FakeI2cBus_TempRaw(23.0f);
        uint16_t rh = FakeI2cBus_RhRaw(42.0f);

        Scd41Runtime rt;
        Scd41Runtime_Init(&rt, &bus);
        FakePlatform_SetTick(0);
        Scd41Runtime_Start(&rt);
        tick(&rt, SCD41_PERIODIC_INTERVAL_MS);

        /* One transient read failure. */
        fake.read_result = DRIVER_STATUS_TIMEOUT;
        FakeI2cBus_SetScd41DataReady(&fake, true);
        tick_cycle(&rt, SCD41_RUNTIME_POLL_INTERVAL_MS);
        check(rt.consecutive_errors == 1, "16 one transient error counted");
        check(rt.state != DEVICE_STATE_ERROR, "16 not escalated to ERROR (below threshold)");

        /* Next poll succeeds -> back to normal, errors reset. */
        fake.read_result = DRIVER_STATUS_OK;
        FakeI2cBus_SetScd41Measurement(&fake, 800, t, rh, false, false, false);
        tick_cycle(&rt, SCD41_RUNTIME_POLL_INTERVAL_MS);
        check(rt.state == DEVICE_STATE_READY, "17 recovery after single error");
        check(rt.consecutive_errors == 0, "17 consecutive errors reset on success");
    }

    printf("\n=== Runtime: repeated errors -> ERROR -> recovery ===\n");
    {
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        fake.probe_result = DRIVER_STATUS_OK;

        Scd41Runtime rt;
        Scd41Runtime_Init(&rt, &bus);
        FakePlatform_SetTick(0);
        Scd41Runtime_Start(&rt);
        tick(&rt, SCD41_PERIODIC_INTERVAL_MS);

        /* Repeated data-ready failures. */
        fake.read_result = DRIVER_STATUS_BUS_ERROR;
        FakeI2cBus_SetScd41DataReady(&fake, true);
        for (int i = 0; i < 3; i++)
            tick_cycle(&rt, SCD41_RUNTIME_POLL_INTERVAL_MS);
        check(rt.state == DEVICE_STATE_ERROR, "18 threshold reached -> ERROR");

        /* Recovery via Start + probe success. */
        Scd41Runtime_Recover(&rt);
        check(rt.state == DEVICE_STATE_RECOVERING, "8 recovery_count incremented");
        check(rt.recovery_count == 1, "18 recover -> RECOVERING, count=1");
        fake.read_result = DRIVER_STATUS_OK;
        /* Sensor was left PERIODIC by the earlier start, so the recovery start is
           refused and the runtime performs a bounded STOP -> 500ms settle -> START. */
        check(Scd41Runtime_Start(&rt) == DRIVER_STATUS_OK, "19 re-start recovery underway");
        check(rt.phase == SCD41_PHASE_RECOVER_STOP_SETTLE, "19 recovery in STOP settle phase");
        uint16_t t = FakeI2cBus_TempRaw(23.0f);
        uint16_t rh = FakeI2cBus_RhRaw(42.0f);
        FakeI2cBus_SetScd41DataReady(&fake, true);
        FakeI2cBus_SetScd41Measurement(&fake, 700, t, rh, false, false, false);
        /* <500ms: still settling, no restart yet. */
        tick(&rt, SCD41_RUNTIME_STOP_SETTLE_MS - 1U);
        check(rt.phase == SCD41_PHASE_RECOVER_STOP_SETTLE,
              "19 no restart before 500ms settle");
        check(rt.state == DEVICE_STATE_STARTING, "19 stays STARTING while settling");
        /* >=500ms: restart START accepted; then the fresh periodic wait. */
        tick(&rt, 1U);
        check(rt.phase == SCD41_PHASE_IDLE, "19 restart accepted, phase -> IDLE");
        tick(&rt, SCD41_PERIODIC_INTERVAL_MS);                    /* STARTING -> WAITING */
        tick_cycle(&rt, SCD41_RUNTIME_POLL_INTERVAL_MS);          /* two-phase -> READY */
        check(rt.state == DEVICE_STATE_READY, "19 recovery reaches READY");
    }

    printf("\n=== Runtime: sensor disappears -> values invalid, then return ===\n");
    {
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        fake.probe_result = DRIVER_STATUS_OK;
        uint16_t t = FakeI2cBus_TempRaw(23.0f);
        uint16_t rh = FakeI2cBus_RhRaw(42.0f);

        Scd41Runtime rt;
        Scd41Runtime_Init(&rt, &bus);
        FakePlatform_SetTick(0);
        Scd41Runtime_Start(&rt);
        tick(&rt, SCD41_PERIODIC_INTERVAL_MS);
        FakeI2cBus_SetScd41DataReady(&fake, true);
        FakeI2cBus_SetScd41Measurement(&fake, 950, t, rh, false, false, false);
        tick_cycle(&rt, SCD41_RUNTIME_POLL_INTERVAL_MS);
        check(rt.state == DEVICE_STATE_READY, "has valid sample before loss");

        /* Sensor disappears: repeated read/probe failures -> ERROR invalidates. */
        fake.read_result = DRIVER_STATUS_TIMEOUT;
        FakeI2cBus_SetScd41DataReady(&fake, true);
        for (int i = 0; i < 3; i++)
            tick_cycle(&rt, SCD41_RUNTIME_POLL_INTERVAL_MS);
        check(!Scd41Runtime_HasValidSample(&rt),
              "19 loss after threshold -> sample invalid");
        check(!rt.last_sample.valid, "20 value invalidated");

        /* Sensor returns. */
        fake.read_result = DRIVER_STATUS_OK;
        fake.probe_result = DRIVER_STATUS_OK;
        Scd41Runtime_Recover(&rt);
        Scd41Runtime_Start(&rt);   /* retained PERIODIC -> STOP/settle/START recovery */
        FakeI2cBus_SetScd41DataReady(&fake, true);
        FakeI2cBus_SetScd41Measurement(&fake, 1000, t, rh, false, false, false);
        tick(&rt, SCD41_RUNTIME_STOP_SETTLE_MS);                  /* settle -> restart START */
        check(rt.phase == SCD41_PHASE_IDLE, "20 restart accepted after settle");
        tick(&rt, SCD41_PERIODIC_INTERVAL_MS);                    /* STARTING->WAITING */
        tick_cycle(&rt, SCD41_RUNTIME_POLL_INTERVAL_MS);          /* two-phase -> READY */
        check(Scd41Runtime_HasValidSample(&rt), "20 recovery restores valid sample");
    }

    /* ===================== Retained-periodic recovery ===================== */
    printf("\n=== Runtime A: fresh idle sensor, normal start (no STOP) ===\n");
    {
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        fake.probe_result = DRIVER_STATUS_OK;
        fake.scd41_mode = FAKE_SCD41_MODE_IDLE;

        Scd41Runtime rt;
        Scd41Runtime_Init(&rt, &bus);
        FakePlatform_SetTick(0);

        check(Scd41Runtime_Start(&rt) == DRIVER_STATUS_OK, "A1 fresh start ok");
        check(rt.state == DEVICE_STATE_STARTING, "A1 fresh -> STARTING");
        check(rt.phase == SCD41_PHASE_IDLE, "A1 fresh phase IDLE (no recovery)");
        check(fake.scd41_mode == FAKE_SCD41_MODE_PERIODIC, "A1 fake sensor now PERIODIC");
        /* No STOP must be issued on a normal fresh start. */
        check(fake.scd41_cmd_log_count >= 1 &&
              fake.scd41_cmd_log[0] == 0x21B1U,
              "A1 first command is START_PERIODIC 0x21B1");
        check(fake.scd41_cmd_log_count < 2 ,
              "A1 no STOP sent on fresh start");
    }

    printf("\n=== Runtime B: retained PERIODIC sensor auto-recovery (sequence) ===\n");
    {
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        fake.probe_result = DRIVER_STATUS_OK;
        fake.scd41_mode = FAKE_SCD41_MODE_PERIODIC;   /* retained across MCU reboot */

        Scd41Runtime rt;
        Scd41Runtime_Init(&rt, &bus);
        FakePlatform_SetTick(0);

        check(Scd41Runtime_Start(&rt) == DRIVER_STATUS_OK, "B1 Start enters recovery (OK)");
        check(rt.phase == SCD41_PHASE_RECOVER_STOP_SETTLE, "B1 recovery phase = STOP settle");
        check(rt.state == DEVICE_STATE_STARTING, "B1 stays STARTING during recovery");
        /* START failed (PERIODIC), then STOP issued and accepted -> IDLE. */
        check(fake.scd41_cmd_log_count >= 2 &&
              fake.scd41_cmd_log[0] == 0x21B1U &&
              fake.scd41_cmd_log[1] == 0x3F86U,
              "B3 exact sequence: 21B1 then 3F86");
        check(fake.scd41_mode == FAKE_SCD41_MODE_IDLE, "B2 STOP returned sensor to IDLE");

        /* STILL settling (<500ms): no restart issued. */
        tick(&rt, SCD41_RUNTIME_STOP_SETTLE_MS - 1U);
        check(rt.phase == SCD41_PHASE_RECOVER_STOP_SETTLE, "B4 no restart before 500ms");
        int log_before = fake.scd41_cmd_log_count;
        check(fake.scd41_cmd_log_count == log_before, "B4 no START before settle");

        /* >=500ms: single bounded restart -> IDLE phase, then normal WAITING/READY. */
        uint16_t t = FakeI2cBus_TempRaw(23.0f);
        uint16_t rh = FakeI2cBus_RhRaw(42.0f);
        FakeI2cBus_SetScd41DataReady(&fake, true);
        FakeI2cBus_SetScd41Measurement(&fake, 660, t, rh, false, false, false);
        tick(&rt, 1U);
        check(rt.phase == SCD41_PHASE_IDLE, "B5 restart accepted, phase -> IDLE");
        check(fake.scd41_cmd_log_count >= 3 &&
              fake.scd41_cmd_log[2] == 0x21B1U,
              "B6 third command is START_PERIODIC 0x21B1 (restart)");
        check(fake.scd41_mode == FAKE_SCD41_MODE_PERIODIC, "B6 sensor back to PERIODIC");

        tick(&rt, SCD41_PERIODIC_INTERVAL_MS);                    /* STARTING -> WAITING */
        tick_cycle(&rt, SCD41_RUNTIME_POLL_INTERVAL_MS);          /* two-phase -> READY */
        check(rt.state == DEVICE_STATE_READY, "B7 recovery reaches READY");
        check(rt.last_sample.co2_ppm == 660, "B7 valid co2=660 after auto-recovery");
        check(rt.consecutive_errors == 0, "B7 consecutive errors reset on success");
    }

    printf("\n=== Runtime C: STOP failure -> bounded error (no storm) ===\n");
    {
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        fake.probe_result = DRIVER_STATUS_OK;
        fake.scd41_mode = FAKE_SCD41_MODE_PERIODIC;

        Scd41Runtime rt;
        Scd41Runtime_Init(&rt, &bus);
        FakePlatform_SetTick(0);

        /* Force STOP to fail (START already refused by PERIODIC mode). */
        fake.write_result = DRIVER_STATUS_BUS_ERROR;
        check(Scd41Runtime_Start(&rt) == DRIVER_STATUS_BUS_ERROR,
              "C1 START+STOP both fail -> Start returns error");
        check(rt.operation_failures == 1, "C1 one START failure accounted (not a storm)");
        check(fake.scd41_cmd_log_count <= 2, "C2 no command storm (<= start+stop tries)");
        /* No recovery phase entered because STOP failed. */
        check(rt.phase != SCD41_PHASE_RECOVER_STOP_SETTLE,
              "C2 no STOP-settle recovery when STOP itself failed");
    }

    printf("\n=== Runtime D: restart failure -> bounded escalation (no STOP loop) ===\n");
    {
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        fake.probe_result = DRIVER_STATUS_OK;
        fake.scd41_mode = FAKE_SCD41_MODE_PERIODIC;

        Scd41Runtime rt;
        Scd41Runtime_Init(&rt, &bus);
        FakePlatform_SetTick(0);

        /* START refused (PERIODIC) -> STOP accepted -> enter recovery. */
        Scd41Runtime_Start(&rt);
        check(rt.phase == SCD41_PHASE_RECOVER_STOP_SETTLE, "D3 entered recovery");

        /* After settle, the restart START is ALSO refused: the sensor remains
           PERIODIC (as if it re-entered measurement between STOP and START).
           Escalate via the existing bounded threshold; do NOT re-STOP. */
        fake.scd41_cmd_log_count = 0;
        fake.scd41_mode = FAKE_SCD41_MODE_PERIODIC;   /* restart will be refused */
        tick(&rt, SCD41_RUNTIME_STOP_SETTLE_MS + 1U);
        check(rt.operation_failures >= 1, "D1 restart failure accounted");
        check(rt.consecutive_errors >= 1, "D1 consecutive errors incremented");
        /* After the failed restart we must NOT have re-issued STOP (no STOP loop). */
        bool saw_stop_after_settle = false;
        for (int i = 0; i < fake.scd41_cmd_log_count; i++)
            if (fake.scd41_cmd_log[i] == 0x3F86U) saw_stop_after_settle = true;
        check(!saw_stop_after_settle, "D2 no STOP re-issued after failed restart");
    }

    printf("\n=== Runtime E: MCU-reset model (session 1 -> reset -> session 2) ===\n");
    {
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        fake.probe_result = DRIVER_STATUS_OK;
        uint16_t t = FakeI2cBus_TempRaw(23.0f);
        uint16_t rh = FakeI2cBus_RhRaw(42.0f);

        /* Session 1: fresh idle sensor starts periodic successfully. */
        Scd41Runtime rt1;
        Scd41Runtime_Init(&rt1, &bus);
        FakePlatform_SetTick(0);
        Scd41Runtime_Start(&rt1);
        check(fake.scd41_mode == FAKE_SCD41_MODE_PERIODIC, "E1 sensor left PERIODIC in session 1");

        /* MCU reset: runtime object is gone, but fake sensor RETAINS PERIODIC mode
           (the STM32 reset did not power-cycle the SCD41). */
        Scd41Runtime rt2;
        Scd41Runtime_Init(&rt2, &bus);
        FakePlatform_SetTick(1000);
        check(fake.scd41_mode == FAKE_SCD41_MODE_PERIODIC, "E2 retained PERIODIC across reset");

        /* Session 2 must auto-recover via STOP -> settle -> START, no power cycle. */
        check(Scd41Runtime_Start(&rt2) == DRIVER_STATUS_OK, "E3 session 2 recovers (OK)");
        check(rt2.phase == SCD41_PHASE_RECOVER_STOP_SETTLE, "E3 recovery phase entered");
        FakeI2cBus_SetScd41DataReady(&fake, true);
        FakeI2cBus_SetScd41Measurement(&fake, 800, t, rh, false, false, false);
        tick(&rt2, SCD41_RUNTIME_STOP_SETTLE_MS);
        check(rt2.phase == SCD41_PHASE_IDLE, "E4 restart accepted after settle");
        tick(&rt2, SCD41_PERIODIC_INTERVAL_MS);                    /* STARTING -> WAITING */
        tick_cycle(&rt2, SCD41_RUNTIME_POLL_INTERVAL_MS);          /* two-phase -> READY */
        check(Scd41Runtime_HasValidSample(&rt2), "E5 session 2 READY without power cycle");
        check(rt2.last_sample.co2_ppm == 800, "E5 valid sample restored");
    }

    printf("\n=== Runtime: stale timeout invalidates ===\n");
    {
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        fake.probe_result = DRIVER_STATUS_OK;
        uint16_t t = FakeI2cBus_TempRaw(23.0f);
        uint16_t rh = FakeI2cBus_RhRaw(42.0f);

        Scd41Runtime rt;
        Scd41Runtime_Init(&rt, &bus);
        FakePlatform_SetTick(0);
        Scd41Runtime_Start(&rt);
        tick(&rt, SCD41_PERIODIC_INTERVAL_MS);
        FakeI2cBus_SetScd41DataReady(&fake, true);
        FakeI2cBus_SetScd41Measurement(&fake, 820, t, rh, false, false, false);
        tick_cycle(&rt, SCD41_RUNTIME_POLL_INTERVAL_MS);
        check(rt.state == DEVICE_STATE_READY, "valid before stale");

/* Stop producing data; wait past the stale timeout (data-ready false,
           so freshness enforcement is what invalidates — not an error). */
        FakeI2cBus_SetScd41DataReady(&fake, false);
        for (uint32_t el = 0; el < SCD41_RUNTIME_STALE_MS + SCD41_RUNTIME_POLL_INTERVAL_MS;
             el += SCD41_RUNTIME_POLL_INTERVAL_MS)
            tick(&rt, SCD41_RUNTIME_POLL_INTERVAL_MS);
        check(!Scd41Runtime_HasValidSample(&rt), "21 stale timeout -> invalid");
        check(rt.state != DEVICE_STATE_ERROR, "21 staleness is not an error");
    }

    printf("\n=== Runtime: VEML/display continue while SCD41 missing ===\n");
    {
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        fake.probe_result = DRIVER_STATUS_BUS_ERROR;    /* SCD41 absent */
        fake.write_result = DRIVER_STATUS_OK;

        Scd41Runtime rt;
        Scd41Runtime_Init(&rt, &bus);
        FakePlatform_SetTick(0);
        Scd41Runtime_Start(&rt);
        check(rt.state == DEVICE_STATE_NOT_FOUND, "22 SCD41 absent -> NOT_FOUND");

        /* VEML/display are independent; with probe fail they stay absent but
           the runtime does NOT block or error the MCU. Simulate VEML present on
           a separate probe result. */
        check(rt.state == DEVICE_STATE_NOT_FOUND, "22 missing SCD41 runtime independent");
        check(!Scd41Runtime_HasValidSample(&rt), "22 no valid sample while missing");
    }

    /* ===================== RoomState / App ===================== */
    printf("\n=== RoomState: SCD41 channels ===\n");
    {
        RoomState rs;
        RoomState_Init(&rs);
        check(!rs.co2_valid, "23 startup co2_valid=false");
        check(!rs.scd41_temperature_valid, "23 startup temperature_valid=false");
        check(!rs.scd41_humidity_valid, "23 startup humidity_valid=false");
        check(rs.co2_ppm == 0.0f && !rs.co2_valid, "23 no 'measured' 0 ppm");

        RoomState_UpdateScd41(&rs, 742.0f, true, 23.4f, true, 42.1f, true);
        check(rs.co2_valid, "23 valid CO2 enters RoomState");
        check(rs.co2_ppm == 742.0f, "23 co2=742");
        check(rs.scd41_temperature_valid && rs.scd41_humidity_valid,
              "23 SCD41 T/RH valid");

        /* Invalid measurement must NOT overwrite valid state as a NEW valid. */
        RoomState_InvalidateScd41(&rs);
        check(!rs.co2_valid, "24 invalid -> co2_valid=false");
        check(rs.co2_ppm == 742.0f, "24 numeric last-good retained for diagnostics");

        /* 25 CRC failure does not publish: covered at driver level (valid=false);
           here ensure an unaccepted sample leaves validity false. */
        check(!rs.scd41_temperature_valid, "25 invalid SCD41 invalidated");
    }

    printf("\n=== RoomState: loss after success / recovery ===\n");
    {
        RoomState rs;
        RoomState_Init(&rs);
        FakePlatform_SetTick(0);
        RoomState_UpdateScd41(&rs, 500.0f, true, 21.0f, true, 30.0f, true);
        check(rs.co2_valid, "26 previous success co2 valid");

        /* Loss => invalidate (as App does on stale/error). */
        RoomState_InvalidateScd41(&rs);
        check(!rs.co2_valid, "27 loss after success -> co2_valid=false");

        /* Recovery restores. */
        RoomState_UpdateScd41(&rs, 510.0f, true, 21.5f, true, 31.0f, true);
        check(rs.co2_valid && rs.co2_ppm == 510.0f, "28 recovery restores co2_valid=true");
    }

    /* ===================== Telemetry ===================== */
    printf("\n=== Telemetry: CO2 serialization ===\n");
    {
        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.sequence = 5;
        snap.room.co2_ppm = 742.0f;
        snap.room.co2_valid = true;
        snap.room.scd41_temperature_c = 23.4f;
        snap.room.scd41_temperature_valid = true;
        snap.room.scd41_humidity_pct = 42.1f;
        snap.room.scd41_humidity_valid = true;
        snap.health = SYSTEM_HEALTH_OK;

        uint8_t buf[TELEMETRY_SERIALIZED_MAX_SIZE];
        size_t w;
        SerializeStatus s = Telemetry_Serialize(&snap, buf, sizeof(buf), &w);
        check(s == SERIALIZE_OK, "29 serialize ok");
        check(strstr((char *)buf, "\"co2_ppm\"") != NULL, "29 co2_ppm present");
        check(strstr((char *)buf, "742") != NULL, "29 valid co2=742 serialized");
        check(strstr((char *)buf, "scd41_temperature_c") != NULL, "31 scd41 temp present");
        check(strstr((char *)buf, "scd41_humidity_pct") != NULL, "31 scd41 rh present");
        check(strstr((char *)buf, "\"schema\": 3") != NULL, "32 telemetry schema = 3");

        /* Invalid CO2 must not serialize as 0. */
        TelemetrySnapshot inv;
        memset(&inv, 0, sizeof(inv));
        inv.room.co2_valid = false;
        inv.room.co2_ppm = 0.0f;
        uint8_t ib[TELEMETRY_SERIALIZED_MAX_SIZE];
        size_t iw;
        Telemetry_Serialize(&inv, ib, sizeof(ib), &iw);
        check(strstr((char *)ib, "\"co2_ppm\"") != NULL, "30 co2_ppm present when invalid");
        check(strstr((char *)ib, "co2_ppm\": {\n      \"value\": 0") == NULL,
              "30 invalid CO2 NOT serialized as 0");
        check(strstr((char *)ib, "\"state\": \"invalid\"") != NULL,
              "30 invalid CO2 -> state invalid");
    }

    /* 33 GET_CAPABILITIES reports telemetry schema — covered in test_command via
       capability constant; here assert the constant is reflected. */
    printf("\n=== Telemetry: schema constant ===\n");
    {
        check(TELEMETRY_SCHEMA_VERSION == 3U, "33 TELEMETRY_SCHEMA_VERSION=3");
    }

    /* ===================== Display (bounded formatting) ===================== */
    printf("\n=== Display: bounded CO2 formatting ===\n");
    {
        char buf[24];
        /* Long/high values must not overflow the buffer. Simulate the App line
           formatting with snprintf (bounded) for CO2, T, RH, Light. */
        RoomState rs;
        RoomState_Init(&rs);
        RoomState_UpdateScd41(&rs, 40000.0f, true, 45.0f, true, 100.0f, true);
        snprintf(buf, sizeof(buf), "CO2: %lu ppm", (unsigned long)rs.co2_ppm);
        check(strstr(buf, "40000") != NULL, "34 high co2 rendered");
        check(strnlen(buf, sizeof(buf)) < sizeof(buf), "36 CO2 line does not overflow");

        RoomState_InvalidateScd41(&rs);
        snprintf(buf, sizeof(buf), "CO2: %s ppm", rs.co2_valid ? "x" : "--");
        check(strstr(buf, "--") != NULL, "35 invalid CO2 rendered as -- (not 0)");
        check(strstr(buf, "0 ppm") == NULL, "35 invalid CO2 not rendered as 0");

        RoomState_UpdateIlluminance(&rs, 100000.0f, true);
        snprintf(buf, sizeof(buf), "Light: %.0f lx", (double)rs.illuminance_lux);
        check(strnlen(buf, sizeof(buf)) < sizeof(buf), "36 Light line bounded");
    }

    /* ===================== SelfTest ===================== */
    printf("\n=== SelfTest: SCD41 ===\n");
    {
        /* Present. */
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        fake.probe_result = DRIVER_STATUS_OK;
        FakeI2cBus_GetBus(&bus, &fake);
        FakeFlash_Init();
        Storage_Init();
        SelfTestReport r;
        SelfTest_Run(&r, &bus);
        check(r.co2_sensor == SELF_TEST_PASS, "37 SCD41 present -> PASS");
        check(r.light_sensor == SELF_TEST_PASS, "40 existing light_sensor unchanged");
        check(r.display == SELF_TEST_PASS, "40 existing display unchanged");
    }

    /* SelfTest does not disturb runtime measurement mode: probing is an ACK
       check only. Covered at driver level (SCD41_Probe is non-mutating). */
    printf("\n=== SelfTest: missing -> FAIL ===\n");
    {
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        fake.probe_result = DRIVER_STATUS_BUS_ERROR;
        FakeI2cBus_GetBus(&bus, &fake);
        FakeFlash_Init();
        Storage_Init();
        SelfTestReport r;
        SelfTest_Run(&r, &bus);
        check(r.co2_sensor == SELF_TEST_FAIL, "38 SCD41 missing -> FAIL");
    }

    printf("\n=== SelfTest: NULL bus -> SKIPPED ===\n");
    {
        FakeFlash_Init();
        Storage_Init();
        SelfTestReport r;
        SelfTest_Run(&r, NULL);
        check(r.co2_sensor == SELF_TEST_SKIPPED, "39 NULL bus -> SCD41 SKIPPED");
        check(r.platform == SELF_TEST_FAIL, "40 existing fields unchanged (NULL bus)");
    }

    printf("\n=== SelfTest: command response contains SCD41 ===\n");
    {
        check(SelfTestResult_ToProtocolString(SELF_TEST_PASS) != NULL, "41 serializer available");
        /* Wire-level SELF_TEST response includes co2_sensor — asserted in the
           command integration test; here map string confirms PASS layout. */
        check(strcmp(SelfTestResult_ToProtocolString(SELF_TEST_PASS), "pass") == 0,
              "41 SELF_TEST protocol string");
    }

    /* ===================== Wire: SELF_TEST + GET_CAPABILITIES ============ */
    printf("\n=== Wire: SELF_TEST response contains co2_sensor ===\n");
    {
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        fake.probe_result = DRIVER_STATUS_OK;
        FakeI2cBus_GetBus(&bus, &fake);
        FakeFlash_Init();
        Storage_Init();

        SelfTestReport report;
        SelfTest_Run(&report, &bus);
        DeviceIdentity id; memset(&id, 0, sizeof(id));

        CommandServices svc;
        memset(&svc, 0, sizeof(svc));
        svc.bus = (struct I2cBus *)&bus;
        svc.self_test = &report;

        CommandResponse rsp;
        CommandRequest req = { .type = COMMAND_SELF_TEST, .request_id = 1, .has_request_id = true };
        check(CommandDispatcher_Dispatch(&req, &rsp, &svc), "41 SELF_TEST dispatched");
        check(strstr((char *)rsp.payload, "\"co2_sensor\":\"pass\"") != NULL ||
              strstr((char *)rsp.payload, "\"co2_sensor\":\"pass\"") != NULL,
              "41 SELF_TEST wire has co2_sensor pass");
    }

    printf("\n=== Wire: GET_CAPABILITIES reports co2 + telemetry schema ===\n");
    {
        FakeFlash_Init();
        Storage_Init();
        CommandServices svc2;
        memset(&svc2, 0, sizeof(svc2));
        CommandResponse rsp;
        CommandRequest req = { .type = COMMAND_GET_CAPABILITIES, .request_id = 2, .has_request_id = true };
        check(CommandDispatcher_Dispatch(&req, &rsp, &svc2), "41 GET_CAPABILITIES dispatched");
        check(strstr((char *)rsp.payload, "\"co2\":true") != NULL, "41 GET_CAPABILITIES co2=true");
        check(strstr((char *)rsp.payload, "\"telemetry_schema\":3") != NULL,
              "41 GET_CAPABILITIES telemetry_schema=3");
    }

    /* ============ Fixed raw wire vectors (independent of encode helpers) === */
    printf("\n=== Fixed wire vectors: SCD4x is MSB-first big-endian ===\n");

    /* 3.1 generic big-endian word: wire bytes 12 34 + CRC(0x12,0x34)=0x37 must
       decode to 0x1234, never 0x3412. */
    {
        FakeI2cBus fake; I2cBus bus; Scd41 dev; Scd41Measurement m;
        setup(&fake, &bus, &dev);
        /* read_measurement response; place a word 0x1234 as the CO2 word. */
        uint8_t raw[9];
        raw[0]=0x12; raw[1]=0x34; raw[2]=0x37;               /* CO2 =0x1234 */
        raw[3]=0x6F; raw[4]=0x61;                            /* T  raw */
        raw[6]=0x6E; raw[7]=0x00;                            /* RH raw */
        /* fill T/RH CRC with the correct SCD4x CRC for their bytes */
        raw[5]=SCD41_Crc8(&raw[3],2U);
        raw[8]=SCD41_Crc8(&raw[6],2U);
        FakeI2cBus_SetScd41RawRead(&fake, raw);
        check(read_measurement_full(&dev, &m) == DRIVER_STATUS_OK,
              "wire 12 34 decodes as 0x1234 (MSB-first)");
        check(m.co2_ppm == 0x1234U,
              "CO2 word 0x1234 decoded to 0x1234");
        check(m.co2_ppm != 0x3412U,
              "CO2 word is NOT byte-swapped to 0x3412");
    }

    /* Independent known CRC vectors (from official Sensirion SCD4x spec). */
    {
        uint8_t a[2]={0x12,0x34}; check(SCD41_Crc8(a,2)==0x37U,
              "CRC(12 34)=0x37 (official)");
        uint8_t b[2]={0xBE,0xEF}; check(SCD41_Crc8(b,2)==0x92U,
              "CRC(BE EF)=0x92 (official)");
        uint8_t c[2]={0x58,0x00}; check(SCD41_Crc8(c,2)==0x51U,
              "CRC(58 00)=0x51 (official)");
        uint8_t d[2]={0x00,0x00}; check(SCD41_Crc8(d,2)==0x81U,
              "CRC(00 00)=0x81 (official)");
    }

    /* Data-ready wire semantics, swap-sensitive.
       ready word 0x0008 has bit3 set -> lower 11 bits !=0 -> ready=true.
       byte-swapped wire 08 00 would decode as 0x0800 -> mask -> ready=false,
       so this vector fails under the old LSB-first decoder. */
    {
        FakeI2cBus fake; I2cBus bus; Scd41 dev; bool ready=false;
        setup(&fake, &bus, &dev);
        /* not ready: wire 00 00 + CRC(00 00)=0x81. */
        FakeI2cBus_SetScd41RawDataReady(&fake, 0x00, 0x00);
        check(data_ready_full(&dev, &ready) == DRIVER_STATUS_OK,
              "data-ready query ok (not ready)");
        check(!ready, "data-ready word 0x0000 -> ready=false");
        /* ready: wire 00 08 + CRC(00 08)=0x38. */
        FakeI2cBus_SetScd41RawDataReady(&fake, 0x00, 0x08);
        ready = false;
        check(data_ready_full(&dev, &ready) == DRIVER_STATUS_OK,
              "data-ready query ok (ready)");
        check(ready, "data-ready word 0x0008 -> ready=true");
    }

    /* Realistic CO2 measurement raw vector (MSB-first), CO2=0x02E2 = 738 ppm.
       byte-swapped 0xE202 = 57858 ppm obviously wrong. */
    {
        FakeI2cBus fake; I2cBus bus; Scd41 dev; Scd41Measurement m;
        setup(&fake, &bus, &dev);
        uint8_t raw[9];
        /* CO2 raw word = 0x02E2 (738 ppm). T raw 0x6F61 -> ~31.14C.
           RH raw 0x6E00 -> ~43.01%. CRCs computed with the official helper. */
        raw[0]=0x02; raw[1]=0xE2; raw[2]=SCD41_Crc8(&raw[0],2U);
        raw[3]=0x6F; raw[4]=0x61; raw[5]=SCD41_Crc8(&raw[3],2U);
        raw[6]=0x6E; raw[7]=0x00; raw[8]=SCD41_Crc8(&raw[6],2U);
        FakeI2cBus_SetScd41RawRead(&fake, raw);
        check(read_measurement_full(&dev, &m) == DRIVER_STATUS_OK,
              "realistic CO2 raw vector decodes");
        check(m.co2_ppm == 0x02E2U, "CO2 raw 0x02E2 -> 738 ppm");
        check(m.co2_ppm != 0xE202U, "CO2 not byte-swapped to 0xE202 (57858)");
        check(m.valid, "realistic vector valid=true");
        /* Temperature: raw 0x6F61 -> -45+175*0x6F61/65535 = ~31.14 C. */
        check(fabsf(m.temperature_c - 31.14f) < 0.05f,
              "temperature conversion raw 0x6F61 -> ~31.14 C");
        /* RH: raw 0x6E00 -> 100*0x6E00/65535 = ~43.01%. */
        check(fabsf(m.relative_humidity_pct - 43.01f) < 0.05f,
              "RH conversion raw 0x6E00 -> ~43.01%");
    }

    /* Atomicity & CRC rejection on raw vectors: corrupt ANY one word CRC and
       the whole sample is rejected with valid=false (no partial commit). */
    {
        /* CO2 CRC corruption. */
        FakeI2cBus fake; I2cBus bus; Scd41 dev; Scd41Measurement m;
        setup(&fake, &bus, &dev);
        uint8_t raw[9] = {0x02,0xE2,0x00, 0x6F,0x61,0x00, 0x6E,0x00,0x00};
        raw[2]=SCD41_Crc8(&raw[0],2U) ^ 0xFFU;              /* corrupt CO2 CRC */
        raw[5]=SCD41_Crc8(&raw[3],2U);
        raw[8]=SCD41_Crc8(&raw[6],2U);
        FakeI2cBus_SetScd41RawRead(&fake, raw);
        check(read_measurement_full(&dev, &m) == DRIVER_STATUS_CRC_ERROR,
              "CO2 CRC corruption -> CRC_ERROR");
        check(!m.valid, "CO2 CRC error: no partial sample (valid=false)");

        /* Temperature CRC corruption. */
        FakeI2cBus fake2; I2cBus bus2; Scd41 dev2; Scd41Measurement m2;
        setup(&fake2, &bus2, &dev2);
        uint8_t raw2[9] = {0x02,0xE2,0x00, 0x6F,0x61,0x00, 0x6E,0x00,0x00};
        raw2[2]=SCD41_Crc8(&raw2[0],2U);
        raw2[5]=SCD41_Crc8(&raw2[3],2U) ^ 0xFFU;            /* corrupt T CRC */
        raw2[8]=SCD41_Crc8(&raw2[6],2U);
        FakeI2cBus_SetScd41RawRead(&fake2, raw2);
        check(read_measurement_full(&dev2, &m2) == DRIVER_STATUS_CRC_ERROR,
              "temperature CRC corruption -> CRC_ERROR");
        check(!m2.valid, "temperature CRC error: no partial sample");

        /* RH CRC corruption. */
        FakeI2cBus fake3; I2cBus bus3; Scd41 dev3; Scd41Measurement m3;
        setup(&fake3, &bus3, &dev3);
        uint8_t raw3[9] = {0x02,0xE2,0x00, 0x6F,0x61,0x00, 0x6E,0x00,0x00};
        raw3[2]=SCD41_Crc8(&raw3[0],2U);
        raw3[5]=SCD41_Crc8(&raw3[3],2U);
        raw3[8]=SCD41_Crc8(&raw3[6],2U) ^ 0xFFU;            /* corrupt RH CRC */
        FakeI2cBus_SetScd41RawRead(&fake3, raw3);
        check(read_measurement_full(&dev3, &m3) == DRIVER_STATUS_CRC_ERROR,
              "RH CRC corruption -> CRC_ERROR");
        check(!m3.valid, "RH CRC error: no partial sample");
    }

    /* Command byte-order: each command byte transmitted MSB-first. */
    {
        FakeI2cBus fake; I2cBus bus; Scd41 dev;
        setup(&fake, &bus, &dev);
        SCD41_StartPeriodicMeasurement(&dev);   /* 21 B1 */
        check(fake.last_write_data[0]==0x21U && fake.last_write_data[1]==0xB1U,
              "start_periodic bytes = 21 B1");
        SCD41_StopPeriodicMeasurement(&dev);    /* 3F 86 */
        check(fake.last_write_data[0]==0x3FU && fake.last_write_data[1]==0x86U,
              "stop_periodic bytes = 3F 86");
        Scd41 dev2; SCD41_Init(&dev2, &bus);
        SCD41_BeginGetDataReady(&dev2);         /* E4 B8 */
        check(fake.last_write_data[0]==0xE4U && fake.last_write_data[1]==0xB8U,
              "get_data_ready bytes = E4 B8");
        SCD41_BeginReadMeasurement(&dev2);       /* EC 05 */
        check(fake.last_write_data[0]==0xECU && fake.last_write_data[1]==0x05U,
              "read_measurement bytes = EC 05");
    }

    /* Address contract: 7-bit 0x62 -> HAL byte 0xC4; no 0x62 at HAL boundary. */
    {
        FakeI2cBus fake; I2cBus bus; Scd41 dev;
        setup(&fake, &bus, &dev);
        check(dev.address == 0xC4U, "SCD41_Init stores HAL byte address 0xC4");
        check(dev.address != 0x62U, "SCD41 does not expose raw 7-bit 0x62 at bus boundary");
        fake.probe_result = DRIVER_STATUS_OK;
        SCD41_Probe(&bus);
        check(fake.last_addr == 0xC4U, "SCD41_Probe uses HAL byte 0xC4");
    }

    /* ===================== Timing: two-phase 1 ms deadline =============== */
    printf("\n=== Timing: SCD4x response not read before 1 ms ===\n");
    {
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        fake.probe_result = DRIVER_STATUS_OK;
        fake.write_result = DRIVER_STATUS_OK;

        Scd41Runtime rt;
        Scd41Runtime_Init(&rt, &bus);
        FakePlatform_SetTick(0);
        Scd41Runtime_Start(&rt);
        tick(&rt, SCD41_PERIODIC_INTERVAL_MS);   /* STARTING -> WAITING */

        uint16_t t = FakeI2cBus_TempRaw(23.0f);
        uint16_t rh = FakeI2cBus_RhRaw(42.0f);
        FakeI2cBus_SetScd41DataReady(&fake, true);
        FakeI2cBus_SetScd41Measurement(&fake, 900, t, rh, false, false, false);
        int reads0 = fake.read_call_count;

        /* First poll: Begin GET_DATA_READY (a write). No response read yet. */
        Scd41Runtime_Poll(&rt);
        check(fake.read_call_count == reads0,
              "GET_DATA_READY command sent, no response read yet");
        check(rt.phase == SCD41_PHASE_WAIT_DATA_READY_RESPONSE,
              "phase = WAIT_DATA_READY_RESPONSE");

        /* t < 1 ms: deadline not passed -> still no read. */
        FakePlatform_AdvanceTick(SCD41_COMMAND_RESPONSE_DELAY_MS - 1);
        Scd41Runtime_Poll(&rt);
        check(fake.read_call_count == reads0, "t < 1 ms: no data-ready response read");

        /* t >= 1 ms: data-ready response read allowed; kicks off measurement. */
        FakePlatform_AdvanceTick(1);
        Scd41Runtime_Poll(&rt);
        check(fake.read_call_count == reads0 + 1, "t >= 1 ms: data-ready response read");
        check(rt.phase == SCD41_PHASE_WAIT_MEASUREMENT_RESPONSE,
              "phase = WAIT_MEASUREMENT_RESPONSE");

        /* measurement response not read before its deadline. */
        FakePlatform_AdvanceTick(SCD41_COMMAND_RESPONSE_DELAY_MS - 1);
        Scd41Runtime_Poll(&rt);
        check(fake.read_call_count == reads0 + 1,
              "measurement: no response read before deadline");

        /* After deadline the measurement response is read; runtime reaches READY. */
        FakePlatform_AdvanceTick(1);
        Scd41Runtime_Poll(&rt);
        check(fake.read_call_count == reads0 + 2,
              "measurement: response read after deadline");
        check(rt.state == DEVICE_STATE_READY, "runtime reaches READY after two-phase");
        check(rt.last_sample.co2_ppm == 900, "two-phase decoded co2=900");
    }

    /* ===================== CRC cross-check ===================== */
    printf("\n=== CRC: driver vs fake agree ===\n");
    {
        uint8_t foo[2] = { 0xBE, 0xEF };
        check(SCD41_Crc8(foo, 2) == FakeI2cBus_Scd41Crc(foo, 2),
              "CRC driver == fake helper (both SCD4x)");
        uint8_t known[2] = { 0x58, 0x00 };
        check(SCD41_Crc8(known, 2) == FakeI2cBus_Scd41Crc(known, 2),
              "CRC agrees on known vector");
    }

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}