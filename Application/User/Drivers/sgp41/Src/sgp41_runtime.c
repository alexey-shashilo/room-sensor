#include "sgp41_runtime.h"
#include "platform_time.h"
#include "recovery_policy.h"
#include <string.h>

/* Non-blocking SGP41 VOC/NOx measurement + gas-index state machine.

   Transitions:
     NOT_FOUND --(probe ok, start)--> STARTING (conditioning)
     STARTING --(conditioning = 10s)--> WAITING
     WAITING  --(first valid raw sample)--> READY
     READY    --(1 Hz cadence)--> stays READY (new raw + index feed)
     READY/WAITING/STARTING --(>= error_threshold consecutive real failures)--> ERROR
     ERROR --(Sgp41Runtime_Recover)--> RECOVERING --(Start)--> STARTING
     READY --(>= stale timeout without a fresh sample)--> raw invalidated

   Sampling contract: the gas-index algorithm assumes a constant 1 Hz input.
   The runtime issues one measure_raw_signals per second and feeds the result to
   the gas-index once per second (never back-to-back on a fast poll). Only real
   communication/CRC/protocol failures are counted as errors (bounded via the
   consecutive-error threshold).

   RAW vs INDEX validity:
     - last_sample.valid is the raw measurement gate.
     - voc_index_valid / nox_index_valid require the specific gas-index channel
       to be OUT of blackout (VOC 45 s; NOx also requires conditioning done) AND
       a fresh raw sample. Blackout/conditioning values are never reported valid.
   */

/* Wrap-safe deadline check. */
static bool Sgp41Runtime_DeadlinePassed(uint32_t now, uint32_t deadline)
{
    return (uint32_t)(now - deadline) < 0x80000000U;
}

void Sgp41Runtime_RecordSuccess(Sgp41Runtime *rt)
{
    rt->operation_successes++;
    rt->consecutive_errors = 0;
    rt->last_success_ms = Platform_GetTickMs();
}

void Sgp41Runtime_RecordFailure(Sgp41Runtime *rt)
{
    rt->operation_failures++;
    rt->consecutive_errors++;
    rt->last_failure_ms = Platform_GetTickMs();
}

static void Sgp41Runtime_EscalateError(Sgp41Runtime *rt)
{
    rt->last_sample.valid = false;
    rt->voc_index_valid = false;
    rt->nox_index_valid = false;
    rt->state = DEVICE_STATE_ERROR;
}

void Sgp41Runtime_Init(Sgp41Runtime *rt, const I2cBus *bus)
{
    if (rt == NULL) return;
    memset(rt, 0, sizeof(*rt));
    rt->state = DEVICE_STATE_NOT_FOUND;
    rt->last_tick_ms = Platform_GetTickMs();
    if (bus != NULL)
        SGP41_Init(&rt->dev, bus);
}

void SGP41_CompensationToTicks(const Sgp41Compensation *comp,
                               uint16_t *rh_ticks, uint16_t *t_ticks)
{
    uint16_t rh = SGP41_DEFAULT_RH_TICKS;
    uint16_t t = SGP41_DEFAULT_T_TICKS;

    if (comp != NULL && comp->valid)
    {
        float rhp = comp->relative_humidity_pct;
        float tc = comp->temperature_c;
        bool rh_ok = (rhp >= 0.0f) && (rhp <= 100.0f);
        bool t_ok = (tc >= -45.0f) && (tc <= 130.0f);
        if (rh_ok)
        {
            float ticks = rhp * 65535.0f / 100.0f;
            if (ticks < 0.0f) ticks = 0.0f;
            else if (ticks > 65535.0f) ticks = 65535.0f;
            rh = (uint16_t)(ticks + 0.5f);
        }
        if (t_ok)
        {
            float ticks = (tc + 45.0f) * 65535.0f / 175.0f;
            if (ticks < 0.0f) ticks = 0.0f;
            else if (ticks > 65535.0f) ticks = 65535.0f;
            t = (uint16_t)(ticks + 0.5f);
        }
    }

    if (rh_ticks) *rh_ticks = rh;
    if (t_ticks) *t_ticks = t;
}

