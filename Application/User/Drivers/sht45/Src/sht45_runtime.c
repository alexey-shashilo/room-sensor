#include "sht45_runtime.h"
#include "platform_time.h"
#include <string.h>

/* Non-blocking single-shot measurement state machine.

   Transitions:
     NOT_FOUND --(probe ok)--> STARTING
     STARTING --(measure issued; conversion deadline passes)--> READY(if valid) / retry
     READY --(measure interval passes)--> STARTING (next single-shot cycle)
     READY/STARTING --(>= error_threshold consecutive real failures)--> ERROR
     ERROR --(App recovery)--> RECOVERING --(re-start)--> STARTING
     READY --(>= stale timeout without a fresh sample)--> value invalidated

   Only real communication/CRC/protocol failures are counted as errors (bounded
   via the consecutive-error threshold). A read that is refused because the
   conversion is still in progress (NACK / NOT_READY) is NOT an error. */

static void Sht45Runtime_RecordSuccess(Sht45Runtime *rt)
{
    rt->operation_successes++;
    rt->consecutive_errors = 0;
    rt->last_success_ms = Platform_GetTickMs();
}

static void Sht45Runtime_RecordFailure(Sht45Runtime *rt)
{
    rt->operation_failures++;
    rt->consecutive_errors++;
    rt->last_failure_ms = Platform_GetTickMs();
}

/* Escalate to ERROR on a durable failure (threshold reached). A previously-valid
   sample is invalidated (the sensor is not trustworthy). Numeric last values are
   retained for diagnostics. */
static void Sht45Runtime_EscalateError(Sht45Runtime *rt)
{
    rt->last_sample.valid = false;
    rt->state = DEVICE_STATE_ERROR;
}

/* Wrap-safe deadline check using subtraction. */
static bool Sht45Runtime_DeadlinePassed(uint32_t now, uint32_t deadline)
{
    return (uint32_t)(now - deadline) < 0x80000000U;
}

static void Sht45Runtime_RecordFailureEscalate(Sht45Runtime *rt)
{
    Sht45Runtime_RecordFailure(rt);
    if (rt->consecutive_errors >= SHT45_RUNTIME_ERROR_THRESHOLD)
    {
        rt->phase = SHT45_PHASE_IDLE;
        Sht45Runtime_EscalateError(rt);
    }
}

void Sht45Runtime_Init(Sht45Runtime *rt, const I2cBus *bus)
{
    if (rt == NULL) return;
    memset(rt, 0, sizeof(*rt));
    rt->state = DEVICE_STATE_NOT_FOUND;
    rt->last_tick_ms = Platform_GetTickMs();
    if (bus != NULL)
        SHT45_Init(&rt->dev, bus);
}

bool Sht45Runtime_HasValidSample(const Sht45Runtime *rt)
{
    /* Sample validity is decoupled from the STARTING/READY transaction state:
       a last-good sample stays valid while the next conversion is in progress.
       'last_sample.valid' is cleared only by stale timeout, durable ERROR,
       confirmed-missing, or explicit invalidation. */
    return rt != NULL && rt->last_sample.valid;
}

bool Sht45Runtime_IsMissing(const Sht45Runtime *rt)
{
    return rt != NULL && rt->state == DEVICE_STATE_NOT_FOUND;
}

void Sht45Runtime_InvalidateSample(Sht45Runtime *rt)
{
    if (rt == NULL) return;
    rt->last_sample.valid = false;
}

/* A previously-valid value becomes invalid once no fresh sample has been
   accepted within the stale timeout. Numeric value preserved; validity cleared.
   Based PURELY on last_valid_measurement_ms, independent of the transaction
   state, so a sample can be invalidated even while a conversion/retry is in
   progress. The in-flight measurement is NOT aborted: if it later produces a
   fresh sample, validity is re-established from that sample. */
static void Sht45Runtime_EnforceFreshness(Sht45Runtime *rt, uint32_t now)
{
    if (rt->last_sample.valid == false)
        return;
    if ((now - rt->last_valid_measurement_ms) >= SHT45_RUNTIME_STALE_MS)
        rt->last_sample.valid = false;
}

DriverStatus Sht45Runtime_Start(Sht45Runtime *rt)
{
    if (rt == NULL)
        return DRIVER_STATUS_INVALID_ARG;

    DriverStatus ps = SHT45_Probe(rt->dev.bus);
    if (ps != DRIVER_STATUS_OK)
    {
        rt->state = DEVICE_STATE_NOT_FOUND;
        return ps;
    }

    if (rt->dev.initialized == 0U)
        SHT45_Init(&rt->dev, rt->dev.bus);

    /* Issue the first measurement command; the read happens on a later tick once
       the conversion deadline has passed. */
    DriverStatus ms = SHT45_BeginMeasurement(&rt->dev);
    if (ms != DRIVER_STATUS_OK)
    {
        Sht45Runtime_RecordFailure(rt);
        if (rt->consecutive_errors >= SHT45_RUNTIME_ERROR_THRESHOLD)
            rt->state = DEVICE_STATE_ERROR;
        return ms;
    }

    rt->deadline_ms = Platform_GetTickMs() + SHT45_MEASUREMENT_DURATION_MS;
    rt->state = DEVICE_STATE_STARTING;
    rt->phase = SHT45_PHASE_MEASURING;
    return DRIVER_STATUS_OK;
}

