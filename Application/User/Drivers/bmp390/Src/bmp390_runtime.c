#include "bmp390_runtime.h"
#include "platform_time.h"
#include "recovery_policy.h"
#include <string.h>

/* Non-blocking FORCED-mode measurement state machine. */

static bool DeadlinePassed(uint32_t now, uint32_t deadline)
{
    return (uint32_t)(now - deadline) < 0x80000000U;
}

static void RecordSuccess(Bmp390Runtime *rt)
{
    rt->operation_successes++;
    rt->consecutive_errors = 0;
    rt->last_success_ms = Platform_GetTickMs();
}

static void RecordFailure(Bmp390Runtime *rt)
{
    rt->operation_failures++;
    rt->consecutive_errors++;
    rt->last_failure_ms = Platform_GetTickMs();
}

static void EscalateError(Bmp390Runtime *rt)
{
    rt->last_sample.valid = false;
    rt->state = DEVICE_STATE_ERROR;
}

void Bmp390Runtime_Init(Bmp390Runtime *rt, const I2cBus *bus)
{
    if (rt == NULL) return;
    memset(rt, 0, sizeof(*rt));
    rt->state = DEVICE_STATE_NOT_FOUND;
    rt->last_tick_ms = Platform_GetTickMs();
    if (bus != NULL)
        BMP390_Init(&rt->dev, bus);
}

bool Bmp390Runtime_HasValidSample(const Bmp390Runtime *rt)
{
    return rt != NULL && rt->last_sample.valid;
}

bool Bmp390Runtime_IsMissing(const Bmp390Runtime *rt)
{
    return rt != NULL && rt->state == DEVICE_STATE_NOT_FOUND;
}

void Bmp390Runtime_InvalidateSample(Bmp390Runtime *rt)
{
    if (rt == NULL) return;
    rt->last_sample.valid = false;
}

/* Sample validity cleared only when no fresh sample arrives within the stale
   timeout — independent of the transaction state. */
static void EnforceFreshness(Bmp390Runtime *rt, uint32_t now)
{
    if (rt->last_sample.valid == false) return;
    if ((now - rt->last_valid_measurement_ms) >= BMP390_RUNTIME_STALE_MS)
        rt->last_sample.valid = false;
}

bool Bmp390Runtime_ProbeDue(const Bmp390Runtime *rt, uint32_t now)
{
    if (rt == NULL) return false;
    if (rt->consecutive_absent == 0U)
        return true;
    if (RecoveryPolicy_Elapsed(now, rt->next_probe_ms, 0U) == false)
        return false;
    return (uint32_t)(now - rt->next_probe_ms) < 0x80000000U;
}

DriverStatus Bmp390Runtime_LastError(const Bmp390Runtime *rt)
{
    return rt != NULL ? rt->last_error_class : DRIVER_STATUS_INVALID_ARG;
}

DriverStatus Bmp390Runtime_Start(Bmp390Runtime *rt)
{
    if (rt == NULL) return DRIVER_STATUS_INVALID_ARG;

    uint32_t now = Platform_GetTickMs();

    /* Phase 4 NOT_FOUND backoff: gate the re-probe behind the per-device ladder
       when this device is in a confirmed-absent episode. */
    if (rt->consecutive_absent > 0U)
    {
        if (RecoveryPolicy_Elapsed(now, rt->next_probe_ms, 0U) == false ||
            (uint32_t)(now - rt->next_probe_ms) >= 0x80000000U)
        {
            rt->state = DEVICE_STATE_NOT_FOUND;
            return DRIVER_STATUS_NOT_FOUND;
        }
    }

    DriverStatus s = BMP390_Detect(&rt->dev);
    if (s != DRIVER_STATUS_OK)
    {
        rt->state = DEVICE_STATE_NOT_FOUND;
        rt->last_error_class = s;
        rt->consecutive_absent = RecoveryPolicy_TrackAbsence(rt->consecutive_absent, true);
        rt->next_probe_ms = now + RecoveryPolicy_BackoffMs(rt->consecutive_absent);
        return s;
    }

    rt->consecutive_absent = 0U;
    rt->last_error_class = DRIVER_STATUS_OK;

    /* Identity OK: (re)load calibration + configure profile, then trigger the
       first measurement. */
    if (rt->dev.calib.calibrated == 0U)
    {
        s = BMP390_InitCalibration(&rt->dev);
        if (s != DRIVER_STATUS_OK)
        {
            rt->last_error_class = s;
            return s;
        }
    }
    s = BMP390_ConfigureRoomProfile(&rt->dev);
    if (s != DRIVER_STATUS_OK)
    {
        rt->last_error_class = s;
        RecordFailure(rt);
        if (rt->consecutive_errors >= BMP390_RUNTIME_ERROR_THRESHOLD)
            rt->state = DEVICE_STATE_ERROR;
        return s;
    }

    s = BMP390_TriggerMeasurement(&rt->dev);
    if (s != DRIVER_STATUS_OK)
    {
        rt->last_error_class = s;
        RecordFailure(rt);
        if (rt->consecutive_errors >= BMP390_RUNTIME_ERROR_THRESHOLD)
            rt->state = DEVICE_STATE_ERROR;
        return s;
    }

    /* Data-ready expected shortly after trigger. */
    rt->deadline_ms = Platform_GetTickMs() + BMP390_RUNTIME_MEASUREMENT_DEADLINE_MS;
    rt->phase = BMP390_PHASE_MEASURING;
    rt->state = DEVICE_STATE_STARTING;
    return DRIVER_STATUS_OK;
}