void Sgp41Runtime_SetCompensation(Sgp41Runtime *rt, const Sgp41Compensation *comp)
{
    if (rt == NULL) return;
    SGP41_CompensationToTicks(comp, &rt->comp_rh_ticks, &rt->comp_t_ticks);
    rt->comp_seen = true;
}

bool Sgp41Runtime_HasValidSample(const Sgp41Runtime *rt)
{
    return rt != NULL && rt->last_sample.valid;
}

bool Sgp41Runtime_HasValidVocIndex(const Sgp41Runtime *rt)
{
    return rt != NULL && rt->voc_index_valid && rt->last_sample.valid;
}

bool Sgp41Runtime_HasValidNoxIndex(const Sgp41Runtime *rt)
{
    return rt != NULL && rt->nox_index_valid && rt->last_sample.valid;
}

bool Sgp41Runtime_IsMissing(const Sgp41Runtime *rt)
{
    return rt != NULL && rt->state == DEVICE_STATE_NOT_FOUND;
}

void Sgp41Runtime_InvalidateSample(Sgp41Runtime *rt)
{
    if (rt == NULL) return;
    rt->last_sample.valid = false;
    rt->voc_index_valid = false;
    rt->nox_index_valid = false;
}

/* Freshness: a previously-valid raw sample becomes invalid once no fresh sample
   has been accepted within the stale timeout. The index validity follows (an
   index without a fresh raw backing is not exposed). */
static void Sgp41Runtime_EnforceFreshness(Sgp41Runtime *rt, uint32_t now)
{
    if (rt->last_sample.valid == false)
        return;
    if ((now - rt->last_valid_measurement_ms) >= SGP41_RUNTIME_STALE_MS)
    {
        rt->last_sample.valid = false;
        rt->voc_index_valid = false;
        rt->nox_index_valid = false;
    }
}

bool Sgp41Runtime_ProbeDue(const Sgp41Runtime *rt, uint32_t now)
{
    if (rt == NULL) return false;
    if (rt->consecutive_absent == 0U)
        return true;
    if (RecoveryPolicy_Elapsed(now, rt->next_probe_ms, 0U) == false)
        return false;
    return (uint32_t)(now - rt->next_probe_ms) < 0x80000000U;
}

DriverStatus Sgp41Runtime_LastError(const Sgp41Runtime *rt)
{
    return rt != NULL ? rt->last_error_class : DRIVER_STATUS_INVALID_ARG;
}

/* Begin the conditioning or, if the conditioning budget is used up, move
   straight to measurement. Returns OK once the first transaction is in flight. */
static DriverStatus Sgp41Runtime_BeginStartup(Sgp41Runtime *rt)
{
    if (rt->conditioning_ms < SGP41_RUNTIME_CONDITIONING_MAX_MS)
    {
        DriverStatus cs = SGP41_BeginConditioning(&rt->dev,
                                                  rt->comp_rh_ticks,
                                                  rt->comp_t_ticks);
        if (cs != DRIVER_STATUS_OK)
            return cs;
        rt->phase = SGP41_PHASE_CONDITIONING;
        rt->deadline_ms = Platform_GetTickMs() + SGP41_CONDITIONING_EXECUTION_MS;
        return DRIVER_STATUS_OK;
    }

    /* Conditioning complete: begin measurement. */
    DriverStatus ms = SGP41_BeginMeasure(&rt->dev, rt->comp_rh_ticks, rt->comp_t_ticks);
    if (ms != DRIVER_STATUS_OK)
        return ms;
    rt->phase = SGP41_PHASE_MEASURING;
    rt->deadline_ms = Platform_GetTickMs() + SGP41_MEASURE_EXECUTION_MS;
    return DRIVER_STATUS_OK;
}

