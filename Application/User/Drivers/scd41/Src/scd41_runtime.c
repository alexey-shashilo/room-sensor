#include "scd41_runtime.h"
#include "platform_time.h"
#include "recovery_policy.h"
#include <string.h>

/* Non-blocking periodic measurement state machine.

   Scd41Runtime_Start  (App, on boot / retry / recovery) — probe + start.
   Scd41Runtime_Poll   (App, on scheduler tick) — advance state machine.

   Transitions:
     NOT_FOUND --(probe ok, start issued)--> STARTING
     STARTING --(>= data wait, first sample window)--> WAITING
     WAITING  --(data ready + valid read)--> READY
     WAITING/READY --(>= error_threshold consecutive real failures)--> ERROR
     ERROR --(App_DoRetry)--> RECOVERING --(re-start)--> STARTING/WAITING
     READY --(>= stale timeout without a sample)--> WAITING (value invalidated)

   data-ready=false is a VALID result: the runtime stays operational, does NOT
   increment error counters, and does NOT overwrite RoomState. Only real
   communication/CRC/protocol failures are counted as errors (bounded via the
   consecutive-error threshold). */

static void Scd41Runtime_RecordSuccess(Scd41Runtime *rt)
{
    rt->operation_successes++;
    rt->consecutive_errors = 0;
    rt->last_success_ms = Platform_GetTickMs();
}

static void Scd41Runtime_RecordFailure(Scd41Runtime *rt)
{
    rt->operation_failures++;
    rt->consecutive_errors++;
    rt->last_failure_ms = Platform_GetTickMs();
}

/* Escalate to ERROR on a durable failure (threshold reached). The previously
   valid sample is invalidated: the sensor is gone, so its data is no longer
   trustworthy. Numeric last values are retained for diagnostics. */
static void Scd41Runtime_EscalateError(Scd41Runtime *rt)
{
    rt->last_sample.valid = false;
    rt->state = DEVICE_STATE_ERROR;
}

/* Wrap-safe deadline check using subtraction: true when `now` is at/after
   `deadline`, including across the 32-bit tick wrap. */
static bool Scd41Runtime_DeadlinePassed(uint32_t now, uint32_t deadline)
{
    return (uint32_t)(now - deadline) < 0x80000000U;
}

/* Record a real communication/CRC/protocol failure and escalate to ERROR
   immediately when the consecutive-error threshold is reached (rather than
   waiting for a later poll). A failed optional sensor never stops the device,
   but a durable failure degrades health via the ERROR state. */
static void Scd41Runtime_RecordFailureEscalate(Scd41Runtime *rt)
{
    Scd41Runtime_RecordFailure(rt);
    if (rt->consecutive_errors >= SCD41_RUNTIME_ERROR_THRESHOLD)
    {
        rt->phase = SCD41_PHASE_IDLE;
        Scd41Runtime_EscalateError(rt);
    }
}

void Scd41Runtime_Init(Scd41Runtime *rt, const I2cBus *bus)
{
    if (rt == NULL) return;
    memset(rt, 0, sizeof(*rt));
    rt->state = DEVICE_STATE_NOT_FOUND;
    rt->last_tick_ms = Platform_GetTickMs();
    if (bus != NULL)
        SCD41_Init(&rt->dev, bus);
}

bool Scd41Runtime_HasValidSample(const Scd41Runtime *rt)
{
    return rt != NULL && rt->state == DEVICE_STATE_READY && rt->last_sample.valid;
}

bool Scd41Runtime_IsMissing(const Scd41Runtime *rt)
{
    return rt != NULL && rt->state == DEVICE_STATE_NOT_FOUND;
}

/* Freshness: a previously-valid value becomes invalid once no new sample has
   been accepted within the stale timeout. The numeric last value is preserved
   for diagnostics, but validity is cleared. Resume data-ready probing. */
static void Scd41Runtime_EnforceFreshness(Scd41Runtime *rt, uint32_t now)
{
    if (rt->state != DEVICE_STATE_READY)
        return;
    if ((now - rt->last_valid_measurement_ms) >= SCD41_RUNTIME_STALE_MS)
    {
        rt->last_sample.valid = false;
        rt->state = DEVICE_STATE_WAITING;
    }
}

bool Scd41Runtime_ProbeDue(const Scd41Runtime *rt, uint32_t now)
{
    if (rt == NULL) return false;
    if (rt->consecutive_absent == 0U)
        return true;
    if (RecoveryPolicy_Elapsed(now, rt->next_probe_ms, 0U) == false)
        return false;
    return (uint32_t)(now - rt->next_probe_ms) < 0x80000000U;
}

DriverStatus Scd41Runtime_LastError(const Scd41Runtime *rt)
{
    return rt != NULL ? rt->last_error_class : DRIVER_STATUS_INVALID_ARG;
}

