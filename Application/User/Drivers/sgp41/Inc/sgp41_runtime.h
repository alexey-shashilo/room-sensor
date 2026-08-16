#ifndef SGP41_RUNTIME_H
#define SGP41_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>
#include "i2c_bus.h"
#include "room_sensor_types.h"
#include "sgp41.h"
#include "gas_index.h"

/* SGP41 runtime (portable). Owns the non-blocking VOC/NOx measurement state
   machine and bridges the SGP41 driver to RoomState via App.

   Layering: App -> SGP41 Runtime -> SGP41 Driver -> I2cBus; and the gas-index
   (raw VOC/NOx -> VOC/NOx Index) is computed via gas_index.h. The runtime never
   blocks; it advances on each polled tick using elapsed time (App scheduler),
   so the watchdog is always serviced. Timing follows the SGP41 official 1 Hz
   measurement cadence and the <=10 s conditioning warm-up.

   The ambient humidity/temperature compensation is passed IN through an
   explicit application interface (SGP41_Compensation), NOT pulled from
   RoomState. This avoids a circular dependency (driver/runtime do not depend on
   RoomState). The App composes it from a fresh SHT45 reading when available,
   else falls back to the SGP41 documented defaults (50%RH @ 25C).

   GAS INDEX vs RAW — SEPARATE FACTS (the SGP41 architectural distinction):
     - The SGP41 returns raw VOC and NOx ticks + their valid/freshness flags.
     - The VOC/NOx Index is produced by the gas-index algorithm from raw ticks
       at a constant 1 Hz cadence, and is only trustworthy AFTER the relevant
       warm-up/blackout (VOC ~45 s initial blackout; NOx requires conditioning
       and a long initial learning). Warm-up/blackout data is NEVER exposed as a
       valid index by the runtime.

   States (mapped onto the existing DeviceState enum):
     NOT_FOUND     -> probe failed / sensor absent (retried with backoff).
     STARTING      -> probing/starting; conditioning in progress (NOx warm-up).
     WAITING       -> established conditioning; no first stable sample yet.
     READY         -> at least one valid raw sample + (eventually) valid index.
     ERROR         -> consecutive real failures >= threshold.
     RECOVERING    -> App_DoRetry before re-probe (bounded recovery).

   SAMPLE VALIDITY vs RUNTIME STATE are SEPARATE: raw validity and index
   validity are tracked independently. last_sample.raw_valid is cleared only by
   stale timeout/ERROR/confirmed-missing; index valid requires the index to be
   out of blackout AND a fresh raw sample. */

/* Internal protocol phase: which transaction is in flight. */
typedef enum
{
    SGP41_PHASE_IDLE = 0,
    SGP41_PHASE_CONDITIONING,   /* conditioning command in flight (NOx warm-up) */
    SGP41_PHASE_MEASURING       /* measure_raw_signals in flight */
} Sgp41RuntimePhase;

/* Compensation input, sourced by the App (from SHT45 or the SGP41 defaults).
   Carried explicitly so the runtime never depends on RoomState. */
typedef struct
{
    /* Compensation ticks already in SGP41 format (see SGP41_*_TICKS). Using the
       floating T/RH inputs is optional; App may instead pass ticks directly. */
    float temperature_c;   /* degC; used to derive T ticks if in_range */
    float relative_humidity_pct; /* %; used to derive RH ticks if in_range */
    bool  valid;          /* if false, SGP41 defaults (50%RH / 25C) are used */
} Sgp41Compensation;

typedef struct
{
    DeviceState state;
    Sgp41RuntimePhase phase;

    /* Absolute tick deadline for the in-flight transaction response. A read is
       only allowed when now >= deadline. */
    uint32_t deadline_ms;

    Sgp41 dev;
    Sgp41RawMeasurement last_sample;

    /* Gas-index algorithm instances (one per channel). */
    GasIndexAlgorithmParams voc_alg;
    GasIndexAlgorithmParams nox_alg;

    /* Diagnostics (DeviceRuntime pattern). */
    uint32_t operation_successes;
    uint32_t operation_failures;
    uint32_t consecutive_errors;
    uint32_t recovery_count;
    uint32_t last_success_ms;
    uint32_t last_failure_ms;

    /* Freshness policy. */
    uint32_t last_valid_measurement_ms;

    /* Conditioning (NOx) warm-up accumulator; capped at
       SGP41_CONDITIONING_MAX_MS. */
    uint32_t conditioning_ms;

    /* Compensation last applied (ticks). */
    uint16_t comp_rh_ticks;
    uint16_t comp_t_ticks;

    /* Per-channel validated index (1..500) + validity. */
    int32_t voc_index;
    int32_t nox_index;
    bool voc_index_valid;
    bool nox_index_valid;

    /* Track whether the gas-index algorithm has been fed at the 1 Hz cadence
       this second (prevents back-to-back calls / double-feeding). */
    bool fed_this_second;
    uint32_t last_index_feed_ms;

    /* Count of gas-index feeds so far this run. Used to gate index validity:
       the VOC blackout is ~45 s of uptime, so no index is valid until at least
       46 one-second feeds have occurred; NOx additionally requires conditioning
       (NOX_CONDITIONING_MS) and a initialized mean-variance estimator. */
    uint32_t fed_count_since_start;

    /* NOT_FOUND re-probe backoff. */
    uint32_t consecutive_absent;
    uint32_t next_probe_ms;

    /* Last real error classified via RecoveryPolicy (for the shared-bus
       monitor). DRIVER_STATUS_OK means no pending transport evidence. */
    DriverStatus last_error_class;

    uint32_t last_tick_ms;

    /* The last compensation instance the App supplied (composed into ticks). */
    bool comp_seen;
} Sgp41Runtime;