DriverStatus Sgp41Runtime_Start(Sgp41Runtime *rt)
{
    if (rt == NULL)
        return DRIVER_STATUS_INVALID_ARG;

    uint32_t now = Platform_GetTickMs();

    /* Phase 4 NOT_FOUND backoff: gate the re-probe behind the per-device ladder
       when in a confirmed-absent episode. */
    if (rt->consecutive_absent > 0U)
    {
        if (RecoveryPolicy_Elapsed(now, rt->next_probe_ms, 0U) == false ||
            (uint32_t)(now - rt->next_probe_ms) >= 0x80000000U)
        {
            rt->state = DEVICE_STATE_NOT_FOUND;
            return DRIVER_STATUS_NOT_FOUND;
        }
    }

    DriverStatus ps = SGP41_Probe(rt->dev.bus);
    if (ps != DRIVER_STATUS_OK)
    {
        rt->state = DEVICE_STATE_NOT_FOUND;
        rt->last_error_class = ps;
        rt->consecutive_absent = RecoveryPolicy_TrackAbsence(rt->consecutive_absent, true);
        rt->next_probe_ms = now + RecoveryPolicy_BackoffMs(rt->consecutive_absent);
        return ps;
    }

    /* Probe succeeded: absence episode over, reset backoff. */
    rt->consecutive_absent = 0U;
    rt->last_error_class = DRIVER_STATUS_OK;

    /* First ever startup: initialize the gas-index algorithms once. */
    if (rt->voc_index == 0 && rt->last_valid_measurement_ms == 0U)
    {
        GasIndexAlgorithm_init(&rt->voc_alg, GasIndexAlgorithm_ALGORITHM_TYPE_VOC);
        GasIndexAlgorithm_init(&rt->nox_alg, GasIndexAlgorithm_ALGORITHM_TYPE_NOX);
        rt->voc_index_valid = false;
        rt->nox_index_valid = false;
        rt->conditioning_ms = 0U;
    }

    if (rt->dev.initialized == 0U)
        SGP41_Init(&rt->dev, rt->dev.bus);

    if (rt->comp_seen == false)
    {
        /* No compensation supplied yet: use SGP41 documented defaults. */
        Sgp41Compensation dflt;
        memset(&dflt, 0, sizeof(dflt));
        dflt.valid = false;
        SGP41_CompensationToTicks(&dflt, &rt->comp_rh_ticks, &rt->comp_t_ticks);
        rt->comp_seen = true;
    }

    DriverStatus ss = Sgp41Runtime_BeginStartup(rt);
    if (ss != DRIVER_STATUS_OK)
    {
        rt->last_error_class = ss;
        Sgp41Runtime_RecordFailure(rt);
        if (rt->consecutive_errors >= SGP41_RUNTIME_ERROR_THRESHOLD)
        {
            rt->phase = SGP41_PHASE_IDLE;
            Sgp41Runtime_EscalateError(rt);
        }
        return ss;
    }

    rt->state = DEVICE_STATE_STARTING;
    return DRIVER_STATUS_OK;
}

/* Complete the in-flight conditioning read, advancing the conditioning budget.
   A failed conditioning read is a real failure but not fatal (bounded). */