void Bmp390Runtime_Recover(Bmp390Runtime *rt)
{
    if (rt == NULL) return;
    rt->recovery_count++;
    rt->consecutive_errors = 0;
    /* LAST-GOOD POLICY (Phase 13): entering a recovery epoch invalidates the
       last-good sample so after a shared-bus reset all I2C sensors clear last-good
       uniformly; a fresh sample must be obtained before any value is trusted. */
    rt->last_sample.valid = false;
    rt->phase = BMP390_PHASE_IDLE;
    rt->state = DEVICE_STATE_RECOVERING;
}

void Bmp390Runtime_Poll(Bmp390Runtime *rt)
{
    if (rt == NULL) return;

    uint32_t now = Platform_GetTickMs();
    rt->last_tick_ms = now;

    if (rt->state == DEVICE_STATE_NOT_FOUND ||
        rt->state == DEVICE_STATE_ERROR ||
        rt->state == DEVICE_STATE_RECOVERING)
        return;

    EnforceFreshness(rt, now);

    if (rt->consecutive_errors >= BMP390_RUNTIME_ERROR_THRESHOLD)
    {
        rt->phase = BMP390_PHASE_IDLE;
        EscalateError(rt);
        return;
    }

    /* ---------- MEASURING: await data-ready, then read a paired sample ------ */
    if (rt->state == DEVICE_STATE_STARTING && rt->phase == BMP390_PHASE_MEASURING)
    {
        if (DeadlinePassed(now, rt->deadline_ms) == false)
            return;

        uint8_t status = 0;
        DriverStatus rs = BMP390_ReadStatus(&rt->dev, &status);
        if (rs != DRIVER_STATUS_OK)
        {
            rt->last_error_class = rs;
            if (rs == DRIVER_STATUS_DEVICE_ERROR)
            {
                RecordFailure(rt);
                if (rt->consecutive_errors >= BMP390_RUNTIME_ERROR_THRESHOLD)
                    EscalateError(rt);
            }
            else
            {
                RecordFailure(rt);
                if (rt->consecutive_errors >= BMP390_RUNTIME_ERROR_THRESHOLD)
                    EscalateError(rt);
            }
            rt->phase = BMP390_PHASE_MEASURING;
            rt->deadline_ms = now + BMP390_RUNTIME_MEASUREMENT_DEADLINE_MS;
            return;
        }

        /* Both pressure AND temperature data-ready required for a paired sample. */
        if ((status & (BMP390_STATUS_DRDY_PRESS | BMP390_STATUS_DRDY_TEMP)) !=
            (BMP390_STATUS_DRDY_PRESS | BMP390_STATUS_DRDY_TEMP))
        {
            /* not ready yet: not an error, keep waiting */
            rt->deadline_ms = now + BMP390_RUNTIME_MEASUREMENT_DEADLINE_MS;
            return;
        }

        /* Device-level fault: fatal/command/config error bits in the ERR register
           mean the addressed device communicated OK but reported an internal
           fault. This is DRIVER_STATUS_DEVICE_ERROR, NOT a transport error. A
           command/config error can be recovered by reconfiguring; a fatal error
           requires recovery. Count it as a bounded failure. */
        {
            uint8_t err = 0;
            DriverStatus er = BMP390_ReadError(&rt->dev, &err);
            if (er == DRIVER_STATUS_DEVICE_ERROR)
            {
                rt->last_error_class = er;
                RecordFailure(rt);
                if (rt->consecutive_errors >= BMP390_RUNTIME_ERROR_THRESHOLD)
                    EscalateError(rt);
                rt->phase = BMP390_PHASE_MEASURING;
                rt->deadline_ms = now + BMP390_RUNTIME_MEASUREMENT_DEADLINE_MS;
                /* Reconfigure to clear a recoverable cmd/conf error. */
                BMP390_ConfigureRoomProfile(&rt->dev);
                return;
            }
            else if (er != DRIVER_STATUS_OK)
            {
                rt->last_error_class = er;
                RecordFailure(rt);
                if (rt->consecutive_errors >= BMP390_RUNTIME_ERROR_THRESHOLD)
                    EscalateError(rt);
                rt->phase = BMP390_PHASE_MEASURING;
                rt->deadline_ms = now + BMP390_RUNTIME_MEASUREMENT_DEADLINE_MS;
                return;
            }
        }

        Bmp390Sample sample;
        DriverStatus rr = BMP390_ReadSample(&rt->dev, &sample);
        rt->phase = BMP390_PHASE_BETWEEN_MEASUREMENTS;
        if (rr != DRIVER_STATUS_OK)
        {
            rt->last_error_class = rr;
            RecordFailure(rt);
            if (rt->consecutive_errors >= BMP390_RUNTIME_ERROR_THRESHOLD)
                EscalateError(rt);
            rt->deadline_ms = now + BMP390_RUNTIME_MEASUREMENT_DEADLINE_MS;
            return;
        }

        if (sample.valid == false)
        {
            /* Compensation/plausibility failure (out-of-range or non-finite).
               Do NOT publish an implausible pressure/temperature to RoomState.
               Bounded retry. */
            rt->last_error_class = DRIVER_STATUS_CRC_ERROR;
            RecordFailure(rt);
            if (rt->consecutive_errors >= BMP390_RUNTIME_ERROR_THRESHOLD)
                EscalateError(rt);
            rt->deadline_ms = now + BMP390_RUNTIME_MEASUREMENT_DEADLINE_MS;
            return;
        }

        rt->last_sample = sample;
        rt->last_valid_measurement_ms = now;
        rt->state = DEVICE_STATE_READY;
        rt->last_error_class = DRIVER_STATUS_OK;
        RecordSuccess(rt);
        return;
    }

    /* ---------- READY: after the interval, trigger the next measurement ----- */
    if (rt->state == DEVICE_STATE_READY && rt->phase == BMP390_PHASE_BETWEEN_MEASUREMENTS)
    {
        if ((now - rt->last_valid_measurement_ms) < BMP390_RUNTIME_MEASUREMENT_INTERVAL_MS)
            return;

        DriverStatus ms = BMP390_TriggerMeasurement(&rt->dev);
        if (ms != DRIVER_STATUS_OK)
        {
            rt->last_error_class = ms;
            RecordFailure(rt);
            if (rt->consecutive_errors >= BMP390_RUNTIME_ERROR_THRESHOLD)
                EscalateError(rt);
            return;
        }
        rt->deadline_ms = now + BMP390_RUNTIME_MEASUREMENT_DEADLINE_MS;
        rt->phase = BMP390_PHASE_MEASURING;
        rt->state = DEVICE_STATE_STARTING;
        return;
    }
}

void Bmp390Runtime_GetDiagnostics(const Bmp390Runtime *rt, DeviceRuntime *out)
{
    if (rt == NULL || out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->state = rt->state;
    out->operation_successes = rt->operation_successes;
    out->operation_failures  = rt->operation_failures;
    out->consecutive_errors  = rt->consecutive_errors;
    out->recovery_count      = rt->recovery_count;
    out->last_success_ms     = rt->last_success_ms;
    out->last_failure_ms     = rt->last_failure_ms;
}