/* Default timing tunables (compile-time constants; the SGP41 datasheet fixes
   the 1 Hz cadence and the <=10 s conditioning cap; nothing here is user
   config).
   Poll interval matches the App scheduler granularity (500 ms). The runtime
   only issues one measure per second (SGP41_RUNTIME_SAMPLE_PERIOD_MS). */
#define SGP41_RUNTIME_POLL_INTERVAL_MS        500U
#define SGP41_RUNTIME_SAMPLE_PERIOD_MS        1000U
#define SGP41_RUNTIME_CONDITIONING_TICKS_MS   1000U
#define SGP41_RUNTIME_CONDITIONING_MAX_MS     SGP41_CONDITIONING_MAX_MS
#define SGP41_RUNTIME_STALE_MS                (3U * SGP41_RUNTIME_SAMPLE_PERIOD_MS)
#define SGP41_RUNTIME_ERROR_THRESHOLD         3U
/* How long into conditioning the NOx index may be considered (VOC index is
   blacked out at 45 s of uptime regardless). We hold the NOx index invalid
   until conditioning is done + the algorithm leaves NOx blackout. */
#define SGP41_RUNTIME_NOX_CONDITIONING_MS     SGP41_CONDITIONING_MAX_MS

void Sgp41Runtime_Init(Sgp41Runtime *rt, const I2cBus *bus);

/* App-driven lifecycle step: probe + start conditioning/measurement. Returns
   DRIVER_STATUS_OK when present and the startup sequence was initiated. */
DriverStatus Sgp41Runtime_Start(Sgp41Runtime *rt);

/* Advance the state machine on the App scheduler tick. Non-blocking. */
void Sgp41Runtime_Poll(Sgp41Runtime *rt);

/* Supply the current ambient compensation (from SHT45 or defaults). The App
   calls this whenever the compensation source changes and/or before ReadPoll.
   The runtime converts it to SGP41 ticks and clamps to the datasheet range. */
void Sgp41Runtime_SetCompensation(Sgp41Runtime *rt, const Sgp41Compensation *comp);

/* Begin a bounded recovery epoch: ERROR -> RECOVERING. App_DoRetry then calls
   Start() to re-probe. Invalidates last-good per the shared recovery contract. */
void Sgp41Runtime_Recover(Sgp41Runtime *rt);

/* Phase 4 NOT_FOUND backoff. `now` in ticks. True when a re-probe is permitted. */
bool Sgp41Runtime_ProbeDue(const Sgp41Runtime *rt, uint32_t now);

/* The DriverStatus of the most recent REAL error for the shared-bus monitor. */
DriverStatus Sgp41Runtime_LastError(const Sgp41Runtime *rt);

/* Validity queries (see header comment on raw vs index validity). */
bool Sgp41Runtime_HasValidSample(const Sgp41Runtime *rt);
bool Sgp41Runtime_HasValidVocIndex(const Sgp41Runtime *rt);
bool Sgp41Runtime_HasValidNoxIndex(const Sgp41Runtime *rt);
bool Sgp41Runtime_IsMissing(const Sgp41Runtime *rt);
void Sgp41Runtime_InvalidateSample(Sgp41Runtime *rt);

/* Fill a portable DeviceRuntime diagnostic snapshot (GET_STATUS / AppStatus). */
void Sgp41Runtime_GetDiagnostics(const Sgp41Runtime *rt, DeviceRuntime *out);

/* Conversion helpers (exposed for direct unit testing and App composition):
   convert a Celsius temp / percent RH into the SGP41 tick format, clamped per
   the SGP41 datasheet (RH 0..100% -> 0..65535; T -45..130C -> 0..65535). The
   datasheet default ticks (0x8000=50%RH, 0x6666=25C) are returned for
   out-of-range or when the input is marked invalid. */
void SGP41_CompensationToTicks(const Sgp41Compensation *comp,
                               uint16_t *rh_ticks, uint16_t *t_ticks);

#endif