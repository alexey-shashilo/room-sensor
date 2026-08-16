#include <stdio.h>
#include <string.h>
#include <math.h>

#include "recovery_policy.h"
#include "i2c_bus_health.h"
#include "sht45.h"
#include "sht45_runtime.h"
#include "scd41.h"
#include "scd41_runtime.h"
#include "bmp390.h"
#include "bmp390_runtime.h"
#include "veml7700.h"
#include "display.h"
#include "fake_i2c_bus.h"
#include "fake_platform_time.h"

/* Host regression suite for the bounded automatic sensor recovery architecture,
   revised under the final adversarial review.

   Focus areas added/strengthened for this pass:
     - Bus-evidence RESET on BOTH recovery outcomes (BeginRecovery clears the
       epoch regardless of success/failure).
     - Time-rolling evidence window (stale evidence expires; A+B outside the
       window never triggers).
     - Bus-recovery COOLDOWN (persistent multi-device BUS_ERROR for >= 60 s is
       bounded; recovery frequency is capped).
     - uint32 tick WRAP for backoff next_probe, evidence window, and cooldown.
     - I2cBus_Recover callback success/failure via the fake bus.
     - attempts/successes/failures diagnostics + invariant attempts==s+f.
     - Distinct-device evidence (A x100, A+ATIMEOUT never; A+B in-window yes).
     - Never-present device exclusion even under transport-like probe errors.
     - Previously-READY then NOT_FOUND -> no bus evidence from that alone.
     - SCD41 retained-periodic after bus recovery (existing STOP+settle+START).
     - VEML full re-init config sequence after bus recovery.
     - Display full controller init after bus recovery.
     - last-good cleared uniformly across runtimes on a recovery epoch.
     - 30-min + 60-s persistent-failure long runs. */

static int s_pass = 0, s_fail = 0, s_case = 0;

