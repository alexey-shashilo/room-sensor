#include "bmp380_runtime.h"
#include "platform_time.h"
#include "recovery_policy.h"
#include <string.h>

/* Non-blocking FORCED-mode measurement state machine (BMP380). */

static bool DeadlinePassed(uint32_t now, uint32_t deadline)
{
    return (uint32_t)(now - deadline) < 0x80000000U;
}

static void RecordSuccess(Bmp380Runtime *rt)
{
    rt->operation_successes++;
    rt->consecutive_errors = 0;
    rt->last_success_ms = Platform_GetTickMs();
}

static void RecordFailure(Bmp380Runtime *rt)
{
    rt->operation_failures++;
    rt->consecutive_errors++;
    rt->last_failure_ms = Platform_GetTickMs();
}

static void EscalateError(Bmp380Runtime *rt)
{
    rt->last_sample.valid = false;
    rt->state = DEVICE_STATE_ERROR;
}

void Bmp380Runtime_Init(Bmp380Runtime *rt, const I2cBus *bus)
{
    if (rt == NULL) return;
    memset(rt, 0, sizeof(*rt));
    rt->state = DEVICE_STATE_NOT_FOUND;
    rt->last_tick_ms = Platform_GetTickMs();
    if (bus != NULL)
        BMP380_Init(&rt->dev, bus);
}

bool Bmp380Runtime_HasValidSample(const Bmp380Runtime *rt)
{
    return rt != NULL && rt->last_sample.valid;
}

bool Bmp380Runtime_IsMissing(const Bmp380Runtime *rt)
{
    return rt != NULL && rt->state == DEVICE_STATE_NOT_FOUND;
}

void Bmp380Runtime_InvalidateSample(Bmp380Runtime *rt)
{
    if (rt == NULL) return;
    rt->last_sample.valid = false;
}

static void EnforceFreshness(Bmp380Runtime *rt, uint32_t now)
{
    if (rt->last_sample.valid == false) return;
    if ((now - rt->last_valid_measurement_ms) >= BMP380_RUNTIME_STALE_MS)
        rt->last_sample.valid = false;
}

bool Bmp380Runtime_ProbeDue(const Bmp380Runtime *rt, uint32_t now)
{
    if (rt == NULL) return false;
    if (rt->consecutive_absent == 0U)
        return true;
    if (RecoveryPolicy_Elapsed(now, rt->next_probe_ms, 0U) == false)
        return false;
    return (uint32_t)(now - rt->next_probe_ms) < 0x80000000U;
}

static bool ReTriggerMeasurement(Bmp380Runtime *rt, uint32_t now)
{
    DriverStatus ts = BMP380_TriggerMeasurement(&rt->dev);
    if (ts != DRIVER_STATUS_OK)
    {
        rt->last_error_class = ts;
        RecordFailure(rt);
        if (rt->consecutive_errors >= BMP380_RUNTIME_ERROR_THRESHOLD)
        {
            rt->phase = BMP380_PHASE_IDLE;
            EscalateError(rt);
        }
        else
        {
            rt->phase = BMP380_PHASE_MEASURING;
            rt->state  = DEVICE_STATE_STARTING;
        }
        return false;
    }
    rt->deadline_ms = now + BMP380_RUNTIME_MEASUREMENT_DEADLINE_MS;
    rt->phase = BMP380_PHASE_MEASURING;
    rt->state  = DEVICE_STATE_STARTING;
    return true;
}

DriverStatus Bmp380Runtime_LastError(const Bmp380Runtime *rt)
{
    return rt != NULL ? rt->last_error_class : DRIVER_STATUS_INVALID_ARG;
}

DriverStatus Bmp380Runtime_Start(Bmp380Runtime *rt)
{
    if (rt == NULL) return DRIVER_STATUS_INVALID_ARG;

    uint32_t now = Platform_GetTickMs();

    if (rt->consecutive_absent > 0U)
    {
        if (RecoveryPolicy_Elapsed(now, rt->next_probe_ms, 0U) == false ||
            (uint32_t)(now - rt->next_probe_ms) >= 0x80000000U)
        {
            rt->state = DEVICE_STATE_NOT_FOUND;
            return DRIVER_STATUS_NOT_FOUND;
        }
    }

    DriverStatus s = BMP380_Detect(&rt->dev);
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

    if (rt->dev.calib.calibrated == 0U)
    {
        s = BMP380_InitCalibration(&rt->dev);
        if (s != DRIVER_STATUS_OK)
        {
            rt->last_error_class = s;
            return s;
        }
    }
    s = BMP380_ConfigureRoomProfile(&rt->dev);
    if (s != DRIVER_STATUS_OK)
    {
        rt->last_error_class = s;
        RecordFailure(rt);
        if (rt->consecutive_errors >= BMP380_RUNTIME_ERROR_THRESHOLD)
            rt->state = DEVICE_STATE_ERROR;
        return s;
    }

    s = BMP380_TriggerMeasurement(&rt->dev);
    if (s != DRIVER_STATUS_OK)
    {
        rt->last_error_class = s;
        RecordFailure(rt);
        if (rt->consecutive_errors >= BMP380_RUNTIME_ERROR_THRESHOLD)
            rt->state = DEVICE_STATE_ERROR;
        return s;
    }

    rt->deadline_ms = Platform_GetTickMs() + BMP380_RUNTIME_MEASUREMENT_DEADLINE_MS;
    rt->phase = BMP380_PHASE_MEASURING;
    rt->state = DEVICE_STATE_STARTING;
    return DRIVER_STATUS_OK;
}

