#ifndef SHT45_RUNTIME_H
#define SHT45_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>
#include "i2c_bus.h"
#include "room_sensor_types.h"
#include "sht45.h"

/* SHT45 runtime (portable). Owns the non-blocking single-shot measurement state
   machine and bridges the SHT45 driver to RoomState via App.

   Layering: App -> SHT45 Runtime -> SHT45 Driver -> I2cBus. The runtime never
   blocks; it advances on each polled tick using elapsed time (App scheduler),
   so the watchdog is always serviced. Timing is driven by the official SHT4x
   high-precision measurement duration.

   States (mapped onto the existing portable DeviceState enum):
     NOT_FOUND -> probe failed / sensor absent (retried by App_DoRetry).
     STARTING  -> measurement command issued; waiting conversion deadline.
     WAITING   -> deadline passed but no valid sample yet (transient).
     READY     -> at least one valid sample accepted; continues measuring.
     ERROR     -> consecutive real failures >= threshold.
     RECOVERING-> App_DoRetry before re-probe (bounded recovery).

   Unlike SCD41, SHT4x is a single-shot sensor: each READY cycle issues one
   measurement command, waits the conversion deadline, reads one sample, and
   then waits the measurement interval before the next cycle.

   SAMPLE VALIDITY vs RUNTIME STATE are SEPARATE facts:
     - Runtime state answers "what is the sensor/runtime doing?"
       (NOT_FOUND/STARTING/WAITING/READY/ERROR/RECOVERING).
     - SHT45Runtime_HasValidSample() answers "can the last accepted measurement
       still be used?" A previously accepted sample REMAINS VALID while the next
       single-shot conversion is in progress (STARTING) and is only invalidated
       by stale timeout, a durable ERROR, confirmed-missing, or explicit
       invalidation. Starting a new conversion must NOT flicker validity.
   Freshness is enforced from last_valid_measurement_ms independent of the
   transaction state, so a stale sample is invalidated even while a retry is in
   flight (without aborting the in-flight measurement).

   Early-read/NACK contract: the runtime prevents reads before the official
   conversion deadline (no I2C read is issued). If an unexpected transport NACK
   occurs after the deadline, the I2cBus maps HAL AF/BUSY/ERROR to
   DRIVER_STATUS_BUS_ERROR; that is treated as a bounded (counted) transport
   failure under the portable contract — the runtime does NOT rely on a
   DRIVER_STATUS_NOT_READY classification for normal timing. */

/* Internal protocol phase: which single-shot transaction is in flight. */
typedef enum
{
    SHT45_PHASE_IDLE = 0,
    SHT45_PHASE_MEASURING,        /* measure command sent, waiting conversion */
    SHT45_PHASE_BETWEEN_MEASUREMENTS  /* sample accepted, idle until next cycle */
} Sht45RuntimePhase;

typedef struct
{
    /* Runtime-oriented state (DeviceState). */
    DeviceState state;

    /* Internal phase for the in-flight single-shot transaction. */
    Sht45RuntimePhase phase;

    /* Absolute tick deadline (conversion completion / next measurement). */
    uint32_t deadline_ms;

    /* Driver handle + last accepted sample. */
    Sht45              dev;
    Sht45Measurement   last_sample;

    /* Diagnostics (reuse the DeviceRuntime pattern). */
    uint32_t operation_successes;
    uint32_t operation_failures;
    uint32_t consecutive_errors;
    uint32_t recovery_count;
    uint32_t last_success_ms;
    uint32_t last_failure_ms;

    /* Freshness policy. */
    uint32_t last_valid_measurement_ms;

    /* Internal timing anchors (ms). */
    uint32_t last_tick_ms;
} Sht45Runtime;

/* Default timing tunables (compile-time; not user configurable). */
#define SHT45_RUNTIME_POLL_INTERVAL_MS  500U
#define SHT45_RUNTIME_MEASUREMENT_INTERVAL_MS 2000U
#define SHT45_RUNTIME_STALE_MS          (3U * SHT45_RUNTIME_MEASUREMENT_INTERVAL_MS)
#define SHT45_RUNTIME_ERROR_THRESHOLD   3U

void Sht45Runtime_Init(Sht45Runtime *rt, const I2cBus *bus);

/* App-driven lifecycle step: probe the SHT45; if present, init the driver and
   start the measurement cycle. Returns DRIVER_STATUS_OK when found. */
DriverStatus Sht45Runtime_Start(Sht45Runtime *rt);

/* Advance the state machine on the App scheduler tick. Non-blocking. */
void Sht45Runtime_Poll(Sht45Runtime *rt);

/* True when at least one valid sample has been accepted and is FRESH. Decoupled
   from the STARTING/READY transaction state: a last-good sample remains valid
   while the next conversion is in progress, and becomes false only on stale
   timeout, durable error invalidation, confirmed-missing, or explicit reset. */
bool Sht45Runtime_HasValidSample(const Sht45Runtime *rt);
bool Sht45Runtime_IsMissing(const Sht45Runtime *rt);

/* Escalate a confirmed-missing / durable error by invalidating the sample and
   forcing the runtime into ERROR so App can drive bounded recovery. Exposed so
   App/self-test can explicitly invalidate on sensor loss. */
void Sht45Runtime_InvalidateSample(Sht45Runtime *rt);

/* Fill a portable DeviceRuntime diagnostic snapshot. */
void Sht45Runtime_GetDiagnostics(const Sht45Runtime *rt, DeviceRuntime *out);

#endif