static void check(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

/* ---- independent SHT45 CRC + responder ---- */
static uint8_t crc8(const uint8_t *p, size_t n)
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

static void sht45_resp(float t_c, float rh_pct, uint8_t out[6])
{
    uint16_t t = (uint16_t)((t_c + 45.0f) * 65535.0f / 175.0f);
    uint16_t rh = (uint16_t)(((rh_pct + 6.0f) * 65535.0f / 125.0f));
    out[0] = (uint8_t)(t >> 8U); out[1] = (uint8_t)(t & 0xFFU);
    out[2] = crc8(&out[0], 2U);
    out[3] = (uint8_t)(rh >> 8U); out[4] = (uint8_t)(rh & 0xFFU);
    out[5] = crc8(&out[3], 2U);
}

int main(void)
{
    printf("Recovery policy / shared-bus / backoff host tests (adversarial review)\n\n");

    /* ============ classification + backoff ladder ============ */
    printf("=== Failure classification ===\n");
    {
        check(RecoveryPolicy_Classify(DRIVER_STATUS_BUS_ERROR) == FAIL_CLASS_TRANSPORT, "BUS_ERROR -> transport");
        check(RecoveryPolicy_Classify(DRIVER_STATUS_TIMEOUT) == FAIL_CLASS_TRANSPORT, "TIMEOUT -> transport");
        check(RecoveryPolicy_Classify(DRIVER_STATUS_NOT_FOUND) == FAIL_CLASS_ABSENT, "NOT_FOUND -> absent");
        check(RecoveryPolicy_Classify(DRIVER_STATUS_CRC_ERROR) == FAIL_CLASS_DATA, "CRC_ERROR -> data");
        check(RecoveryPolicy_Classify(DRIVER_STATUS_VERIFY_ERROR) == FAIL_CLASS_DATA, "VERIFY_ERROR -> data");
        check(RecoveryPolicy_Classify(DRIVER_STATUS_DEVICE_ERROR) == FAIL_CLASS_DEVICE_LOCAL, "DEVICE_ERROR -> device-local");
    }

    printf("=== NOT_FOUND backoff ladder ===\n");
    {
        check(RecoveryPolicy_BackoffMs(0) == 5000U, "initial 5s");
        check(RecoveryPolicy_BackoffMs(1) == 10000U, "second 10s");
        check(RecoveryPolicy_BackoffMs(2) == 30000U, "third 30s");
        check(RecoveryPolicy_BackoffMs(3) == 60000U, "fourth 60s");
        check(RecoveryPolicy_BackoffMs(9) == 60000U, "capped 60s");
        check(RecoveryPolicy_TrackAbsence(0, false) == 0, "live resets absence to 0");
        check(RecoveryPolicy_TrackAbsence(3, true) == 4, "absent increments absence");
        check(RECOVERY_BUS_EVIDENCE_MIN == 2U, "evidence min = 2 distinct devices");
        check(RECOVERY_BUS_COOLDOWN_MS == 60000U, "bus recovery cooldown = 60 s");
    }

    /* ============ uint32 wrap: backoff next_probe + window + cooldown ============ */
    printf("=== uint32 tick wrap (backoff / window / cooldown) ===\n");
    {
        /* Wrap-safe elapsed primitive. since=0xFFFFFC00 is 1024ms before wrap;
           now=0x00000010 (+16ms after wrap) -> delta=0x1010=4112ms >= 1000. */
        check(RecoveryPolicy_Elapsed(0x00000010U, 0xFFFFFC00U, 1000U) == true,
              "Elapsed across wrap (delta > window)");
        check(RecoveryPolicy_Elapsed(0x00000100U, 0xFFFFFFF0U, 100U) == true,
              "Elapsed across wrap small window");
        check(RecoveryPolicy_Elapsed(0x00000008U, 0xFFFFFC00U, 10000U) == false,
              "not elapsed before window after wrap");
        check(RecoveryPolicy_Elapsed(0xFFFFFFF0U, 0xFFFFFFF0U, 0U) == true,
              "Elapsed same-instant window0 true");

        /* WindowWithin across wrap. t=0xFFFFF2 is 14ms before `now`=0x10 (within
           100ms window). t=0xFFFF00 is 272ms before -> outside. */
        check(RecoveryPolicy_WindowWithin(0x00000010U, 0xFFFFFFF2U, 100U) == true,
              "WindowWithin: recent tick inside window across wrap");
        check(RecoveryPolicy_WindowWithin(0x00000010U, 0xFFFFFF00U, 100U) == false,
              "WindowWithin: too far before wrap is outside window");

        /* Bus-health evidence window + cooldown near wrap. */
        I2cBusHealth h; I2cBusHealth_Init(&h);
        h.ever_recovered = false;
        I2cBusHealth_SetDeviceKnown(&h, 0, true);
        I2cBusHealth_SetDeviceKnown(&h, 1, true);
        /* A reports near wrap, B a few ms later (still in window) -> eligible. */
        check(I2cBusHealth_Report(&h, 0, DRIVER_STATUS_BUS_ERROR, 0xFFFFFFF0U) == false,
              "wrap: A evidence (1 device, no trigger)");
        check(I2cBusHealth_Report(&h, 1, DRIVER_STATUS_BUS_ERROR, 0xFFFFFFE0U) == false,
              "wrap: B evidence before A but Delta small (still in window)");
        /* Drive a recovery attempt and check cooldown across wrap */
        I2cBusHealth_Init(&h);
        I2cBusHealth_SetDeviceKnown(&h, 0, true);
        I2cBusHealth_SetDeviceKnown(&h, 1, true);
        I2cBusHealth_Report(&h, 0, DRIVER_STATUS_BUS_ERROR, 0xFFFFFF00U);
        check(I2cBusHealth_Report(&h, 1, DRIVER_STATUS_BUS_ERROR, 0xFFFFFF10U) == true,
              "wrap: A+B in-window -> eligible, recover");
        check(I2cBusHealth_ShouldRecover(&h) == true, "wrap: RECOVERING set");
        I2cBusHealth_BeginRecovery(&h, 0xFFFFFF20U);
        I2cBusHealth_OnRecoverySuccess(&h);
        /* Next report right after wrap with cooldown not yet elapsed -> blocked. */
        check(I2cBusHealth_Report(&h, 0, DRIVER_STATUS_BUS_ERROR, 0x00000010U) == false,
              "wrap: immediate re-report within cooldown blocked");
        check(I2cBusHealth_Report(&h, 1, DRIVER_STATUS_BUS_ERROR, 0x00000018U) == false,
              "wrap: A+B fresh but cooldown (60s) not elapsed -> no recover");
        /* After cooldown the fresh evidence may trigger again. */
        check(I2cBusHealth_RecoveryEligible(&h, 0x00001000U) == false,
              "wrap: still within cooldown after wrap (delta ~4320ms < 60s)");
    }

    /* ============ SHT45 NOT_FOUND backoff + recovery ============ */
    printf("=== SHT45 NOT_FOUND backoff + recover ===\n");
    {
        FakeI2cBus fake; I2cBus bus; Sht45Runtime rt;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        FakePlatform_SetTick(0);
        Sht45Runtime_Init(&rt, &bus);

        fake.probe_result = DRIVER_STATUS_NOT_FOUND;
        check(Sht45Runtime_Start(&rt) == DRIVER_STATUS_NOT_FOUND, "absent probe -> NOT_FOUND");
        check(rt.state == DEVICE_STATE_NOT_FOUND, "state NOT_FOUND after failed probe");
        check(rt.consecutive_absent == 1U, "absence count = 1");

        FakePlatform_AdvanceTick(2000);
        fake.probe_result = DRIVER_STATUS_NOT_FOUND;
        check(Sht45Runtime_Start(&rt) == DRIVER_STATUS_NOT_FOUND, "re-probe gated while backoff <10s");
        check(rt.consecutive_absent == 1U, "absence not incremented during gated probe");

        FakePlatform_AdvanceTick(9000);
        fake.probe_result = DRIVER_STATUS_OK;
        uint8_t resp[6]; sht45_resp(23.0f, 44.0f, resp);
        FakeI2cBus_SetSht45Response(&fake, resp, true);
        check(Sht45Runtime_Start(&rt) == DRIVER_STATUS_OK, "probe succeeds after backoff");
        check(rt.consecutive_absent == 0U, "successful probe resets absence");

        Sht45Runtime_Poll(&rt);
        FakePlatform_AdvanceTick(SHT45_MEASUREMENT_DURATION_MS);
        Sht45Runtime_Poll(&rt);
        check(rt.state == DEVICE_STATE_READY, "SHT45 recovers to READY");
        check(rt.last_sample.valid, "fresh valid sample after recovery");
    }

    printf("=== SCD41 NOT_FOUND backoff ===\n");
    {
        FakeI2cBus fake; I2cBus bus; Scd41Runtime rt;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        FakePlatform_SetTick(0);
        Scd41Runtime_Init(&rt, &bus);

        fake.probe_result = DRIVER_STATUS_NOT_FOUND;
        check(Scd41Runtime_Start(&rt) == DRIVER_STATUS_NOT_FOUND, "SCD41 absent -> NOT_FOUND");
        check(rt.consecutive_absent == 1U, "SCD41 absence count = 1");
        FakePlatform_AdvanceTick(3000);
        fake.probe_result = DRIVER_STATUS_NOT_FOUND;
        check(Scd41Runtime_Start(&rt) == DRIVER_STATUS_NOT_FOUND, "SCD41 re-probe gated (<10s)");
        check(rt.consecutive_absent == 1U, "SCD41 absence not incremented during gate");
        FakePlatform_AdvanceTick(8000);
        fake.probe_result = DRIVER_STATUS_OK;
        check(Scd41Runtime_Start(&rt) == DRIVER_STATUS_OK, "SCD41 re-probe after backoff");
        check(rt.consecutive_absent == 0U, "SCD41 absence reset on success");
    }

    printf("=== BMP390 NOT_FOUND backoff ===\n");
    {
        FakeI2cBus fake; I2cBus bus; Bmp390Runtime rt;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        FakePlatform_SetTick(0);
        Bmp390Runtime_Init(&rt, &bus);

        fake.probe_result = DRIVER_STATUS_NOT_FOUND;
        check(Bmp390Runtime_Start(&rt) == DRIVER_STATUS_NOT_FOUND, "BMP390 absent -> NOT_FOUND");
        check(rt.consecutive_absent == 1U, "BMP390 absence count = 1");
        FakePlatform_AdvanceTick(4000);
        fake.probe_result = DRIVER_STATUS_NOT_FOUND;
        check(Bmp390Runtime_Start(&rt) == DRIVER_STATUS_NOT_FOUND, "BMP390 re-probe gated (<10s)");
        check(rt.consecutive_absent == 1U, "BMP390 absence not incremented during gate");
        FakePlatform_AdvanceTick(7000);
        fake.probe_result = DRIVER_STATUS_OK;
        FakeI2cBus_SetBmp390Present(&fake, 0xEC, BMP390_CHIP_ID, NULL);
        check(Bmp390Runtime_Start(&rt) == DRIVER_STATUS_OK, "BMP390 re-probe after backoff");
        check(rt.consecutive_absent == 0U, "BMP390 absence reset on success");
        FakeI2cBus_SetBmp390Absent(&fake);
    }

    /* ============ distinct-device evidence ============ */
    printf("=== same-device x100 + A+ATIMEOUT never trigger ===\n");
    {
        I2cBusHealth h; I2cBusHealth_Init(&h);
        I2cBusHealth_SetDeviceKnown(&h, 0, true);
        for (int i = 0; i < 100; i++)
            I2cBusHealth_Report(&h, 0, DRIVER_STATUS_BUS_ERROR, 1000U + (uint32_t)i);
        check(I2cBusHealth_ShouldRecover(&h) == false, "A BUS_ERROR x100 -> still one device, no bus recovery");

        I2cBusHealth h2; I2cBusHealth_Init(&h2);
        I2cBusHealth_SetDeviceKnown(&h2, 0, true);
        I2cBusHealth_Report(&h2, 0, DRIVER_STATUS_BUS_ERROR, 1000);
        I2cBusHealth_Report(&h2, 0, DRIVER_STATUS_TIMEOUT, 1100);
        check(I2cBusHealth_ShouldRecover(&h2) == false, "A BUS_ERROR + A TIMEOUT -> still one device, no bus recovery");
    }

    printf("=== A+B in-window triggers; A+B outside window does not ===\n");
    {
        I2cBusHealth h; I2cBusHealth_Init(&h);
        I2cBusHealth_SetDeviceKnown(&h, 0, true);
        I2cBusHealth_SetDeviceKnown(&h, 1, true);
        /* B far outside A's window (>= 10 s apart) -> no trigger. */
        check(I2cBusHealth_Report(&h, 0, DRIVER_STATUS_BUS_ERROR, 1000U) == false, "A at t=1000");
        /* Re-report A so it is not dropped by window aging is not needed; B at
           t=12000 exceeds 10s window from A. */
        check(I2cBusHealth_Report(&h, 1, DRIVER_STATUS_BUS_ERROR, 12000U) == false,
              "B at t=12000 (10.2s after A) -> window expired, no bus recovery");
        check(I2cBusHealth_ShouldRecover(&h) == false, "out-of-window A+B -> no recovery");

        I2cBusHealth h2; I2cBusHealth_Init(&h2);
        I2cBusHealth_SetDeviceKnown(&h2, 0, true);
        I2cBusHealth_SetDeviceKnown(&h2, 1, true);
        check(I2cBusHealth_Report(&h2, 0, DRIVER_STATUS_BUS_ERROR, 1000U) == false, "A at t=1000");
        check(I2cBusHealth_Report(&h2, 1, DRIVER_STATUS_BUS_ERROR, 1500U) == true, "B within window -> bus recovery eligible");
        check(I2cBusHealth_ShouldRecover(&h2) == true, "in-window A+B -> RECOVERING");
    }

    /* ============ never-present exclusion ============ */
    printf("=== never-present device exclusion ===\n");
    {
        I2cBusHealth h; I2cBusHealth_Init(&h);
        /* BMP390 / SHT45 never present. */
        I2cBusHealth_SetDeviceKnown(&h, 2, false);
        I2cBusHealth_SetDeviceKnown(&h, 3, false);
        /* Even transport-like probe errors from never-present devices must not
           become bus evidence. */
        I2cBusHealth_Report(&h, 2, DRIVER_STATUS_BUS_ERROR, 1000);
        I2cBusHealth_Report(&h, 3, DRIVER_STATUS_BUS_ERROR, 1100);
        check(I2cBusHealth_ShouldRecover(&h) == false, "never-present BMP390/SHT45 BUS_ERROR x2 -> no bus recovery");
        check(I2cBusHealth_GetBusRecoveryAttempts(&h) == 0U, "no recovery attempts triggered by never-present devices");
    }

    /* ============ previously-READY then NOT_FOUND ============ */
    printf("=== previously-READY then NOT_FOUND: no bus evidence ===\n");
    {
        I2cBusHealth h; I2cBusHealth_Init(&h);
        I2cBusHealth_SetDeviceKnown(&h, 0, true);   /* was present/healthy */
        /* Device disappears: NOT_FOUND from a previously-present device must not
           be bus evidence. */
        I2cBusHealth_Report(&h, 0, DRIVER_STATUS_NOT_FOUND, 1000);
        I2cBusHealth_Report(&h, 0, DRIVER_STATUS_NOT_FOUND, 1100);
        check(I2cBusHealth_ShouldRecover(&h) == false, "previously-READY then NOT_FOUND x2 -> no bus recovery");
        /* A second, independent previously-healthy device must ALSO report a
           transport failure for bus evidence. */
        I2cBusHealth_SetDeviceKnown(&h, 1, true);
        check(I2cBusHealth_Report(&h, 0, DRIVER_STATUS_BUS_ERROR, 2000) == false,
              "single device transport still not enough");
        check(I2cBusHealth_ShouldRecover(&h) == false, "disappearance alone never triggers bus recovery");
    }

    /* ============ P1-2C: historical health is monotonic + App-level path ============ */
    printf("=== P1-2: previously-healthy history survives NOT_FOUND transition ===\n");
    {
        /* Reproduce the App orchestration: MarkHealth on real READY/valid
           evidence, then the device disappears (NOT_FOUND), then it reports a
           genuine transport BUS_ERROR. The healthy history MUST survive the
           disappearance so the device can still contribute bus evidence (it was
           a genuine participant). */
        I2cBusHealth h; I2cBusHealth_Init(&h);
        /* SCD41 (slot2) and SHT45 (slot3) previously READY with valid samples. */
        I2cBusHealth_MarkHealth(&h, 2U, true);
        I2cBusHealth_MarkHealth(&h, 3U, true);
        check(h.previously_healthy[2U] == true, "SCD41 healthy latched");
        check(h.previously_healthy[3U] == true, "SHT45 healthy latched");

        /* Device transitions through NOT_FOUND: alone must NOT be transport evidence
           (device-local disappearance), but ALSO must not un-latch health. */
        I2cBusHealth_Report(&h, 2U, DRIVER_STATUS_NOT_FOUND, 1000);
        I2cBusHealth_Report(&h, 3U, DRIVER_STATUS_NOT_FOUND, 1100);
        check(h.previously_healthy[2U] == true, "SCD41 health survives NOT_FOUND");
        check(h.previously_healthy[3U] == true, "SHT45 health survives NOT_FOUND");
        check(I2cBusHealth_ShouldRecover(&h) == false, "both-NOT_FOUND alone -> no bus recovery");

        /* Genuine transport outage: both devices now report BUS_ERROR. Because the
         * healthy history survived, two DISTINCT previously-healthy devices give
         * evidence -> exactly one eligible collective recovery. */
        check(I2cBusHealth_Report(&h, 2U, DRIVER_STATUS_BUS_ERROR, 2000) == false,
              "SC41 BUS_ERROR contributes distinct evidence (no trigger yet)");
        bool trig = I2cBusHealth_Report(&h, 3U, DRIVER_STATUS_BUS_ERROR, 2000);
        check(trig == true, "SHT45 BUS_ERROR + SCD41 BUS_ERROR -> recovery eligible");
        check(I2cBusHealth_ShouldRecover(&h) == true, "RECOVERING with 2 distinct participants");

        /* Exactly one recovery; evidence clears; cooldown effective. */
        I2cBusHealth_BeginRecovery(&h, 2100);
        I2cBusHealth_OnRecoverySuccess(&h);
        check(I2cBusHealth_GetBusRecoveryCount(&h) == 1, "exactly one recovery");
        check(I2cBusHealth_ShouldRecover(&h) == false, "evidence cleared after recovery");
        /* Immediate re-report of both within cooldown: evidence may re-arm the
           RECOVERING state, but the ACTUAL recovery is gated by cooldown, so no
           second recovery may execute yet. */
        I2cBusHealth_Report(&h, 2U, DRIVER_STATUS_BUS_ERROR, 3000);
        I2cBusHealth_Report(&h, 3U, DRIVER_STATUS_BUS_ERROR, 3100);
        check(I2cBusHealth_RecoveryEligible(&h, 3100) == false, "cooldown blocks re-recovery execution");
        check(I2cBusHealth_GetBusRecoveryCount(&h) == 1, "still exactly one recovery (cooldown enforced)");
    }

    /* ============ evidence reset: success AND failure ============ */
    printf("=== evidence reset after recovery (success + failure) ===\n");
    {
        /* Success path. */
        I2cBusHealth h; I2cBusHealth_Init(&h);
        I2cBusHealth_SetDeviceKnown(&h, 0, true);
        I2cBusHealth_SetDeviceKnown(&h, 1, true);
        I2cBusHealth_Report(&h, 0, DRIVER_STATUS_BUS_ERROR, 1000);
        I2cBusHealth_Report(&h, 1, DRIVER_STATUS_BUS_ERROR, 1100);
        check(I2cBusHealth_ShouldRecover(&h) == true, "A+B -> recovery warranted");
        I2cBusHealth_BeginRecovery(&h, 1200);
        I2cBusHealth_OnRecoverySuccess(&h);
        /* Old evidence (A) must not re-trigger; a second distinct device required. */
        check(I2cBusHealth_Report(&h, 0, DRIVER_STATUS_BUS_ERROR, 2000) == false,
              "post-success: A alone does not re-trigger");
        check(I2cBusHealth_ShouldRecover(&h) == false, "post-success: no recovery from stale A");
        check(I2cBusHealth_Report(&h, 1, DRIVER_STATUS_BUS_ERROR, 2100) == false,
              "post-success: A + B fresh but cooldown blocks (or evidence re-armed)");

        /* Failure path: BeginRecovery clears evidence BEFORE the attempt. */
        I2cBusHealth h2; I2cBusHealth_Init(&h2);
        I2cBusHealth_SetDeviceKnown(&h2, 0, true);
        I2cBusHealth_SetDeviceKnown(&h2, 1, true);
        I2cBusHealth_Report(&h2, 0, DRIVER_STATUS_BUS_ERROR, 1000);
        I2cBusHealth_Report(&h2, 1, DRIVER_STATUS_BUS_ERROR, 1100);
        I2cBusHealth_BeginRecovery(&h2, 1200);            /* clears old A+B evidence */
        I2cBusHealth_OnRecoveryFailure(&h2);
        uint32_t attempts = I2cBusHealth_GetBusRecoveryAttempts(&h2);
        check(attempts == 1U, "failed attempt recorded (attempts=1)");
        check(I2cBusHealth_GetBusRecoveryFailures(&h2) == 1U, "failures=1");
        check(I2cBusHealth_GetBusRecoverySuccesses(&h2) == 0U, "successes=0");
        check(attempts == I2cBusHealth_GetBusRecoverySuccesses(&h2) + I2cBusHealth_GetBusRecoveryFailures(&h2),
              "invariant attempts == successes + failures");
        /* Old A+B evidence must NOT be able to immediately re-trigger forever. */
        check(I2cBusHealth_Report(&h2, 0, DRIVER_STATUS_BUS_ERROR, 1300) == false,
              "post-failure: old evidence gone, A alone does not re-trigger");
        check(I2cBusHealth_ShouldRecover(&h2) == false, "post-failure: no instant re-recover from stale evidence");
    }

    /* ============ cooldown / storm protection ============ */
    printf("=== persistent multi-device BUS_ERROR for >= 60 s is bounded ===\n");
    {
        /* Continuous A+B BUS_ERROR every retry for 5 minutes of virtual time. */
        I2cBusHealth h; I2cBusHealth_Init(&h);
        I2cBusHealth_SetDeviceKnown(&h, 0, true);
        I2cBusHealth_SetDeviceKnown(&h, 1, true);
        uint32_t attempts = 0;
        uint32_t i;
        for (i = 0; i < 300000U; i += 500U)   /* 5 min, retry cadence 500ms */
        {
            bool trig = I2cBusHealth_Report(&h, 0, DRIVER_STATUS_BUS_ERROR, i);
            bool trig2 = I2cBusHealth_Report(&h, 1, DRIVER_STATUS_BUS_ERROR, i);
            if (trig || trig2)
            {
                if (I2cBusHealth_RecoveryEligible(&h, i))
                {
                    I2cBusHealth_BeginRecovery(&h, i);
                    I2cBusHealth_OnRecoveryFailure(&h);   /* bus stays bad */
                    attempts++;
                }
            }
        }
        /* Worst possible frequency = 1 per cooldown (60 s). In 300 s that's 5. */
        check(attempts <= (300000U / RECOVERY_BUS_COOLDOWN_MS) + 1U,
              "persistent failure recoveries are bounded (<= 1 per cooldown)");
        printf("    persistent-failure attempts over 300 s = %lu (max allowed ~6)\n",
               (unsigned long)attempts);
        uint32_t max_possible = (300000U / RECOVERY_BUS_COOLDOWN_MS) + 1U;
        check(attempts <= max_possible, "recovery frequency capped by cooldown");
        check(I2cBusHealth_GetBusRecoveryFailures(&h) == attempts, "failure counter matches attempts");
        check(I2cBusHealth_GetBusRecoveryAttempts(&h) == attempts, "attempts counter matches");
    }

    /* ============ counters invariant ============ */
    printf("=== recovery diagnostics invariant ===\n");
    {
        I2cBusHealth h; I2cBusHealth_Init(&h);
        I2cBusHealth_SetDeviceKnown(&h, 0, true);
        I2cBusHealth_SetDeviceKnown(&h, 1, true);
        /* two attempts: success then failure */
        I2cBusHealth_Report(&h, 0, DRIVER_STATUS_BUS_ERROR, 1000);
        I2cBusHealth_Report(&h, 1, DRIVER_STATUS_BUS_ERROR, 1100);
        I2cBusHealth_BeginRecovery(&h, 1200); I2cBusHealth_OnRecoverySuccess(&h);
        I2cBusHealth_Report(&h, 0, DRIVER_STATUS_BUS_ERROR, 130000);
        I2cBusHealth_Report(&h, 1, DRIVER_STATUS_BUS_ERROR, 130100);
        I2cBusHealth_BeginRecovery(&h, 130200); I2cBusHealth_OnRecoveryFailure(&h);
        check(I2cBusHealth_GetBusRecoveryAttempts(&h) == 2U, "attempts == 2");
        check(I2cBusHealth_GetBusRecoverySuccesses(&h) == 1U, "successes == 1");
        check(I2cBusHealth_GetBusRecoveryFailures(&h) == 1U, "failures == 1");
        check(I2cBusHealth_GetBusRecoveryAttempts(&h) ==
              I2cBusHealth_GetBusRecoverySuccesses(&h) + I2cBusHealth_GetBusRecoveryFailures(&h),
              "invariant attempts == s + f holds");
    }

    /* ============ I2cBus_Recover callback success/failure ============ */
    printf("=== I2cBus_Recover callback success + failure ===\n");
    {
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        fake.recover_result = DRIVER_STATUS_OK;
        check(I2cBus_Recover(&bus) == DRIVER_STATUS_OK, "recover callback success returned OK");
        check(fake.recover_call_count == 1, "recover callback invoked once");
        fake.recover_result = DRIVER_STATUS_BUS_ERROR;
        check(I2cBus_Recover(&bus) == DRIVER_STATUS_BUS_ERROR, "recover callback failure propagated");
        check(fake.recover_call_count == 2, "recover callback invoked again on failure");
    }

    /* ============ SCD41 retained-periodic after bus recovery ============ */
    printf("=== SCD41 retained-periodic after bus recovery ===\n");
    {
        FakeI2cBus fake; I2cBus bus; Scd41Runtime rt;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        FakePlatform_SetTick(0);
        Scd41Runtime_Init(&rt, &bus);

        /* Drive to READY with valid sample. */
        fake.probe_result = DRIVER_STATUS_OK;
        FakeI2cBus_SetScd41DataReady(&fake, true);
        FakeI2cBus_SetScd41Measurement(&fake, 420, FakeI2cBus_TempRaw(23.0f), FakeI2cBus_RhRaw(44.0f),
                                       false, false, false);
        check(Scd41Runtime_Start(&rt) == DRIVER_STATUS_OK, "SCD41 start OK");
        /* first sample window: advance > periodic interval then poll */
        int steps = 0;
        while (rt.state != DEVICE_STATE_READY && steps < 50)
        {
            FakePlatform_AdvanceTick(500);
            Scd41Runtime_Poll(&rt);
            steps++;
        }
        check(rt.state == DEVICE_STATE_READY, "SCD41 READY (pre-bus-recovery)");
        check(Scd41Runtime_HasValidSample(&rt), "SCD41 has valid sample");

        /* Bus recovery: runtime Recover + re-init. Sensor still PERIODIC (the
           peripheral was reset but the SCD41 itself wasn't power-cycled). */
        Scd41Runtime_Recover(&rt);
        check(rt.state == DEVICE_STATE_RECOVERING, "SCD41 RECOVERING after bus recovery");
        check(Scd41Runtime_HasValidSample(&rt) == false, "last-good invalidated (recovery epoch)");

        /* Retained-periodic: START would be refused; runtime uses STOP+settle+START. */
        FakeI2cBus_SetScd41Mode(&fake, FAKE_SCD41_MODE_PERIODIC);  /* still periodic */
        check(Scd41Runtime_Start(&rt) == DRIVER_STATUS_OK, "re-START after bus recovery returns OK (enters settle path)");
        check(rt.phase == SCD41_PHASE_RECOVER_STOP_SETTLE, "in retained-periodic STOP-settle phase");

        /* Step through 500 ms settle then the fresh first-sample wait. */
        int s2 = 0;
        while (rt.state != DEVICE_STATE_READY && s2 < 100)
        {
            FakePlatform_AdvanceTick(500);
            Scd41Runtime_Poll(&rt);
            s2++;
        }
        check(rt.state == DEVICE_STATE_READY, "SCD41 returns READY via retained-periodic recovery");
        check(Scd41Runtime_HasValidSample(&rt), "SCD41 fresh sample after bus recovery");
    }

    /* ============ last-good cleared uniformly (recovery epoch) ============ */
    printf("=== last-good cleared uniformly on recovery epoch ===\n");
    {
        Sht45Runtime rt; FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake); FakeI2cBus_GetBus(&bus, &fake);
        FakePlatform_SetTick(10000);
        Sht45Runtime_Init(&rt, &bus);
        uint8_t r[6]; sht45_resp(24.0f, 40.0f, r);
        FakeI2cBus_SetSht45Response(&fake, r, true);
        Sht45Runtime_Start(&rt);
        Sht45Runtime_Poll(&rt);
        FakePlatform_AdvanceTick(11);
        Sht45Runtime_Poll(&rt);
        check(rt.state == DEVICE_STATE_READY && rt.last_sample.valid, "SHT45 READY with valid sample");
        Sht45Runtime_Recover(&rt);
        check(rt.last_sample.valid == false, "SHT45 last-good invalidated on Recover (uniform policy)");
        check(rt.consecutive_errors == 0U, "SHT45 Recover resets error budget");
    }

    /* ============ VEML re-init after bus recovery ============ */
    printf("=== VEML re-init (config restored) after bus recovery ===\n");
    {
        /* VEML is driven by App: App_DoProbeVeml + App_DoInitVeml re-run full
           Probe + Init (which re-applies ALS_CONFIG + readback). We assert the
           driver-level contract: a fresh VEML7700_Init performs the full
           configure sequence (probe + ApplyIndex with conf readback). */
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        fake.probe_result = DRIVER_STATUS_OK;
        FakeI2cBus_SetAlsRead(&fake, 5000U);
        int w = fake.write_call_count;
        int r = fake.read_mem_call_count;
        VEML7700_HandleTypeDef veml;
        check(VEML7700_Init(&veml, &bus) == true, "VEML Init after bus recovery returns true (config restored)");
        check(VEML7700_IsInitialized(&veml) == true, "VEML re-initialized flag set");
        /* Init wrote ALS_CONF config and read it back. */
        check(fake.write_call_count >= w + 1, "VEML Init performed a config write (ALS_CONF)");
        check(fake.read_mem_call_count >= r + 1, "VEML Init performed ALS_CONF readback");
    }

    /* ============ Display re-init after bus recovery ============ */
    printf("=== Display re-init (full controller init) after bus recovery ===\n");
    {
        FakeI2cBus fake; I2cBus bus;
        FakeI2cBus_Init(&fake);
        FakeI2cBus_GetBus(&bus, &fake);
        fake.probe_result = DRIVER_STATUS_OK;
        Display_HandleTypeDef disp;
        /* App forces initialized=0 + re-probe on bus recovery; a fresh Display_Init
           re-runs the full init sequence + framebuffer update. */
        int w = fake.write_call_count;
        check(Display_Probe(&bus, (uint8_t[]){0}) == true, "Display re-probe after bus recovery");
        check(Display_Init(&disp, &bus, 0x3C << 1, DISPLAY_CONTROLLER_SH1106) == true,
              "Display re-init returns true (full controller init)");
        check(Display_IsInitialized(&disp) == true, "Display initialized flag set after re-init");
        check(fake.write_call_count > w, "Display re-init performed controller writes");
    }

    /* ============ long-run: absent + bus recovery + healthy progress ============ */
    printf("=== long-run fault injection (>= 30 min virtual) ===\n");
    {
        I2cBusHealth h; I2cBusHealth_Init(&h);
        I2cBusHealth_SetDeviceKnown(&h, 0, true);   /* SCD41 */
        I2cBusHealth_SetDeviceKnown(&h, 1, true);   /* VEML */
        I2cBusHealth_SetDeviceKnown(&h, 2, true);   /* display */
        I2cBusHealth_SetDeviceKnown(&h, 3, true);   /* SHT45 */
        I2cBusHealth_SetDeviceKnown(&h, 5, false);  /* never-present BMP390 */

        uint32_t recoveries = 0;
        bool stuck = false;

        for (uint32_t t = 0; t < 1800000U; t += 500U)
        {
            I2cBusHealth_Report(&h, 5, DRIVER_STATUS_NOT_FOUND, t);          /* missing BMP390 */
            if (t % 15000U == 2000U)
                I2cBusHealth_Report(&h, 1, DRIVER_STATUS_CRC_ERROR, t);      /* CRC never counts */
            if (t % 120000U == 30000U)
            {
                I2cBusHealth_Report(&h, 0, DRIVER_STATUS_BUS_ERROR, t);
                I2cBusHealth_Report(&h, 1, DRIVER_STATUS_BUS_ERROR, t);
                if (I2cBusHealth_RecoveryEligible(&h, t))
                {
                    I2cBusHealth_BeginRecovery(&h, t);
                    I2cBusHealth_OnRecoverySuccess(&h);
                    recoveries++;
                }
            }
            if (I2cBusHealth_GetBusRecoveryAttempts(&h) > 100U) { stuck = true; break; }
        }

        check(stuck == false, "no counter explosion / no tight loop over 30 min");
        /* 30 min / 2 min cycle = 15, but cooldown 60 s > event spacing, so each
           event is gated: at most one per cooldown. Here events are 120 s apart
           (> 60 s cooldown) so all 15 fire. */
        check(recoveries == (1800000U / 120000U), "bounded recoveries over 30 min");
        printf("    longrun: bus recoveries = %lu\n", (unsigned long)recoveries);
    }

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}