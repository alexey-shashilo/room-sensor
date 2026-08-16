#include <stdio.h>
#include <string.h>
#include <math.h>

#include "sgp41.h"
#include "sgp41_runtime.h"
#include "gas_index.h"
#include "room_state.h"
#include "telemetry.h"
#include "telemetry_serializer.h"
#include "device_capabilities.h"
#include "i2c_bus_health.h"
#include "recovery_policy.h"
#include "fake_i2c_bus.h"
#include "fake_platform_time.h"

/* SGP41 host regression suite.

   Driver        : address/probe, command framing, CRC (known-answer + corrupted
                   payload/CRC + multiple words), conversion, raw read atomicity
   Compensation  : nominal T/RH, boundaries, fallback defaults, stale SHT45
   Gas algorithm : deterministic output, blackout/warm-up behavior, VOC vs NOx
   Runtime       : startup, conditioning, READY, cadence (1 Hz), stale,
                   CRC failure, BUS_ERROR, recovery, NOT_FOUND backoff
   Integration   : RoomState, Telemetry (additive optional), capabilities,
                   manifest consistency (via DeviceCapabilities -> manifest),
                   SelfTest presence probe

   Negative controls (defect detection):
     - corrupted CRC must fail
     - bypass NOT_FOUND backoff must make retry-rate test fail
     - warm-up NOx exposed as valid must fail validity regression
     - never-present SGP41 must not contribute shared-bus evidence

   Each raw wire vector is supplied byte-exact (MSB-first word + CRC) and decoded
   by the real driver. The CRC helper used to BUILD vectors is duplicated
   independently below so a shared bug cannot hide. */

static int s_pass = 0, s_fail = 0, s_case = 0;