void Bmp380Runtime_Recover(Bmp380Runtime *rt)
{
    if (rt == NULL) return;
    rt->recovery_count++;
    rt->consecutive_errors = 0;
    rt->last_sample.valid = false;
    rt->phase = BMP380_PHASE_IDLE;
    rt->state = DEVICE_STATE_RECOVERING;
}

void Bmp380Runtime_Poll(Bmp380Runtime *rt)
{
    if (rt == NULL) return;

    uint32_t now = Platform_GetTickMs();
    rt->last_tick_ms = now;

    if (rt->state == DEVICE_STATE_NOT_FOUND ||
        rt->state == DEVICE_STATE_ERROR ||
        rt->state == DEVICE_STATE_RECOVERING)
        return;

    EnforceFreshness(rt, now);

    if (rt->consecutive_errors >= BMP380_RUNTIME_ERROR_THRESHOLD)
    {
        rt->phase = BMP380_PHASE_IDLE;
        EscalateError(rt);
        return;
    }

    if (rt->state == DEVICE_STATE_STARTING && rt->phase == BMP380_PHASE_MEASURING)
    {
        if (DeadlinePassed(now, rt->deadline_ms) == false)
            return;

        uint8_t status = 0;
        DriverStatus rs = BMP380_ReadStatus(&rt->dev, &status);
        if (rs != DRIVER_STATUS_OK)
        {
            rt->last_error_class = rs;
            RecordFailure(rt);
            if (rt->consecutive_errors >= BMP380_RUNTIME_ERROR_THRESHOLD)
                EscalateError(rt);
            rt->phase = BMP380_PHASE_MEASURING;
            rt->deadline_ms = now + BMP380_RUNTIME_MEASUREMENT_DEADLINE_MS;
            return;
        }

        if ((status & (BMP380_STATUS_DRDY_PRESS | BMP380_STATUS_DRDY_TEMP)) !=
            (BMP380_STATUS_DRDY_PRESS | BMP380_STATUS_DRDY_TEMP))
        {
            rt->deadline_ms = now + BMP380_RUNTIME_MEASUREMENT_DEADLINE_MS;
            return;
        }

        {
            uint8_t err = 0;
            DriverStatus er = BMP380_ReadError(&rt->dev, &err);
            if (er == DRIVER_STATUS_DEVICE_ERROR || er != DRIVER_STATUS_OK)
            {
                rt->last_error_class = er;
                RecordFailure(rt);
                if (rt->consecutive_errors >= BMP380_RUNTIME_ERROR_THRESHOLD)
                    EscalateError(rt);
                rt->phase = BMP380_PHASE_MEASURING;
                rt->deadline_ms = now + BMP380_RUNTIME_MEASUREMENT_DEADLINE_MS;
                BMP380_ConfigureRoomProfile(&rt->dev);
                return;
            }
        }

        Bmp380Sample sample;
        DriverStatus rr = BMP380_ReadSample(&rt->dev, &sample);
        if (rr != DRIVER_STATUS_OK)
        {
            rt->last_error_class = rr;
            RecordFailure(rt);
            if (rt->consecutive_errors >= BMP380_RUNTIME_ERROR_THRESHOLD)
            {
                rt->phase = BMP380_PHASE_IDLE;
                EscalateError(rt);
                return;
            }
            ReTriggerMeasurement(rt, now);
            return;
        }

        if (sample.valid == false)
        {
            rt->last_error_class = DRIVER_STATUS_CRC_ERROR;
            RecordFailure(rt);
            if (rt->consecutive_errors >= BMP380_RUNTIME_ERROR_THRESHOLD)
            {
                rt->phase = BMP380_PHASE_IDLE;
                EscalateError(rt);
                return;
            }
            ReTriggerMeasurement(rt, now);
            return;
        }

        rt->last_sample = sample;
        rt->last_valid_measurement_ms = now;
        rt->state = DEVICE_STATE_READY;
        rt->phase = BMP380_PHASE_BETWEEN_MEASUREMENTS;
        rt->last_error_class = DRIVER_STATUS_OK;
        RecordSuccess(rt);
        return;
    }

    if (rt->state == DEVICE_STATE_READY && rt->phase == BMP380_PHASE_BETWEEN_MEASUREMENTS)
    {
        if ((now - rt->last_valid_measurement_ms) < BMP380_RUNTIME_MEASUREMENT_INTERVAL_MS)
            return;

        DriverStatus ms = BMP380_TriggerMeasurement(&rt->dev);
        if (ms != DRIVER_STATUS_OK)
        {
            rt->last_error_class = ms;
            RecordFailure(rt);
            if (rt->consecutive_errors >= BMP380_RUNTIME_ERROR_THRESHOLD)
                EscalateError(rt);
            return;
        }
        rt->deadline_ms = now + BMP380_RUNTIME_MEASUREMENT_DEADLINE_MS;
        rt->phase = BMP380_PHASE_MEASURING;
        rt->state = DEVICE_STATE_STARTING;
        return;
    }
}

void Bmp380Runtime_GetDiagnostics(const Bmp380Runtime *rt, DeviceRuntime *out)
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