static void Sgp41Runtime_FinishConditioning(Sgp41Runtime *rt, uint32_t now)
{
    uint16_t raw_voc = 0U;
    DriverStatus rs = SGP41_FinishConditioning(&rt->dev, &raw_voc);
    rt->phase = SGP41_PHASE_IDLE;
    if (rs != DRIVER_STATUS_OK)
    {
        rt->last_error_class = rs;
        Sgp41Runtime_RecordFailure(rt);
        if (rt->consecutive_errors >= SGP41_RUNTIME_ERROR_THRESHOLD)
        {
            Sgp41Runtime_EscalateError(rt);
            return;
        }
        /* Bounded retry: re-issue another conditioning tick. */
        rt->state = DEVICE_STATE_STARTING;
        return;
    }

    /* One conditioning second elapsed. Conditioning raw VOC is intentionally
       NOT committed as a measurement and never exposed as valid. */
    rt->conditioning_ms += SGP41_RUNTIME_CONDITIONING_TICKS_MS;
    (void)raw_voc;

    if (rt->conditioning_ms >= SGP41_RUNTIME_CONDITIONING_MAX_MS)
    {
        /* Conditioning complete: begin measurement. */
        DriverStatus ms = SGP41_BeginMeasure(&rt->dev, rt->comp_rh_ticks, rt->comp_t_ticks);
        if (ms != DRIVER_STATUS_OK)
        {
            rt->last_error_class = ms;
            Sgp41Runtime_RecordFailure(rt);
            if (rt->consecutive_errors >= SGP41_RUNTIME_ERROR_THRESHOLD)
                Sgp41Runtime_EscalateError(rt);
            return;
        }
        rt->phase = SGP41_PHASE_MEASURING;
        rt->deadline_ms = now + SGP41_MEASURE_EXECUTION_MS;
        rt->state = DEVICE_STATE_WAITING;
        return;
    }

    /* Continue conditioning: next tick after the conditioning period. */
    rt->state = DEVICE_STATE_STARTING;
}

/* Issue the next single measure at the 1 Hz cadence. */
static void Sgp41Runtime_IssueNextMeasure(Sgp41Runtime *rt, uint32_t now)
{
    DriverStatus ms = SGP41_BeginMeasure(&rt->dev, rt->comp_rh_ticks, rt->comp_t_ticks);
    if (ms != DRIVER_STATUS_OK)
    {
        rt->last_error_class = ms;
        Sgp41Runtime_RecordFailure(rt);
        if (rt->consecutive_errors >= SGP41_RUNTIME_ERROR_THRESHOLD)
            Sgp41Runtime_EscalateError(rt);
        return;
    }
    rt->phase = SGP41_PHASE_MEASURING;
    rt->deadline_ms = now + SGP41_MEASURE_EXECUTION_MS;
}

