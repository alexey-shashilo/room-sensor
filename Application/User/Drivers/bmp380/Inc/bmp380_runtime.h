#ifndef BMP380_RUNTIME_H
#define BMP380_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>
#include "i2c_bus.h"
#include "room_sensor_types.h"
#include "bmp380.h"

/* BMP380 runtime (portable). Owns the non-blocking FORCED-mode measurement state
   machine for the separate BMP380 driver.

   Layering: App -> BMP380 Runtime -> BMP380 Driver -> I2cBus. The runtime never
   blocks; it advances on each polled tick using elapsed time (App scheduler).

   v1 operating profile (mirrors BMP390):
   - FORCED mode (single-shot per trigger, sensor returns to sleep). Fits the
     low-rate environmental/barometric model and keeps the shared I2C bus quiet.
   - Pressure x8 oversampling, temperature x4, IIR coefficient x3.

   States (existing DeviceState):
     NOT_FOUND  -> probe/identity failed / absent (with per-device backoff).
     STARTING   -> identity verified; configuring + calibration loading / measuring.
     WAITING    -> measurement triggered; waiting data-ready.
     READY      -> at least one valid sample accepted; continues.
     ERROR      -> durable failures >= threshold.
     RECOVERING -> Bmp380Runtime_Recover() epoch.

   SAMPLE VALIDITY vs RUNTIME STATE are SEPARATE: a last-good sample stays valid
   while the next acquisition is in progress and is invalidated only by stale
   timeout, durable error, confirmed loss, or explicit invalidation. */

typedef enum
{
    BMP380_PHASE_IDLE = 0,
    BMP380_PHASE_CONFIGURING,
    BMP380_PHASE_MEASURING,
    BMP380_PHASE_BETWEEN_MEASUREMENTS
} Bmp380RuntimePhase;

typedef struct
{
    DeviceState        state;
    Bmp380RuntimePhase phase;
    uint32_t           deadline_ms;

    Bmp380      dev;
    Bmp380Sample last_sample;

    uint32_t operation_successes;
    uint32_t operation_failures;
    uint32_t consecutive_errors;
    uint32_t recovery_count;
    uint32_t last_success_ms;
    uint32_t last_failure_ms;
    uint32_t last_valid_measurement_ms;

    uint32_t consecutive_absent;
    uint32_t next_probe_ms;

    DriverStatus last_error_class;

    uint32_t last_tick_ms;
} Bmp380Runtime;

/* v1 timing (forced single-shot, room/barometric trend). Same budget as BMP390. */
#define BMP380_RUNTIME_POLL_INTERVAL_MS          500U
#define BMP380_RUNTIME_MEASUREMENT_INTERVAL_MS   5000U      /* ~0.2 Hz */
#define BMP380_RUNTIME_MEASUREMENT_DEADLINE_MS   2000U      /* data-ready budget */
#define BMP380_RUNTIME_STALE_MS                  (3U * BMP380_RUNTIME_MEASUREMENT_INTERVAL_MS)
#define BMP380_RUNTIME_ERROR_THRESHOLD           3U

void Bmp380Runtime_Init(Bmp380Runtime *rt, const I2cBus *bus);

DriverStatus Bmp380Runtime_Start(Bmp380Runtime *rt);
void         Bmp380Runtime_Poll(Bmp380Runtime *rt);
void         Bmp380Runtime_Recover(Bmp380Runtime *rt);

bool Bmp380Runtime_ProbeDue(const Bmp380Runtime *rt, uint32_t now);
DriverStatus Bmp380Runtime_LastError(const Bmp380Runtime *rt);

bool Bmp380Runtime_HasValidSample(const Bmp380Runtime *rt);
bool Bmp380Runtime_IsMissing(const Bmp380Runtime *rt);
void Bmp380Runtime_InvalidateSample(Bmp380Runtime *rt);
void Bmp380Runtime_GetDiagnostics(const Bmp380Runtime *rt, DeviceRuntime *out);

#endif