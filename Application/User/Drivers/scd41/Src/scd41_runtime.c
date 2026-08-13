#include "scd41_runtime.h"
#include "platform_time.h"
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

DriverStatus Scd41Runtime_Start(Scd41Runtime *rt)
{
    if (rt == NULL)
        return DRIVER_STATUS_INVALID_ARG;

    /* Probe first; a missing sensor stays NOT_FOUND (like VEML) and is retried
       by App_DoRetry — it is not an error escalation. */
    DriverStatus ps = SCD41_Probe(rt->dev.bus);
    if (ps != DRIVER_STATUS_OK)
    {
        rt->state = DEVICE_STATE_NOT_FOUND;
        return ps;
    }

    if (rt->dev.initialized == 0U)
        SCD41_Init(&rt->dev, rt->dev.bus);

    /* Issue start_periodic_measurement. Non-blocking (single I2C write). */
    DriverStatus ss = SCD41_StartPeriodicMeasurement(&rt->dev);
    if (ss != DRIVER_STATUS_OK)
    {
        Scd41Runtime_RecordFailure(rt);
        if (rt->consecutive_errors >= SCD41_RUNTIME_ERROR_THRESHOLD)
            rt->state = DEVICE_STATE_ERROR;
        return ss;
    }

    rt->started_at_ms = Platform_GetTickMs();
    rt->state = DEVICE_STATE_STARTING;
    Scd41Runtime_RecordSuccess(rt);
    return DRIVER_STATUS_OK;
}

void Scd41Runtime_Poll(Scd41Runtime *rt)
{
    if (rt == NULL) return;

    uint32_t now = Platform_GetTickMs();
    rt->last_tick_ms = now;

    if (rt->state == DEVICE_STATE_STARTING)
    {
        /* Wait the periodic interval before the first data-ready check: the
           SCD4x produces its first sample ~5 s after start_periodic. */
        if ((now - rt->started_at_ms) >= SCD41_PERIODIC_INTERVAL_MS)
            rt->state = DEVICE_STATE_WAITING;
        return;
    }

    if (rt->state == DEVICE_STATE_NOT_FOUND || rt->state == DEVICE_STATE_ERROR ||
        rt->state == DEVICE_STATE_RECOVERING)
        return;

    if (rt->state != DEVICE_STATE_WAITING && rt->state != DEVICE_STATE_READY)
        return;

    Scd41Runtime_EnforceFreshness(rt, now);

    bool new_data = false;
    DriverStatus qs = SCD41_GetDataReady(&rt->dev, &new_data);
    if (qs != DRIVER_STATUS_OK)
    {
        Scd41Runtime_RecordFailure(rt);
        if (rt->consecutive_errors >= SCD41_RUNTIME_ERROR_THRESHOLD)
            Scd41Runtime_EscalateError(rt);
        return;
    }

    if (!new_data)
        return;   /* not an error — keep operational */

    Scd41Measurement meas;
    DriverStatus rs = SCD41_ReadMeasurement(&rt->dev, &meas);
    if (rs != DRIVER_STATUS_OK)
    {
        /* CRC failure included: no partial sample is committed. Escalate via the
           same bounded consecutive-error threshold. */
        Scd41Runtime_RecordFailure(rt);
        if (rt->consecutive_errors >= SCD41_RUNTIME_ERROR_THRESHOLD)
            Scd41Runtime_EscalateError(rt);
        return;
    }

    if (meas.valid)
    {
        rt->last_sample = meas;
        rt->last_valid_measurement_ms = now;
        rt->state = DEVICE_STATE_READY;
        Scd41Runtime_RecordSuccess(rt);
    }
}

/* Bounded recovery: escalate ERROR (or a stuck STARTING) into RECOVERING and
   increment the recovery counter. App_DoRetry then calls Start() to re-probe
   and restart periodic measurement. One SCD41 failure never resets the MCU or
   stops VEML/display/App. */
void Scd41Runtime_Recover(Scd41Runtime *rt)
{
    if (rt == NULL) return;
    rt->recovery_count++;
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