DriverStatus Scd41Runtime_Start(Scd41Runtime *rt)
{
    if (rt == NULL)
        return DRIVER_STATUS_INVALID_ARG;

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

    /* Probe first; a missing sensor stays NOT_FOUND (like VEML) and is retried
       by App_DoRetry with per-device backoff — it is not an error escalation. */
    DriverStatus ps = SCD41_Probe(rt->dev.bus);
    if (ps != DRIVER_STATUS_OK)
    {
        rt->state = DEVICE_STATE_NOT_FOUND;
        rt->last_error_class = ps;
        rt->consecutive_absent = RecoveryPolicy_TrackAbsence(rt->consecutive_absent, true);
        rt->next_probe_ms = now + RecoveryPolicy_BackoffMs(rt->consecutive_absent);
        return ps;
    }

    rt->consecutive_absent = 0U;
    rt->last_error_class = DRIVER_STATUS_OK;

    if (rt->dev.initialized == 0U)
        SCD41_Init(&rt->dev, rt->dev.bus);

    /* A normal START_PERIODIC is the cheap, expected path for a freshly-idle
       sensor. Only if it is refused (NACK/AF — the sensor is already in
       periodic measurement mode, e.g. it retained state across an STM32-only
       reboot) do we enter the controlled STOP -> settle -> START recovery. */
    DriverStatus ss = SCD41_StartPeriodicMeasurement(&rt->dev);
    if (ss == DRIVER_STATUS_OK)
    {
        rt->started_at_ms = Platform_GetTickMs();
        rt->state = DEVICE_STATE_STARTING;
        rt->phase = SCD41_PHASE_IDLE;
        Scd41Runtime_RecordSuccess(rt);
        return DRIVER_STATUS_OK;
    }

    /* START was NACKed. This is consistent with the sensor already periodically
       measuring. Issue STOP_PERIODIC (legal during measurement) and hold the
       sensor in idle for the official 500 ms settle before a bounded single
       restart. The App keeps polling the runtime in STARTING state, so this is
       fully non-blocking and the old ERROR->RECOVER->START loop cannot recur. */

    DriverStatus stops = SCD41_StopPeriodicMeasurement(&rt->dev);
    if (stops != DRIVER_STATUS_OK)
    {
        /* Neither START nor STOP is accepted: a genuine communication failure,
           not the retained-periodic case. Escalate via the bounded error path
           (positive/negative threshold semantics unchanged). */
        rt->last_error_class = stops;
        Scd41Runtime_RecordFailure(rt);
        if (rt->consecutive_errors >= SCD41_RUNTIME_ERROR_THRESHOLD)
            rt->state = DEVICE_STATE_ERROR;
        return stops;
    }

    rt->stop_settle_deadline_ms = Platform_GetTickMs() + SCD41_RUNTIME_STOP_SETTLE_MS;
    rt->state = DEVICE_STATE_STARTING;
    rt->phase = SCD41_PHASE_RECOVER_STOP_SETTLE;
    return DRIVER_STATUS_OK;
}