void Sgp41Runtime_Poll(Sgp41Runtime *rt)
{
    if (rt == NULL) return;

    uint32_t now = Platform_GetTickMs();
    rt->last_tick_ms = now;

    if (rt->state == DEVICE_STATE_NOT_FOUND ||
        rt->state == DEVICE_STATE_ERROR ||
        rt->state == DEVICE_STATE_RECOVERING)
        return;

    Sgp41Runtime_EnforceFreshness(rt, now);

    if (rt->consecutive_errors >= SGP41_RUNTIME_ERROR_THRESHOLD)
    {
        rt->phase = SGP41_PHASE_IDLE;
        Sgp41Runtime_EscalateError(rt);
        return;
    }

    /* ----------------------- Conditioning phase ----------------------- */
    if (rt->state == DEVICE_STATE_STARTING && rt->phase == SGP41_PHASE_CONDITIONING)
    {
        if (Sgp41Runtime_DeadlinePassed(now, rt->deadline_ms) == false)
            return;
        Sgp41Runtime_FinishConditioning(rt, now);
        return;
    }

    /* STARTING without an in-flight transaction: begin conditioning/measure. */
    if (rt->state == DEVICE_STATE_STARTING && rt->phase == SGP41_PHASE_IDLE)
    {
        DriverStatus ss = Sgp41Runtime_BeginStartup(rt);
        if (ss != DRIVER_STATUS_OK)
        {
            rt->last_error_class = ss;
            Sgp41Runtime_RecordFailure(rt);
            if (rt->consecutive_errors >= SGP41_RUNTIME_ERROR_THRESHOLD)
                Sgp41Runtime_EscalateError(rt);
        }
        return;
    }

    /* ----------------------- Measurement phase ------------------------ */
    if (rt->state == DEVICE_STATE_WAITING && rt->phase == SGP41_PHASE_MEASURING)
    {
        if (Sgp41Runtime_DeadlinePassed(now, rt->deadline_ms) == false)
            return;

        Sgp41RawMeasurement m;
        DriverStatus rs = SGP41_FinishMeasure(&rt->dev, &m);
        rt->phase = SGP41_PHASE_IDLE;
        if (rs != DRIVER_STATUS_OK)
        {
            rt->last_error_class = rs;
            Sgp41Runtime_RecordFailure(rt);
            if (rt->consecutive_errors >= SGP41_RUNTIME_ERROR_THRESHOLD)
            {
                Sgp41Runtime_EscalateError(rt);
                return;
            }
            /* Bounded retry: re-issue the measurement so the WAITING state keeps
               making forward progress (never orphan into WAITING/IDLE). A failed
               read does NOT feed the gas index, so the 1 Hz cadence is intact. */
            Sgp41Runtime_IssueNextMeasure(rt, now);
            return;
        }

        /* A fresh raw sample: commit, feed the gas index at 1 Hz, and gate
           index validity on out-of-blackout + fresh. */
        rt->last_sample = m;
        rt->last_valid_measurement_ms = now;
        rt->state = DEVICE_STATE_READY;
        rt->last_error_class = DRIVER_STATUS_OK;
        Sgp41Runtime_RecordSuccess(rt);

        /* Feed the gas-index once per second. */
        if ((now - rt->last_index_feed_ms) >= SGP41_RUNTIME_SAMPLE_PERIOD_MS ||
            rt->last_index_feed_ms == 0U)
        {
            GasIndexAlgorithm_process(&rt->voc_alg, m.raw_voc, &rt->voc_index);
            GasIndexAlgorithm_process(&rt->nox_alg, m.raw_nox, &rt->nox_index);
            rt->last_index_feed_ms = now;

            /* VOC index valid once the algorithm is out of initial blackout
               (its uptime > 45 s) — approximated by feeding >= 46 samples.
               NOx index valid only after conditioning completed AND the NOx
               algorithm is out of blackout (its uptime > 45 s) AND its mean
               estimator has initialized (only after the blackout window so the
               firmware never reports a fabricated stable value). */
            if (rt->fed_count_since_start >= 46U && rt->last_sample.valid)
                rt->voc_index_valid = true;
            if (rt->conditioning_ms >= SGP41_RUNTIME_NOX_CONDITIONING_MS &&
                rt->fed_count_since_start >= 46U &&
                rt->nox_alg.m_Mean_Variance_Estimator___Initialized &&
                rt->last_sample.valid)
                rt->nox_index_valid = true;
            rt->fed_count_since_start++;
        }
        return;
    }

    /* READY: after the 1 Hz sample period, issue the next measure. */
    if (rt->state == DEVICE_STATE_READY)
    {
        if ((now - rt->last_valid_measurement_ms) < SGP41_RUNTIME_SAMPLE_PERIOD_MS)
            return;   /* not yet time for the next sample */

        Sgp41Runtime_IssueNextMeasure(rt, now);
        if (rt->phase == SGP41_PHASE_MEASURING)
            rt->state = DEVICE_STATE_WAITING;
        return;
    }
}

void Sgp41Runtime_Recover(Sgp41Runtime *rt)
{
    if (rt == NULL) return;
    rt->recovery_count++;
    rt->consecutive_errors = 0U;
    /* LAST-GOOD POLICY (coherent with bus recovery): entering a recovery epoch
       invalidates the last-good sample and indices; fresh data is required. */
    rt->last_sample.valid = false;
    rt->voc_index_valid = false;
    rt->nox_index_valid = false;
    rt->phase = SGP41_PHASE_IDLE;
    rt->state = DEVICE_STATE_RECOVERING;
}

void Sgp41Runtime_GetDiagnostics(const Sgp41Runtime *rt, DeviceRuntime *out)
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