#ifndef SCD41_RUNTIME_H
#define SCD41_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>
#include "i2c_bus.h"
#include "room_sensor_types.h"
#include "scd41.h"

/* SCD41 runtime (portable). Owns the non-blocking periodic measurement state
   machine and bridges the SCD41 driver to RoomState via App.

   Layering: App -> SCD41 Runtime -> SCD41 Driver -> I2cBus. The runtime never
   blocks; it advances on each polled tick using elapsed time (App scheduler),
   so the watchdog is always serviced. Timing is driven by the official SCD4x
   periodic cadence (SCD41_PERIODIC_INTERVAL_MS).

   States (mapped onto the existing portable DeviceState enum):
     NOT_FOUND   -> probe failed / sensor absent (retried by App_DoRetry).
     STARTING    -> start_periodic issued; waiting periodic interval for the
                    first sample to be produced.
     INITIALIZING-> (transient) no probe yet / re-probe in progress.
     WAITING     -> measuring; polling data-ready. No sample consumed yet.
     READY       -> at least one valid sample accepted; continues measuring.
     ERROR       -> consecutive real failures >= threshold.
     RECOVERING  -> App_DoRetry before re-probe (bounded recovery).

   data-ready=false is a VALID result: the runtime stays operational, does NOT
   increment error counters, and does NOT overwrite RoomState. Only real
   communication/CRC/protocol failures are counted as errors. */

/* Internal protocol phase (kept separate from the externally-visible
   DeviceState). Tracks which two-phase SCD4x transaction is in flight so the
   runtime can enforce the ~1 ms command-execution deadline cooperatively. */
typedef enum
{
    SCD41_PHASE_IDLE = 0,
    SCD41_PHASE_WAIT_DATA_READY_RESPONSE,
    SCD41_PHASE_WAIT_MEASUREMENT_RESPONSE
} Scd41RuntimePhase;

typedef struct
{
    /* Runtime-oriented state (DeviceState). */
    DeviceState state;

    /* Internal response-phase for the in-flight two-phase transaction. */
    Scd41RuntimePhase phase;

    /* Absolute tick deadline by which the two-phase response may be read. A read
       is only allowed when now >= deadline. */
    uint32_t deadline_ms;

    /* Driver handle + last accepted sample. */
    Scd41              dev;
    Scd41Measurement   last_sample;

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
    uint32_t started_at_ms;   /* when periodic measurement was (re)started */
    uint32_t last_tick_ms;
    bool     start_pending;   /* start command accepted, not yet WAITING */
} Scd41Runtime;

/* Default timing tunables. Kept OUT of persistent Config (task §20): they are
   compile-time constants because SCD4x periodic cadence is fixed by the
   datasheet and no genuine user-facing configuration requirement exists. Exposed
   as named constants for tests and clarity. */
#define SCD41_RUNTIME_POLL_INTERVAL_MS     500U
#define SCD41_RUNTIME_STALE_MS             (3U * SCD41_PERIODIC_INTERVAL_MS)
#define SCD41_RUNTIME_ERROR_THRESHOLD      3U

void Scd41Runtime_Init(Scd41Runtime *rt, const I2cBus *bus);

/* App-driven lifecycle step (mirrors App_DoProbeVeml / App_DoInitVeml): probe
   the SCD41; if present, init the driver and issue start_periodic_measurement.
   Used for both first boot and bounded recovery. Returns DRIVER_STATUS_OK when
   the sensor was found and periodic measurement was started. */
DriverStatus Scd41Runtime_Start(Scd41Runtime *rt);

/* Advance the state machine on the App scheduler tick (poll_interval). Must be
   called from App when the runtime is STARTING/WAITING/READY. Non-blocking. */
void Scd41Runtime_Poll(Scd41Runtime *rt);

/* Bounded recovery: escalate ERROR into RECOVERING and increment the recovery
   counter; App_DoRetry then calls Start() to re-probe + restart. */
void Scd41Runtime_Recover(Scd41Runtime *rt);

/* True when at least one valid sample has been accepted and is FRESH. */
bool Scd41Runtime_HasValidSample(const Scd41Runtime *rt);
bool Scd41Runtime_IsMissing(const Scd41Runtime *rt);

/* Fill a portable DeviceRuntime diagnostic snapshot (for GET_STATUS /
   AppStatus). */
void Scd41Runtime_GetDiagnostics(const Scd41Runtime *rt, DeviceRuntime *out);

#endif