void Scd41Runtime_Poll(Scd41Runtime *rt)
{
    if (rt == NULL) return;

    uint32_t now = Platform_GetTickMs();
    rt->last_tick_ms = now;

    if (rt->state == DEVICE_STATE_STARTING)
    {
        /* Retained-periodic recovery: STOP was accepted, wait the official
           500 ms stop-settle, then issue ONE bounded START_PERIODIC. If the
           restart is accepted we begin the fresh periodic wait; if it is
           refused again we escalate via the bounded error path (no loop into
           another STOP). */
        if (rt->phase == SCD41_PHASE_RECOVER_STOP_SETTLE)
        {
            if (Scd41Runtime_DeadlinePassed(now, rt->stop_settle_deadline_ms) == false)
                return;   /* still settling: do not START before 500 ms */

            DriverStatus rs = SCD41_StartPeriodicMeasurement(&rt->dev);
            rt->phase = SCD41_PHASE_IDLE;
            if (rs != DRIVER_STATUS_OK)
            {
                rt->last_error_class = rs;
                Scd41Runtime_RecordFailureEscalate(rt);
                return;
            }
            rt->started_at_ms = now;   /* restart accepted: begin periodic wait */
            rt->state = DEVICE_STATE_STARTING;
            rt->last_error_class = DRIVER_STATUS_OK;
            Scd41Runtime_RecordSuccess(rt);
            return;
        }

        /* Normal STARTING: wait the periodic interval before the first
           data-ready check (SCD4x first sample ~5 s after start_periodic). */
        if ((now - rt->started_at_ms) >= SCD41_PERIODIC_INTERVAL_MS)
        {
            rt->state = DEVICE_STATE_WAITING;
            rt->phase = SCD41_PHASE_IDLE;
        }
        return;
    }

    if (rt->state == DEVICE_STATE_NOT_FOUND || rt->state == DEVICE_STATE_ERROR ||
        rt->state == DEVICE_STATE_RECOVERING)
        return;

    if (rt->state != DEVICE_STATE_WAITING && rt->state != DEVICE_STATE_READY)
        return;

    Scd41Runtime_EnforceFreshness(rt, now);

    /* State machine error escalation shared by both two-phase paths. */
    if (rt->consecutive_errors >= SCD41_RUNTIME_ERROR_THRESHOLD)
    {
        rt->phase = SCD41_PHASE_IDLE;
        Scd41Runtime_EscalateError(rt);
        return;
    }

    /* Two-phase SCD4x transaction. Each finish() is gated on now >= deadline
       (the ~1 ms command-execution time). No transaction is read before its
       deadline. */
    if (rt->phase == SCD41_PHASE_IDLE)
    {
        /* Begin: send GET_DATA_READY, record the response deadline. */
        DriverStatus bs = SCD41_BeginGetDataReady(&rt->dev);
        if (bs != DRIVER_STATUS_OK)
        {
            rt->last_error_class = bs;
            Scd41Runtime_RecordFailureEscalate(rt);
            return;
        }
        rt->deadline_ms = now + SCD41_COMMAND_RESPONSE_DELAY_MS;
        rt->phase = SCD41_PHASE_WAIT_DATA_READY_RESPONSE;
        return;   /* return to scheduler; read happens on a later tick */
    }

    if (rt->phase == SCD41_PHASE_WAIT_DATA_READY_RESPONSE)
    {
        if (Scd41Runtime_DeadlinePassed(now, rt->deadline_ms) == false)
            return;   /* not yet elapsed: do not read before 1 ms */

        bool new_data = false;
        DriverStatus fs = SCD41_FinishGetDataReady(&rt->dev, &new_data);
        if (fs != DRIVER_STATUS_OK)
        {
            rt->phase = SCD41_PHASE_IDLE;
            rt->last_error_class = fs;
            Scd41Runtime_RecordFailureEscalate(rt);
            return;
        }
        if (!new_data)
        {
            rt->phase = SCD41_PHASE_IDLE;
            return;   /* data-ready=false is not an error; stay operational */
        }

        /* Data ready: begin the measurement read transaction. */
        DriverStatus mbs = SCD41_BeginReadMeasurement(&rt->dev);
        if (mbs != DRIVER_STATUS_OK)
        {
            rt->phase = SCD41_PHASE_IDLE;
            rt->last_error_class = mbs;
            Scd41Runtime_RecordFailureEscalate(rt);
            return;
        }
        rt->deadline_ms = now + SCD41_COMMAND_RESPONSE_DELAY_MS;
        rt->phase = SCD41_PHASE_WAIT_MEASUREMENT_RESPONSE;
        return;
    }

    if (rt->phase == SCD41_PHASE_WAIT_MEASUREMENT_RESPONSE)
    {
        if (Scd41Runtime_DeadlinePassed(now, rt->deadline_ms) == false)
            return;   /* not yet elapsed: no read */

        Scd41Measurement meas;
        DriverStatus rs = SCD41_FinishReadMeasurement(&rt->dev, &meas);
        rt->phase = SCD41_PHASE_IDLE;
        if (rs != DRIVER_STATUS_OK)
        {
            /* CRC failure included: no partial sample is committed. Escalate via
               the same bounded consecutive-error threshold. */
            rt->last_error_class = rs;
            Scd41Runtime_RecordFailureEscalate(rt);
            return;
        }

        if (meas.valid)
        {
            rt->last_sample = meas;
            rt->last_valid_measurement_ms = now;
            rt->state = DEVICE_STATE_READY;
            rt->last_error_class = DRIVER_STATUS_OK;
            Scd41Runtime_RecordSuccess(rt);
        }
    }
}

/* Bounded recovery: escalate ERROR (or a stuck STARTING) into RECOVERING, reset
   the consecutive-error budget so the next probe is a NEW epoch, and increment
   the recovery counter. App_DoRetry then calls Start() to re-probe and restart
   periodic measurement. One SCD41 failure never resets the MCU or stops
   VEML/display/App.

   LAST-GOOD POLICY (coherent with bus recovery, Phase 13): entering a recovery
   epoch invalidates the last-good sample, so after a shared-bus reset no sensor
   keeps a stale "last-good" while another invalidates. A fresh sample is
   required before any value is trusted again. */
void Scd41Runtime_Recover(Scd41Runtime *rt)
{
    if (rt == NULL) return;
    rt->recovery_count++;
    rt->consecutive_errors = 0U;
    rt->last_sample.valid = false;
    rt->state = DEVICE_STATE_RECOVERING;
}

void Scd41Runtime_GetDiagnostics(const Scd41Runtime *rt, DeviceRuntime *out)
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