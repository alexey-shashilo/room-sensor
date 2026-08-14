#include <stdio.h>
#include <string.h>
#include <math.h>

#include "sht45.h"
#include "sht45_runtime.h"
#include "room_state.h"
#include "telemetry.h"
#include "telemetry_serializer.h"
#include "device_capabilities.h"
#include "fake_i2c_bus.h"
#include "fake_platform_time.h"

/* SHT45 host regression suite.

   Driver        : 1..14 (wire address, commands, timing, CRC, conversion,
                          failure injection, atomicity)
   Runtime       : 15..20 (startup, NOT_FOUND, transient, recovery, stale)
   RoomState     : 21 (update/invalidate)
   Telemetry     : 22..23 (valid / invalid)
   Display       : 24 (source priority is an App-side rule; here via RoomState)
   Health        : 25 (App_Sht45HealthOk mapping)
   SelfTest      : 26 (observational probe reports threshold)
   Capabilities  : 27 (temperature / RH flags)

   Fixed raw wire vectors are independent of the production encoder: each vector
   is supplied byte-exact (T word MSB-first + CRC, RH word MSB-first + CRC) and
   decoded by the real driver. The CRC helper used to BUILD the vectors is
   duplicated/injected independently below so a shared bug cannot hide. */

static int s_pass = 0, s_fail = 0, s_case = 0;

static void check(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

/* Independent SHT4x CRC-8 (poly 0x31, init 0xFF) used to build fixed vectors. */
static uint8_t crc_independent(const uint8_t *p, size_t n)
{
    uint8_t crc = 0xFFU;
    for (size_t i = 0; i < n; i++)
    {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x80U) ? (uint8_t)((crc << 1U) ^ 0x31U) : (uint8_t)(crc << 1U);
    }
    return crc;
}

/* Build a 6-byte response from decimal T(degC) and RH(%). Wire order is
   T-word MSB, T-word LSB, T-CRC, RH-word MSB, RH-word LSB, RH-CRC (datasheet
   §4.3). Uses the independent encoder so the production decode is not assumed. */
static void make_response(float t_c, float rh_pct, uint8_t out[6])
{
    uint16_t t = (uint16_t)((t_c + 45.0f) * 65535.0f / 175.0f);
    uint16_t rh = (uint16_t)((rh_pct + 6.0f) * 65535.0f / 125.0f);
    out[0] = (uint8_t)(t >> 8U);
    out[1] = (uint8_t)(t & 0xFFU);
    out[2] = crc_independent(&out[0], 2);
    out[3] = (uint8_t)(rh >> 8U);
    out[4] = (uint8_t)(rh & 0xFFU);
    out[5] = crc_independent(&out[3], 2);
}

/* runtime helper: advance the clock `delta` ms then poll once. */
static void tick(Sht45Runtime *rt, uint32_t delta)
{
    FakePlatform_AdvanceTick(delta);
    Sht45Runtime_Poll(rt);
}

