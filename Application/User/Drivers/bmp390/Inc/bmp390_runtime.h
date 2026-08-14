#ifndef BMP390_RUNTIME_H
#define BMP390_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>
#include "i2c_bus.h"
#include "room_sensor_types.h"
#include "bmp390.h"

/* BMP390 runtime (portable). Owns the non-blocking FORCED-mode measurement state
   machine and bridges the BMP390 driver to RoomState via App.

   Layering: App -> BMP390 Runtime -> BMP390 Driver -> I2cBus. The runtime never
   blocks; it advances on each polled tick using elapsed time (App scheduler).

   v1 operating profile (PHASE 9/10 decision):
   - FORCED mode (single-shot per trigger, sensor returns to sleep). Fits the
     low-rate environmental/barometric model, keeps the shared I2C bus quiet,
     and gives a simple non-blocking state machine. NORMAL mode is not needed.
   - Pressure x8 oversampling, temperature x4, IIR coefficient x3, no explicit
     ODR use (forced mode).

   States (existing DeviceState):
     NOT_FOUND  -> probe/identity failed / absent.
     STARTING   -> identity verified; configuring + calibration loading.
     WAITING    -> measurement triggered; waiting data-ready.
     READY      -> at least one valid sample accepted; continues.
     ERROR      -> durable failures >= threshold.
     RECOVERING -> Bmp390Runtime_Recover() epoch.

   SAMPLE VALIDITY vs RUNTIME STATE are SEPARATE: a last-good sample stays
   valid while the next acquisition is in progress and is invalidated only by
   stale timeout, durable error, confirmed loss, or explicit invalidation. */

typedef enum
{
    BMP390_PHASE_IDLE = 0,
    BMP390_PHASE_CONFIGURING,   /* configure + load calibration once */
    BMP390_PHASE_MEASURING,     /* forced trigger sent, awaiting data-ready */
    BMP390_PHASE_BETWEEN_MEASUREMENTS
} Bmp390RuntimePhase;

typedef struct
{
    DeviceState       state;
    Bmp390RuntimePhase phase;
    uint32_t          deadline_ms;

    Bmp390      dev;
    Bmp390Sample last_sample;

    uint32_t operation_successes;
    uint32_t operation_failures;
    uint32_t consecutive_errors;
    uint32_t recovery_count;
    uint32_t last_success_ms;
    uint32_t last_failure_ms;
    uint32_t last_valid_measurement_ms;

    uint32_t last_tick_ms;
} Bmp390Runtime;

/* v1 timing (forced single-shot, room/barometric trend).

   OSR measurement-time check (Bosch BMP3 datasheet): measurement time grows
   with oversampling as ~ T_conv*(2^temp_os) + P_conv*(2^press_os) + overhead.
   With temp_os=2 (x4 → ~2.9 ms) and press_os=3 (x8 → ~32.2 ms) the total
   conversion is well under ~40 ms, far below the 2 s data-ready deadline and the
   5 s measurement interval. The selected profile is environmental trending
   (~0.2 Hz), NOT a high-rate flight-control configuration. */
#define BMP390_RUNTIME_POLL_INTERVAL_MS          500U
#define BMP390_RUNTIME_MEASUREMENT_INTERVAL_MS   5000U      /* ~0.2 Hz */
#define BMP390_RUNTIME_MEASUREMENT_DEADLINE_MS   2000U      /* data-ready budget */
#define BMP390_RUNTIME_STALE_MS                  (3U * BMP390_RUNTIME_MEASUREMENT_INTERVAL_MS)
#define BMP390_RUNTIME_ERROR_THRESHOLD           3U

void Bmp390Runtime_Init(Bmp390Runtime *rt, const I2cBus *bus);

DriverStatus Bmp390Runtime_Start(Bmp390Runtime *rt);
void         Bmp390Runtime_Poll(Bmp390Runtime *rt);
void         Bmp390Runtime_Recover(Bmp390Runtime *rt);

bool Bmp390Runtime_HasValidSample(const Bmp390Runtime *rt);
bool Bmp390Runtime_IsMissing(const Bmp390Runtime *rt);
void Bmp390Runtime_InvalidateSample(Bmp390Runtime *rt);
void Bmp390Runtime_GetDiagnostics(const Bmp390Runtime *rt, DeviceRuntime *out);

#endif