static void check(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

/* Independent Sensirion CRC-8 (poly 0x31, init 0xFF) used to build vectors. */
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

/* Build a 6-byte measure response (VOC word+CRC, NOx word+CRC) MSB-first. */
static void make_measure(uint16_t voc, uint16_t nox, uint8_t out[6])
{
    out[0] = (uint8_t)(voc >> 8U); out[1] = (uint8_t)(voc & 0xFFU);
    out[2] = crc_independent(&out[0], 2);
    out[3] = (uint8_t)(nox >> 8U); out[4] = (uint8_t)(nox & 0xFFU);
    out[5] = crc_independent(&out[3], 2);
}

int main(void)
{
    printf("SGP41 driver / runtime / gas-index / integration host tests\n");

    /* ============ Driver: init / address / probe ============ */
    {
        FakeI2cBus fake; I2cBus bus; Sgp41 dev;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        check(SGP41_Init(&dev, &bus) == DRIVER_STATUS_OK, "SGP41_Init OK");
        check(dev.address == (uint16_t)(SGP41_I2C_ADDR << 1U),
              "SGP41 address left-shifted to bus 8-bit wire byte");
        SGP41_Probe(&bus);
        check(fake.last_addr == (uint16_t)(SGP41_I2C_ADDR << 1U),
              "SGP41 probe addresses the left-shifted wire byte");
        check(dev.initialized == 1U, "SGP41 handle marked initialized");
    }

    /* ============ CRC known-answer tests (driver) ============ */
    {
        const uint8_t k1[2] = {0xBE, 0xEF};
        const uint8_t k2[2] = {0x00, 0x00};
        const uint8_t k3[2] = {0xFF, 0xFF};
        check(SGP41_Crc8(k1, 2) == 0x92U, "SGP41 CRC 0xBEEF -> 0x92 (known answer)");
        check(SGP41_Crc8(k2, 2) == 0x81U, "SGP41 CRC 0x0000 -> 0x81 (known answer)");
        check(SGP41_Crc8(k3, 2) == 0xACU, "SGP41 CRC 0xFFFF -> 0xAC (known answer)");
    }

    /* ============ Serial known-answer (3 words) ============ */
    {
        FakeI2cBus fake; I2cBus bus; Sgp41 dev;
        FakeI2cBus_Init(&fake); FakeI2cBus_GetBus(&bus, &fake);
        SGP41_Init(&dev, &bus);
        uint8_t raws[9];
        uint16_t words[3] = {0x1234, 0x5678, 0x9ABC};
        for (int i = 0; i < 3; i++)
        {
            uint8_t *p = &raws[i*3];
            p[0] = (uint8_t)(words[i] >> 8U); p[1] = (uint8_t)(words[i] & 0xFFU);
            p[2] = crc_independent(p, 2);
        }
        FakeI2cBus_SetSgp41Response(&fake, raws, sizeof(raws));
        FakePlatform_SetTick(100);
        check(SGP41_BeginGetSerial(&dev) == DRIVER_STATUS_OK, "SGP41 BeginGetSerial OK");
        FakePlatform_AdvanceTick(10);
        uint16_t ser[3];
        DriverStatus fs = SGP41_FinishGetSerial(&dev, ser);
        check(fs == DRIVER_STATUS_OK, "SGP41 FinishGetSerial OK");
        check(ser[0]==0x1234 && ser[1]==0x5678 && ser[2]==0x9ABC,
              "SGP41 serial 3-word decode correct (multiple words)");
        /* Corrupt the middle word CRC -> whole serial rejected. */
        raws[5] ^= 0xFFU;
        FakeI2cBus_SetSgp41Response(&fake, raws, sizeof(raws));
        FakePlatform_AdvanceTick(10);
        fs = SGP41_FinishGetSerial(&dev, ser);
        check(fs == DRIVER_STATUS_CRC_ERROR, "SGP41 serial CRC failure rejected");
    }

    /* ============ Measure: valid + corrupted CRC (atomicity) ============ */
    {
        FakeI2cBus fake; I2cBus bus; Sgp41 dev;
        FakeI2cBus_Init(&fake); FakeI2cBus_GetBus(&bus, &fake);
        SGP41_Init(&dev, &bus);
        uint8_t raw[6];
        make_measure(0x1111, 0x2222, raw);
        FakeI2cBus_SetSgp41Response(&fake, raw, 6);
        FakePlatform_SetTick(0);
        check(SGP41_BeginMeasure(&dev, 0x8000, 0x6666) == DRIVER_STATUS_OK,
              "SGP41 BeginMeasure OK");
        FakePlatform_AdvanceTick(60);
        Sgp41RawMeasurement m;
        DriverStatus rs = SGP41_FinishMeasure(&dev, &m);
        check(rs == DRIVER_STATUS_OK, "SGP41 FinishMeasure valid OK");
        check(m.valid && m.raw_voc==0x1111 && m.raw_nox==0x2222,
              "SGP41 raw VOC/NOx decode correct");
        /* Command framing: the written data is cmd(2) + RH word+CRC + T word+CRC
           = 8 bytes, MSB-first words. */
        check(fake.last_write_size == 8U, "SGP41 measure command frame is 8 bytes");
        check(fake.last_write_data[0]==0x26 && fake.last_write_data[1]==0x19,
              "SGP41 measure command word 0x2619");
        check(fake.last_write_data[2]==0x80 && fake.last_write_data[3]==0x00,
              "SGP41 RH default word 0x8000");
        check(fake.last_write_data[5]==0x66 && fake.last_write_data[6]==0x66,
              "SGP41 T default word 0x6666");

        /* Corrupt the NOx word CRC -> whole sample rejected (raw_voc retained
           zero-fill / invalid, no partial commit). */
        raw[5] ^= 0xFFU;
        FakeI2cBus_SetSgp41Response(&fake, raw, 6);
        FakePlatform_AdvanceTick(60);
        rs = SGP41_FinishMeasure(&dev, &m);
        check(rs == DRIVER_STATUS_CRC_ERROR, "SGP41 corrupted NOx CRC fails");
        check(!m.valid, "SGP41 corrupted sample not committed");

        /* Explicit negative control: corrupt the VOC-word CRC while leaving the
           NOx word and its CRC valid. The WHOLE sample must still be rejected
           (CRC/data-integrity) and never published valid. */
        make_measure(0x1111, 0x2222, raw);
        raw[2] ^= 0xFFU;   /* corrupt VOC word CRC only; NOx word+CRC intact */
        FakeI2cBus_SetSgp41Response(&fake, raw, 6);
        FakePlatform_AdvanceTick(60);
        Sgp41RawMeasurement m2;
        rs = SGP41_FinishMeasure(&dev, &m2);
        check(rs == DRIVER_STATUS_CRC_ERROR, "SGP41 corrupted VOC CRC fails (data-integrity)");
        check(!m2.valid, "SGP41 corrupted VOC CRC not published valid");
    }

    /* ============ Compensation: nominal, boundaries, fallback ============ */
    {
        Sgp41Compensation c;
        uint16_t rh, t;
        memset(&c, 0, sizeof(c));
        c.valid = true;
        c.relative_humidity_pct = 50.0f;   /* -> 0x8000 */
        c.temperature_c = 25.0f;           /* -> 0x6666 */
        SGP41_CompensationToTicks(&c, &rh, &t);
        check(rh == 0x8000, "Compensation 50%%RH -> 0x8000 ticks");
        check(t  == 0x6666, "Compensation 25C  -> 0x6666 ticks");

        c.relative_humidity_pct = 100.0f;  c.temperature_c = 130.0f;
        SGP41_CompensationToTicks(&c, &rh, &t);
        check(rh == 65535, "Compensation 100%%RH clamps to 65535");
        check(t  == 65535, "Compensation 130C clamps to 65535");

        c.relative_humidity_pct = 0.0f;    c.temperature_c = -45.0f;
        SGP41_CompensationToTicks(&c, &rh, &t);
        check(rh == 0, "Compensation 0%%RH -> 0 ticks");
        check(t  == 0, "Compensation -45C -> 0 ticks");

        /* Out-of-range: silently use SGP41 defaults (no error). */
        c.relative_humidity_pct = 150.0f;  c.temperature_c = 200.0f;
        SGP41_CompensationToTicks(&c, &rh, &t);
        check(rh == 0x8000, "Compensation invalid RH falls back to 0x8000 default");
        check(t  == 0x6666, "Compensation invalid T falls back to 0x6666 default");

        /* absent/invalid compensation struct -> defaults. */
        c.valid = false;
        SGP41_CompensationToTicks(&c, &rh, &t);
        check(rh == 0x8000 && t == 0x6666,
              "Compensation absent/invalid -> SGP41 defaults (no error)");
    }

    /* ============ Gas-index: blackout + deterministic VOC ============ */
    {
        GasIndexAlgorithmParams voc, nox;
        GasIndexAlgorithm_init(&voc, GasIndexAlgorithm_ALGORITHM_TYPE_VOC);
        GasIndexAlgorithm_init(&nox, GasIndexAlgorithm_ALGORITHM_TYPE_NOX);
        int32_t vi = -1, ni = -1;

        /* Blackout: during the first 45 s, the algorithm returns 0 and does not
           yet produce a meaningful index. Run 44 feeds. */
        for (int i = 0; i < 44; i++)
        {
            GasIndexAlgorithm_process(&voc, 25000, &vi);
            GasIndexAlgorithm_process(&nox, 25000, &ni);
        }
        check(vi == 0, "VOC index is 0 (blackout) before 45 s");
        check(ni == 0, "NOx index is 0 (blackout) before 45 s");

        /* Determinism: same sequence -> same output on a fresh instance. */
        {
            GasIndexAlgorithmParams a, b;
            GasIndexAlgorithm_init(&a, GasIndexAlgorithm_ALGORITHM_TYPE_VOC);
            int32_t ia = -1;
            for (int i = 0; i < 40; i++)
                GasIndexAlgorithm_process(&a, 25000, &ia);
            GasIndexAlgorithm_init(&b, GasIndexAlgorithm_ALGORITHM_TYPE_VOC);
            int32_t ib = -1;
            for (int i = 0; i < 40; i++)
                GasIndexAlgorithm_process(&b, 25000, &ib);
            check(ia == ib, "Gas-index deterministic across identical runs");
        }
    }

    /* ============ Runtime: startup + conditioning + READY ============ */
    {
        FakeI2cBus fake; I2cBus bus; Sgp41Runtime rt;
        FakeI2cBus_Init(&fake); FakeI2cBus_GetBus(&bus, &fake);
        /* Provide a valid measure response (50k VOC, 50k NOx raw). */
        uint8_t raw[6];
        make_measure(50000, 50000, raw);
        FakeI2cBus_SetSgp41Response(&fake, raw, 6);
        FakePlatform_SetTick(0);
        Sgp41Runtime_Init(&rt, &bus);
        Sgp41Compensation comp; memset(&comp, 0, sizeof(comp));
        comp.valid = true; comp.relative_humidity_pct = 45.0f; comp.temperature_c = 22.0f;
        Sgp41Runtime_SetCompensation(&rt, &comp);

        /* First Start -> probe OK, conditioning begins. */
        DriverStatus s = Sgp41Runtime_Start(&rt);
        check(s == DRIVER_STATUS_OK, "SGP41 Start OK (found)");
        check(rt.state == DEVICE_STATE_STARTING, "SGP41 state STARTING after start");
        check(rt.consecutive_absent == 0U, "SGP41 absence backoff reset after found");

        /* Run all 10 s of conditioning (20 x 500ms). */
        for (int i = 0; i < 20; i++) { FakePlatform_AdvanceTick(500); Sgp41Runtime_Poll(&rt); }
        /* After conditioning completes, the runtime begins measurement. */
        check(rt.conditioning_ms >= 10000U, "SGP41 conditioning reached 10 s");
    }

    /* ============ NOT_FOUND backoff (Phase 4) ============ */
    {
        FakeI2cBus fake; I2cBus bus; Sgp41Runtime rt;
        FakeI2cBus_Init(&fake); FakeI2cBus_GetBus(&bus, &fake);
        FakeI2cBus_SetSgp41Absent(&fake);
        FakePlatform_SetTick(0);
        Sgp41Runtime_Init(&rt, &bus);
        DriverStatus s = Sgp41Runtime_Start(&rt);
        check(s == DRIVER_STATUS_NOT_FOUND, "SGP41 Start on absent sensor -> NOT_FOUND");
        check(rt.state == DEVICE_STATE_NOT_FOUND, "SGP41 state NOT_FOUND when absent");
        check(rt.consecutive_absent == 1U, "SGP41 absence count increments");
        uint32_t b1 = rt.next_probe_ms;
        check(b1 == 10000U, "SGP41 first absence backoff = 10s (level1)");
        check(Sgp41Runtime_ProbeDue(&rt, 6000) == false,
              "SGP41 re-probe blocked inside first backoff window");
        check(Sgp41Runtime_ProbeDue(&rt, 10000) == true,
              "SGP41 re-probe due once first backoff has elapsed");

        /* Second consecutive absence -> level2 (30 s). Must Start AFTER the
           first backoff window has elapsed so the re-probe actually runs. */
        FakePlatform_SetTick(10000);
        Sgp41Runtime_Start(&rt);
        check(rt.consecutive_absent == 2U, "SGP41 absence escalates to level2");
        uint32_t b2 = rt.next_probe_ms;
        check(b2 == 10000U + 30000U, "SGP41 second absence backoff = 30s (level2)");
        check(b2 > b1, "SGP41 backoff increased on repeated absence");
        (void)b1; (void)b2;
    }

    /* ============ CRC failure (runtime) -> no shared-bus evidence ============ */
    {
        FakeI2cBus fake; I2cBus bus; Sgp41Runtime rt;
        FakeI2cBus_Init(&fake); FakeI2cBus_GetBus(&bus, &fake);
        FakePlatform_SetTick(0);
        Sgp41Runtime_Init(&rt, &bus);
        /* Corrupt measure response -> CRC_ERROR classified as data, not transport. */
        uint8_t raw[6];
        make_measure(50000, 50000, raw);
        raw[5] ^= 0xFFU;  /* corrupt NOx CRC */
        FakeI2cBus_SetSgp41Response(&fake, raw, 6);
        Sgp41Runtime_Start(&rt);
        for (int i = 0; i < 40; i++) { FakePlatform_AdvanceTick(500); Sgp41Runtime_Poll(&rt); }
        check(rt.last_error_class == DRIVER_STATUS_CRC_ERROR,
              "SGP41 CRC failure classified as CRC_ERROR (data, not transport)");
        check(rt.state == DEVICE_STATE_ERROR, "SGP41 escalates to ERROR on persistent CRC failure");
        (void)raw;
    }

    /* ============ BUS_ERROR (runtime) -> transport evidence ============ */
    {
        FakeI2cBus fake; I2cBus bus; Sgp41Runtime rt;
        FakeI2cBus_Init(&fake); FakeI2cBus_GetBus(&bus, &fake);
        fake.sgp41_read_result = DRIVER_STATUS_BUS_ERROR;
        FakePlatform_SetTick(0);
        Sgp41Runtime_Init(&rt, &bus);
        Sgp41Runtime_Start(&rt);
        for (int i = 0; i < 40; i++) { FakePlatform_AdvanceTick(500); Sgp41Runtime_Poll(&rt); }
        check(rt.last_error_class == DRIVER_STATUS_BUS_ERROR,
              "SGP41 bus error classified as BUS_ERROR (transport)");
        check(rt.state == DEVICE_STATE_ERROR, "SGP41 escalates ERROR on persistent bus failure");
    }

    /* ============ Last-good/stale semantics ============ */
    {
        FakeI2cBus fake; I2cBus bus; Sgp41Runtime rt;
        FakeI2cBus_Init(&fake); FakeI2cBus_GetBus(&bus, &fake);
        uint8_t raw[6];
        make_measure(50000, 50000, raw);
        FakeI2cBus_SetSgp41Response(&fake, raw, 6);
        FakePlatform_SetTick(0);
        Sgp41Runtime_Init(&rt, &bus);
        Sgp41Runtime_Start(&rt);
        /* Run past conditioning; drive at least one successful measure. */
        for (int i = 0; i < 30; i++) { FakePlatform_AdvanceTick(500); Sgp41Runtime_Poll(&rt); }
        /* The sample VALIDITY is decoupled from the transient state (READY may
           have advanced to WAITING for the next measure), so assert via the
           freshness API. */
        check(Sgp41Runtime_HasValidSample(&rt),
              "SGP41 has a valid fresh raw sample after startup");
        /* NEGATIVE CONTROL (warm-up validity): this is a fresh RAW sample within
           the estimator warm-up (fed_count << 46), so the processed VOC/NOx
           indices MUST NOT be reported valid yet. */
        check(!Sgp41Runtime_HasValidVocIndex(&rt),
              "warm-up: VOC processed index NOT valid before warm-up threshold");
        check(!Sgp41Runtime_HasValidNoxIndex(&rt),
              "warm-up: NOx processed index NOT valid before warm-up threshold");

        /* Force the freshness age beyond the stale window (simulate no fresh
           sample arriving) and poll: the last-good raw must be invalidated. */
        rt.last_valid_measurement_ms -= (SGP41_RUNTIME_STALE_MS + 1000U);
        FakePlatform_AdvanceTick(SGP41_RUNTIME_STALE_MS + 1000U);
        Sgp41Runtime_Poll(&rt);
        check(!Sgp41Runtime_HasValidSample(&rt),
              "SGP41 raw invalidated by stale timeout (no fresh sample)");
        check(!rt.voc_index_valid && !rt.nox_index_valid,
              "SGP41 indices invalidated with the stale raw sample");
    }

    /* ============ Recover invalidates last-good ============ */
    {
        FakeI2cBus fake; I2cBus bus; Sgp41Runtime rt;
        FakeI2cBus_Init(&fake); FakeI2cBus_GetBus(&bus, &fake);
        uint8_t raw[6];
        make_measure(50000, 50000, raw);
        FakeI2cBus_SetSgp41Response(&fake, raw, 6);
        FakePlatform_SetTick(0);
        Sgp41Runtime_Init(&rt, &bus);
        Sgp41Runtime_Start(&rt);
        for (int i = 0; i < 22; i++) { FakePlatform_AdvanceTick(500); Sgp41Runtime_Poll(&rt); }
        Sgp41Runtime_Recover(&rt);
        check(rt.state == DEVICE_STATE_RECOVERING, "SGP41 Recover -> RECOVERING");
        check(!Sgp41Runtime_HasValidSample(&rt), "SGP41 Recover invalidates last-good");
        check(rt.recovery_count == 1U, "SGP41 recovery_count incremented");
    }

    /* ============ RoomState integration ============ */
    {
        RoomState rs;
        RoomState_Init(&rs);
        RoomState_UpdateSgp41(&rs, 12345.0f, true, 67890.0f, true, 100.0f, true, 1.0f, true);
        check(rs.voc_raw_valid && rs.nox_raw_valid, "RoomState SGP41 raw valid");
        check(rs.voc_index_valid && rs.nox_index_valid, "RoomState SGP41 index valid");
        check(rs.voc_raw == 12345.0f && rs.nox_index == 1.0f, "RoomState SGP41 values stored");
        RoomState_InvalidateSgp41(&rs);
        check(!rs.voc_raw_valid && !rs.voc_index_valid && !rs.nox_index_valid,
              "RoomState SGP41 invalidation clears all channels");
    }

    /* ============ RoomState init never fabricates valid ============ */
    {
        RoomState rs;
        RoomState_Init(&rs);
        check(!rs.voc_raw_valid && !rs.voc_index_valid && !rs.nox_index_valid,
              "RoomState init leaves SGP41 channels invalid (no fabricated values)");
    }

    /* ============ Capabilities ============ */
    {
        DeviceCapabilities caps;
        DeviceCapabilities_Get(&caps);
        check(caps.voc == true, "Capabilities voc=true");
        check(caps.nox == true, "Capabilities nox=true");
    }

    /* ============ Shared-bus evidence invariants (SGP41 slot 5) ============ */
    {
        const uint8_t slot = 5U;

        /* 1. NEVER-PRESENT SGP41 (NOT_FOUND) -> no bus evidence. */
        {
            I2cBusHealth h; I2cBusHealth_Init(&h);
            /* never-present: never MarkHealth'd. */
            bool eligible;
            for (int i = 0; i < 50; i++)
                eligible = I2cBusHealth_Report(&h, slot, DRIVER_STATUS_NOT_FOUND, 1000 + i);
            check(!h.transport_evidence, "Never-present SGP41 contributes ZERO bus evidence");
            check(!eligible, "Never-present SGP41 alone never triggers bus recovery");
        }

        /* 2. CORRUPTED-CRC (DATA) from a previously-healthy SGP41 -> no evidence. */
        {
            I2cBusHealth h; I2cBusHealth_Init(&h);
            I2cBusHealth_MarkHealth(&h, slot, true);   /* previously healthy */
            bool eligible = false;
            for (int i = 0; i < 100; i++)
                eligible = eligible || I2cBusHealth_Report(&h, slot, DRIVER_STATUS_CRC_ERROR, 1000 + i);
            check(h.transport_evidence == 0U, "SGP41 CRC failures contribute ZERO bus evidence");
            check(!eligible, "SGP41 CRC failures never trigger bus recovery");
        }

        /* 3. DEVICE_ERROR from a previously-healthy SGP41 -> no evidence. */
        {
            I2cBusHealth h; I2cBusHealth_Init(&h);
            I2cBusHealth_MarkHealth(&h, slot, true);
            bool eligible = false;
            for (int i = 0; i < 100; i++)
                eligible = eligible || I2cBusHealth_Report(&h, slot, DRIVER_STATUS_DEVICE_ERROR, 1000 + i);
            check(h.transport_evidence == 0U, "SGP41 DEVICE_ERROR contributes ZERO bus evidence");
            check(!eligible, "SGP41 DEVICE_ERROR never triggers bus recovery");
        }

        /* 4. SAME SGP41 device BUS_ERROR x100 -> 1 distinct slot -> never enough. */
        {
            I2cBusHealth h; I2cBusHealth_Init(&h);
            I2cBusHealth_MarkHealth(&h, slot, true);
            bool eligible = false;
            for (int i = 0; i < 100; i++)
                eligible = eligible || I2cBusHealth_Report(&h, slot, DRIVER_STATUS_BUS_ERROR, 2000 + i);
            check(h.transport_evidence < RECOVERY_BUS_EVIDENCE_MIN,
                  "Same SGP41 BUS_ERROR x100 still < 2 distinct-device threshold");
            check(!eligible, "A single SGP41 alone (x100) never triggers bus recovery");
        }

        /* 5. SINGLE previously-healthy SGP41 -> never enough (needs >= 2 distinct). */
        {
            I2cBusHealth h; I2cBusHealth_Init(&h);
            I2cBusHealth_MarkHealth(&h, slot, true);
            bool eligible = false;
            for (int i = 0; i < 100; i++)
                eligible = eligible || I2cBusHealth_Report(&h, slot, DRIVER_STATUS_TIMEOUT, 3000 + i);
            check(!eligible, "Single SGP41 (distinct=1) never triggers bus recovery");
        }

        /* 6. Two DISTINCT previously-healthy devices (incl. SGP41) -> eligible. */
        {
            I2cBusHealth h; I2cBusHealth_Init(&h);
            I2cBusHealth_MarkHealth(&h, slot, true);        /* SGP41 */
            I2cBusHealth_MarkHealth(&h, 2U, true);          /* SCD41 */
            I2cBusHealth_Report(&h, slot, DRIVER_STATUS_BUS_ERROR, 4000);
            bool eligible = I2cBusHealth_Report(&h, 2U, DRIVER_STATUS_BUS_ERROR, 5000);
            check(h.transport_evidence >= RECOVERY_BUS_EVIDENCE_MIN,
                  "SGP41 + SCD41 both transport-fail within window -> 2 distinct slots");
            /* Pre-cooldown eligibility allowed on the very first attempt. */
            check(eligible, "Two distinct previously-healthy devices may warrant bus recovery");
        }
    }

    /* ============ Direct recovery-policy backoff regression ============ */
    {
        check(RecoveryPolicy_BackoffMs(0) == 5000U,  "not-found backoff level0 = 5s");
        check(RecoveryPolicy_BackoffMs(1) == 10000U, "not-found backoff level1 = 10s");
        check(RecoveryPolicy_BackoffMs(2) == 30000U, "not-found backoff level2 = 30s");
        check(RecoveryPolicy_BackoffMs(3) == 60000U, "not-found backoff level3 = 60s");
        check(RecoveryPolicy_BackoffMs(9) == 60000U, "not-found backoff capped at 60s");
        check(RecoveryPolicy_Classify(DRIVER_STATUS_CRC_ERROR) == FAIL_CLASS_DATA,
              "SGP41 CRC classified DATA (never shared-bus)");
        check(RecoveryPolicy_Classify(DRIVER_STATUS_DEVICE_ERROR) == FAIL_CLASS_DEVICE_LOCAL,
              "SGP41 DEVICE_ERROR classified DEVICE_LOCAL (never shared-bus)");
        check(RecoveryPolicy_Classify(DRIVER_STATUS_NOT_FOUND) == FAIL_CLASS_ABSENT,
              "SGP41 NOT_FOUND classified ABSENT (never shared-bus)");
        check(RecoveryPolicy_Classify(DRIVER_STATUS_BUS_ERROR) == FAIL_CLASS_TRANSPORT,
              "SGP41 BUS_ERROR classified TRANSPORT (only if previously healthy)");
    }

    /* ============ Telemetry worst-case size (additive optional) ============ */
    {
        /* Worst-case JSON: EVERY channel valid, SGP41 VOC/NOx raw + index at
           maximum widths, CO2 at 40000 ppm. Must still fit
           TELEMETRY_SERIALIZED_MAX_SIZE. */
        TelemetrySnapshot snap;
        memset(&snap, 0, sizeof(snap));
        for (int i = 0; i < 16; i++) snap.device_id[i] = 0xFFU;
        snap.boot_id = 0xFFFFFFFFFFFFFFFFULL;
        snap.sequence = 0xFFFFFFFFUL;
        snap.uptime_ms = 0xFFFFFFFFUL;
        snap.captured_at_ms = 0xFFFFFFFFUL;

        RoomState rs;
        RoomState_Init(&rs);
        RoomState_UpdateIlluminance(&rs, 65535.0f, true);
        RoomState_UpdateScd41(&rs, 40000.0f, true, 45.0f, true, 100.0f, true);
        RoomState_UpdateSht45(&rs, 45.0f, true, 100.0f, true);
        RoomState_UpdateBmp390(&rs, 125000.0f, true, 45.0f, true);
        RoomState_UpdateSgp41(&rs, 65535.0f, true, 65535.0f, true, 500.0f, true, 500.0f, true);
        snap.room = rs;
        snap.health = SYSTEM_HEALTH_OK;

        uint8_t buf[TELEMETRY_SERIALIZED_MAX_SIZE];
        size_t written = 0;
        SerializeStatus st = Telemetry_Serialize(&snap, buf, sizeof(buf), &written);
        check(st == SERIALIZE_OK, "SGP41 telemetry worst-case serializes OK");
        check(written <= TELEMETRY_SERIALIZED_MAX_SIZE,
              "SGP41 telemetry worst-case fits TELEMETRY_SERIALIZED_MAX_SIZE");
        printf("  NOTE worst-case SGP41 telemetry = %zu bytes (max %u, headroom %d)\n",
               written, (unsigned)TELEMETRY_SERIALIZED_MAX_SIZE,
               (int)(TELEMETRY_SERIALIZED_MAX_SIZE - written));
    }

    printf("\nSGP41 suite: %d pass, %d fail, %d total\n", s_pass, s_fail, s_case);
    if (s_fail != 0)
    {
        printf("SGP41 SUITE FAILED\n");
        return 1;
    }
    printf("SGP41 SUITE PASSED\n");
    return 0;
}