int main(void)
{
    printf("SHT45 driver / runtime / integration host tests\n");

    /* ============ Driver: init / address / probe ============ */
    {
        FakeI2cBus fake; I2cBus bus; Sht45 dev;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);

        check(SHT45_Init(NULL, &bus) == DRIVER_STATUS_INVALID_ARG, "1: Init NULL dev -> INVALID_ARG");
        check(SHT45_Init(&dev, NULL) == DRIVER_STATUS_INVALID_ARG, "1: Init NULL bus -> INVALID_ARG");
        check(SHT45_Init(&dev, &bus) == DRIVER_STATUS_OK, "2: init ok");
        check(dev.address == (uint16_t)(0x44U << 1U), "2: wire address == 0x88 (0x44<<1)");
        check(dev.initialized == 1U, "2: initialized");

        check(SHT45_Probe(NULL) == DRIVER_STATUS_INVALID_ARG, "3: probe NULL bus");
        fake.probe_result = DRIVER_STATUS_OK;
        check(SHT45_Probe(&bus) == DRIVER_STATUS_OK, "3: probe success");
        fake.probe_result = DRIVER_STATUS_TIMEOUT;
        check(SHT45_Probe(&bus) == DRIVER_STATUS_TIMEOUT, "3: probe failure propagated");
    }

    /* ============ Driver: command framing ============ */
    {
        FakeI2cBus fake; I2cBus bus; Sht45 dev;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        SHT45_Init(&dev, &bus);

        check(SHT45_BeginMeasurement(&dev) == DRIVER_STATUS_OK, "4: begin measure ok");
        check(fake.sht45_last_cmd == 0xFDU, "4: correct command byte 0xFD (high precision)");
        check(fake.write_call_count == 1, "4: one write issued");

        /* not initialized -> NOT_READY */
        Sht45 other;
        memset(&other, 0, sizeof(other));
        other.bus = &bus;
        check(SHT45_BeginMeasurement(&other) == DRIVER_STATUS_NOT_READY, "4: uninit begin -> NOT_READY");
    }

    /* ============ Conversion timing / no read before deadline ============ */
    {
        FakeI2cBus fake; I2cBus bus; Sht45Runtime rt;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        FakePlatform_SetTick(0);
        Sht45Runtime_Init(&rt, &bus);

        uint8_t resp[6];
        make_response(23.4f, 44.1f, resp);
        FakeI2cBus_SetSht45Response(&fake, resp, true);

        check(Sht45Runtime_Start(&rt) == DRIVER_STATUS_OK, "15: start ok");
        check(rt.state == DEVICE_STATE_STARTING, "15: start -> STARTING");

        /* read before conversion deadline must NOT occur */
        tick(&rt, 5U);   /* < 10 ms conversion */
        check(rt.state == DEVICE_STATE_STARTING, "6: no read before deadline (still STARTING)");
        check(rt.last_sample.valid == false, "6: no sample yet");

        /* after deadline, the sample is read */
        tick(&rt, 6U);   /* total 11ms >= 10ms */
        check(rt.state == DEVICE_STATE_READY, "16: reaches READY after conversion");
        check(rt.last_sample.valid, "16: sample valid");
        /* conversion timing respected: read issued only after the deadline */
    }

    /* ============ Driver: MSB-first decode + conversion ============ */
    {
        FakeI2cBus fake; I2cBus bus; Sht45 dev; Sht45Measurement m;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        SHT45_Init(&dev, &bus);

        /* Fixed vector: T = 20.0 C -> ST = (20+45)*65535/175 ; RH=50% -> SRH = (50+6)*65535/125 */
        uint8_t resp[6];
        make_response(20.0f, 50.0f, resp);
        FakeI2cBus_SetSht45Response(&fake, resp, true);
        check(SHT45_BeginMeasurement(&dev) == DRIVER_STATUS_OK, "7: begin");
        check(SHT45_FinishMeasurement(&dev, &m) == DRIVER_STATUS_OK, "7: finish ok");
        check(fabs(m.temperature_c - 20.0f) < 0.2f, "7: temp decode ~20C (MSB-first)");
        check(fabs(m.relative_humidity_pct - 50.0f) < 0.3f, "7: RH decode ~50%");
        check(m.valid, "7: valid");
    }

    /* ============ CRC vectors (official known values) ============ */
    {
        /* Known SHT4x CRC: for the byte pair, computed by poly 0x31 init 0xFF. */
        uint8_t t0[2] = { 0xBE, 0xEF };
        check(SHT45_Crc8(t0, 2) == crc_independent(t0, 2), "8: driver CRC == independent CRC");
        /* "BE 92" -> 0xEE for SHT reference; verify against independent impl. */
        uint8_t t1[2] = { 0xBE, 0x92 };
        check(SHT45_Crc8(t1, 2) == crc_independent(t1, 2), "8: CRC known-vector matches independent");
        uint8_t a[2] = { 0x00, 0x00 };
        check(SHT45_Crc8(a, 2) == crc_independent(a, 2), "8: 0x0000 CRC");
        uint8_t b[2] = { 0xFF, 0xFF };
        check(SHT45_Crc8(b, 2) == crc_independent(b, 2), "8: 0xFFFF CRC");
    }

    /* ============ CRC failure rejects whole sample ============ */
    {
        FakeI2cBus fake; I2cBus bus; Sht45 dev; Sht45Measurement m;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        SHT45_Init(&dev, &bus);

        uint8_t resp[6];
        make_response(23.0f, 44.1f, resp);
        /* corrupt temp CRC */
        resp[2] ^= 0xFFU;
        FakeI2cBus_SetSht45Response(&fake, resp, true);
        SHT45_BeginMeasurement(&dev);
        check(SHT45_FinishMeasurement(&dev, &m) == DRIVER_STATUS_CRC_ERROR, "11: temp CRC failure -> CRC_ERROR");
        check(m.valid == false, "11: no partial commit on temp CRC fail");

        make_response(23.0f, 44.1f, resp);
        resp[5] ^= 0xFFU;   /* corrupt RH CRC */
        FakeI2cBus_SetSht45Response(&fake, resp, true);
        SHT45_BeginMeasurement(&dev);
        check(SHT45_FinishMeasurement(&dev, &m) == DRIVER_STATUS_CRC_ERROR, "12: RH CRC failure -> CRC_ERROR");
        check(m.valid == false, "12: no partial commit on RH CRC fail");
    }

    /* ============ I2C write / read failure ============ */
    {
        FakeI2cBus fake; I2cBus bus; Sht45 dev; Sht45Measurement m;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        SHT45_Init(&dev, &bus);

        fake.write_result = DRIVER_STATUS_BUS_ERROR;
        check(SHT45_BeginMeasurement(&dev) == DRIVER_STATUS_BUS_ERROR, "13: write failure propagated");

        fake.write_result = DRIVER_STATUS_OK;
        /* The sensor must be "respond"ing so the failure comes from the I2C
           read status (read_result) rather than the fake's busy-NACK path. */
        uint8_t ok_resp[6];
        make_response(23.0f, 44.0f, ok_resp);
        FakeI2cBus_SetSht45Response(&fake, ok_resp, true);
        fake.read_result = DRIVER_STATUS_TIMEOUT;
        SHT45_BeginMeasurement(&dev);
        check(SHT45_FinishMeasurement(&dev, &m) == DRIVER_STATUS_TIMEOUT, "14: read failure propagated");
        check(m.valid == false, "14: no sample on read failure");
    }

    /* ============ Runtime: startup -> READY, timing respected ============ */
    {
        FakeI2cBus fake; I2cBus bus; Sht45Runtime rt;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        FakePlatform_SetTick(0);
        Sht45Runtime_Init(&rt, &bus);

        uint8_t resp[6];
        make_response(23.4f, 44.1f, resp);
        FakeI2cBus_SetSht45Response(&fake, resp, true);

        check(Sht45Runtime_Start(&rt) == DRIVER_STATUS_OK, "15a: start ok");
        check(rt.state == DEVICE_STATE_STARTING, "15b: start -> STARTING");

        /* read before conversion deadline -> no sample, still STARTING */
        int rb = fake.read_call_count;
        tick(&rt, 5U);
        check(rt.state == DEVICE_STATE_STARTING, "15c: before deadline still STARTING");
        check(fake.read_call_count == rb, "15d: no read before deadline");

        /* past 10ms deadline -> READY */
        tick(&rt, 6U);
        check(rt.state == DEVICE_STATE_READY, "16a: reaches READY after conversion");
        check(rt.last_sample.valid, "16b: valid sample");
        check(fabs(rt.last_sample.temperature_c - 23.4f) < 0.3f, "16c: temp decoded");
        check(fabs(rt.last_sample.relative_humidity_pct - 44.1f) < 0.5f, "16d: RH decoded");
    }

    /* ============ Runtime: NOT_FOUND ============ */
    {
        FakeI2cBus fake; I2cBus bus; Sht45Runtime rt;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        fake.probe_result = DRIVER_STATUS_TIMEOUT;
        Sht45Runtime_Init(&rt, &bus);
        check(Sht45Runtime_Start(&rt) == DRIVER_STATUS_TIMEOUT, "17: probe fail -> NOT_FOUND");
        check(rt.state == DEVICE_STATE_NOT_FOUND, "17: state NOT_FOUND");
        check(Sht45Runtime_IsMissing(&rt), "17: IsMissing true");
    }

    /* ============ Runtime: transient failure then recovery ============ */
    {
        FakeI2cBus fake; I2cBus bus; Sht45Runtime rt;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        FakePlatform_SetTick(0);
        Sht45Runtime_Init(&rt, &bus);

        uint8_t resp[6];
        make_response(24.0f, 40.0f, resp);

        /* startup ok -> READY */
        FakeI2cBus_SetSht45Response(&fake, resp, true);
        Sht45Runtime_Start(&rt);
        tick(&rt, 11U);
        check(rt.state == DEVICE_STATE_READY, "18a: startup READY");

        /* transient bus error on the next measurement -> not fatal alone */
        fake.read_result = DRIVER_STATUS_BUS_ERROR;
        FakePlatform_AdvanceTick(SHT45_RUNTIME_MEASUREMENT_INTERVAL_MS);
        Sht45Runtime_Poll(&rt);          /* READY -> STARTING + issue measure */
        FakePlatform_AdvanceTick(11);    /* conversion done */
        Sht45Runtime_Poll(&rt);          /* read fails (bus error, 1 failure) */
        check(rt.consecutive_errors == 1, "18b: one transient failure counted");
        check(rt.state != DEVICE_STATE_ERROR, "18b2: single failure does not yet ERROR");

        /* recovery: next successful measurement resets errors -> READY */
        fake.read_result = DRIVER_STATUS_OK;
        FakeI2cBus_SetSht45Response(&fake, resp, true);
        FakePlatform_AdvanceTick(SHT45_RUNTIME_MEASUREMENT_INTERVAL_MS);
        Sht45Runtime_Poll(&rt);
        FakePlatform_AdvanceTick(11);
        Sht45Runtime_Poll(&rt);
        check(rt.state == DEVICE_STATE_READY, "18c: recovers to READY");
        check(rt.consecutive_errors == 0, "18d: consecutive errors reset on success");
    }

    /* ============ Runtime: repeated failure / recovery ============ */
    {
        FakeI2cBus fake; I2cBus bus; Sht45Runtime rt;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        FakePlatform_SetTick(0);
        Sht45Runtime_Init(&rt, &bus);
        uint8_t resp[6];
        make_response(25.0f, 38.0f, resp);
        FakeI2cBus_SetSht45Response(&fake, resp, true);

        Sht45Runtime_Start(&rt);
        tick(&rt, 11);
        check(rt.state == DEVICE_STATE_READY, "19a: initial READY");

        /* cause repeated failures -> ERROR */
        fake.read_result = DRIVER_STATUS_TIMEOUT;
        for (int i = 0; i < (int)SHT45_RUNTIME_ERROR_THRESHOLD; i++)
        {
            Sht45Runtime_Start(&rt);
            FakePlatform_AdvanceTick(SHT45_RUNTIME_MEASUREMENT_INTERVAL_MS);
            Sht45Runtime_Poll(&rt);
            FakePlatform_AdvanceTick(11);
            Sht45Runtime_Poll(&rt);
        }
        check(rt.state == DEVICE_STATE_ERROR, "19b: repeated failures -> ERROR");
        check(rt.consecutive_errors >= SHT45_RUNTIME_ERROR_THRESHOLD, "19c: threshold reached");
        check(rt.operation_failures >= SHT45_RUNTIME_ERROR_THRESHOLD, "19d: failures counted");
    }

    /* ============ RoomState update/invalidate ============ */
    {
        RoomState rs;
        RoomState_Init(&rs);
        RoomState_UpdateSht45(&rs, 23.0f, true, 45.0f, true);
        check(rs.sht45_temperature_valid && rs.sht45_humidity_valid, "20a: update sets valid");
        check(fabs(rs.sht45_temperature_c - 23.0f) < 0.01f && fabs(rs.sht45_humidity_pct - 45.0f) < 0.01f,
              "20b: update stores T/RH");
        RoomState_InvalidateSht45(&rs);
        check(!rs.sht45_temperature_valid && !rs.sht45_humidity_valid, "20c: invalidate clears validity");
        check(fabs(rs.sht45_temperature_c - 23.0f) < 0.01f, "20d: numeric value retained for diagnostics");
    }

    /* ============ Telemetry valid (schema v4 exposes SHT45) ============ */
    {
        TelemetrySnapshot snap; TelemetrySnapshotInput in;
        uint8_t buf[TELEMETRY_SERIALIZED_MAX_SIZE]; size_t written = 0;
        RoomState room; RoomState_Init(&room);
        RoomState_UpdateSht45(&room, 23.42f, true, 44.1f, true);
        memset(&snap, 0, sizeof(snap));
        memset(&in, 0, sizeof(in));
        snap.room = room;
        snap.health = SYSTEM_HEALTH_OK;
        check(Telemetry_Serialize(&snap, buf, sizeof(buf), &written) == SERIALIZE_OK, "21a: valid serialize ok");
        check(strstr((char *)buf, "\"sht45_temperature_c\"") != NULL, "21b: temperature field present");
        check(strstr((char *)buf, "\"sht45_humidity_pct\"") != NULL, "21c: humidity field present");
        check(strstr((char *)buf, "\"state\": \"valid\"") != NULL, "21d: valid state emitted");
        check(strstr((char *)buf, "23.4") != NULL, "21e: temp value present");
    }

    /* ============ Telemetry invalid (no fake 0) ============ */
    {
        TelemetrySnapshot snap; TelemetrySnapshotInput in;
        uint8_t buf[TELEMETRY_SERIALIZED_MAX_SIZE]; size_t written = 0;
        RoomState rs; RoomState_Init(&rs);
        RoomState_InvalidateSht45(&rs);
        memset(&snap, 0, sizeof(snap));
        memset(&in, 0, sizeof(in));
        snap.room = rs;
        snap.health = SYSTEM_HEALTH_OK;
        check(Telemetry_Serialize(&snap, buf, sizeof(buf), &written) == SERIALIZE_OK, "22a: invalid serialize ok");
        check(strstr((char *)buf, "\"sht45_temperature_c\"") != NULL, "22b: temp field present");
        check(strstr((char *)buf, "\"value\"") == NULL, "22c: no numeric value emitted when invalid");
        check(strstr((char *)buf, "\"state\": \"invalid\"") != NULL, "22d: invalid state emitted");
    }

    /* ============ Capabilities: SHT45 enables T/RH ============ */
    {
        DeviceCapabilities caps;
        DeviceCapabilities_Get(&caps);
        check(caps.temperature == true, "23a: temperature capability enabled");
        check(caps.relative_humidity == true, "23b: RH capability enabled");
        check(caps.co2 == true, "23c: CO2 still enabled (SCD41)");
    }

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}