void Sht45Runtime_Poll(Sht45Runtime *rt)
{
    if (rt == NULL) return;

    uint32_t now = Platform_GetTickMs();
    rt->last_tick_ms = now;

    if (rt->state == DEVICE_STATE_NOT_FOUND || rt->state == DEVICE_STATE_ERROR ||
        rt->state == DEVICE_STATE_RECOVERING)
        return;

    Sht45Runtime_EnforceFreshness(rt, now);

    if (rt->consecutive_errors >= SHT45_RUNTIME_ERROR_THRESHOLD)
    {
        rt->phase = SHT45_PHASE_IDLE;
        Sht45Runtime_EscalateError(rt);
        return;
    }

    /* STARTING: measurement command in flight; read once the conversion
       deadline has elapsed. */
    if (rt->state == DEVICE_STATE_STARTING)
    {
        if (rt->phase == SHT45_PHASE_MEASURING)
        {
            if (Sht45Runtime_DeadlinePassed(now, rt->deadline_ms) == false)
                return;   /* conversion not done: do not read yet */

            Sht45Measurement m;
            DriverStatus rs = SHT45_FinishMeasurement(&rt->dev, &m);
            rt->phase = SHT45_PHASE_IDLE;
            if (rs != DRIVER_STATUS_OK)
            {
                if (rs == DRIVER_STATUS_BUS_ERROR)
                {
                    /* Genuine I2C failure: count it. If it repeats enough times
                       we escalate to ERROR; otherwise the next poll re-issues
                       the measurement (bounded retry). */
                    Sht45Runtime_RecordFailure(rt);
                    if (rt->consecutive_errors >= SHT45_RUNTIME_ERROR_THRESHOLD)
                    {
                        Sht45Runtime_EscalateError(rt);
                        return;
                    }
                }
                else if (rs == DRIVER_STATUS_NOT_READY)
                {
                    /* Conversion not complete / read refused: not an error,
                       retry the read next poll. */
                    rt->deadline_ms = now + SHT45_MEASUREMENT_DURATION_MS;
                    rt->phase = SHT45_PHASE_MEASURING;
                    rt->state = DEVICE_STATE_STARTING;
                    return;
                }
                else
                {
                    /* CRC or other protocol failure: bounded escalation. */
                    Sht45Runtime_RecordFailureEscalate(rt);
                    return;
                }
                /* Fall through: re-issue the measurement for the retry. */
                DriverStatus ms = SHT45_BeginMeasurement(&rt->dev);
                if (ms != DRIVER_STATUS_OK)
                {
                    Sht45Runtime_RecordFailureEscalate(rt);
                    return;
                }
                rt->deadline_ms = Platform_GetTickMs() + SHT45_MEASUREMENT_DURATION_MS;
                rt->phase = SHT45_PHASE_MEASURING;
                rt->state = DEVICE_STATE_STARTING;
                return;
            }

            if (!m.valid)
            {
                /* Decoded but flagged invalid: retry. */
                return;
            }

            /* Sample accepted: commit, then wait the measure interval. */
            rt->last_sample = m;
            rt->last_valid_measurement_ms = now;
            rt->state = DEVICE_STATE_READY;
            rt->phase = SHT45_PHASE_BETWEEN_MEASUREMENTS;
            Sht45Runtime_RecordSuccess(rt);
            return;
        }
        return;
    }

    /* READY: after the measurement interval, start the next cycle. */
    if (rt->state == DEVICE_STATE_READY)
    {
        if (rt->phase == SHT45_PHASE_BETWEEN_MEASUREMENTS)
        {
            if ((now - rt->last_valid_measurement_ms) < SHT45_RUNTIME_MEASUREMENT_INTERVAL_MS)
                return;   /* not yet time for the next sample */

            DriverStatus ms = SHT45_BeginMeasurement(&rt->dev);
            if (ms != DRIVER_STATUS_OK)
            {
                Sht45Runtime_RecordFailureEscalate(rt);
                return;
            }
            rt->deadline_ms = now + SHT45_MEASUREMENT_DURATION_MS;
            rt->phase = SHT45_PHASE_MEASURING;
            rt->state = DEVICE_STATE_STARTING;
            return;
        }
        return;
    }
}

void Sht45Runtime_GetDiagnostics(const Sht45Runtime *rt, DeviceRuntime *out)
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