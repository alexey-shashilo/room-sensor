#ifndef ROOM_STATE_H
#define ROOM_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include "room_sensor_types.h"

typedef enum
{
    MEASUREMENT_INVALID = 0,
    MEASUREMENT_VALID,
    MEASUREMENT_STALE
} MeasurementState;

typedef struct
{
    float  illuminance_lux;
    bool   illuminance_valid;

    /* SCD41-backed environmental channels. Clean source semantics: SCD41's
       temperature/RH are its LOCAL/internal compensation values — NOT declared
       as the canonical room T/RH (a future dedicated SHT45 will become the
       primary environmental T/RH source). Naming keeps the source explicit so
       later sensor fusion can select primaries without domain renames. */

    /* CO2 concentration (ppm). valid=false before the first sample and when the
       SCD41 is unavailable/stale. Never represent "not measured" as 0 ppm. */
    float  co2_ppm;
    bool   co2_valid;

    /* SCD41 internal/local temperature (degC) and RH (%). Valid only when a
       fully CRC-valid SCD41 sample was accepted and is still fresh. */
    float  scd41_temperature_c;
    bool   scd41_temperature_valid;

    float  scd41_humidity_pct;
    bool   scd41_humidity_valid;

    /* SHT45-backed environmental T/RH (explicitly separate sensor source). The
       future canonical room temperature/humidity source may be SHT45 primary with
       SCD41 secondary/diagnostic; both remain separately observable until then. */
    float  sht45_temperature_c;
    bool   sht45_temperature_valid;

    float  sht45_humidity_pct;
    bool   sht45_humidity_valid;

    /* Generic barometric domain channels (Phase 17.7B). The active barometric
       PROVIDER (BAROMETER_PROVIDER_BMP390 or _BMP380) owns these values; validity
       follows that provider's runtime freshness semantics (no second stale timer
       here — the provider runtime owns freshness). provider == NONE when no
       supported barometer has a fresh valid sample; then no valid pressure is
       published (never fabricate 0 Pa). Exactly ONE provider is authoritative
       (no fusion, no double publication). */
    float  barometric_pressure_pa;
    bool   barometric_pressure_valid;

    float  barometric_temperature_c;
    bool   barometric_temperature_valid;

    BarometerProvider barometric_provider;

    /* BMP390 LEGACY COMPATIBILITY fields (DEPRECATED w.r.t. the generic
       barometric channels). Populated ONLY when the selected provider is BMP390
       (from a real BMP390 sample); NEVER populated by BMP380. BMP380 must never
       set bmp390_* valid. Kept for backward-compatible telemetry until consumers
       migrate to the generic barometric_* channels. */
    float  bmp390_pressure_pa;
    bool   bmp390_pressure_valid;

    float  bmp390_temperature_c;
    bool   bmp390_temperature_valid;

    /* SGP41 VOC/NOx gas-index channels. raw_* are the raw tick signals; the
       voc_index/nox_index are the gas-index algorithm outputs (1..500 when
       valid). No partial commitment: each *_valid is set only when the backing
       measurement is fresh and (for the index channels) the gas-index is out of
       warm-up/blackout. The raw signals can be valid even while the index
       channels are not yet valid (during warm-up). */
    float  voc_raw;
    bool   voc_raw_valid;

    float  nox_raw;
    bool   nox_raw_valid;

    float  voc_index;
    bool   voc_index_valid;

    float  nox_index;
    bool   nox_index_valid;

    /* Monotonic tick when the last measurement was committed. */
    uint32_t timestamp_ms;
} RoomState;

void         RoomState_Init(RoomState *state);
void         RoomState_UpdateIlluminance(RoomState *state, float lux, bool valid);
void         RoomState_UpdateScd41(RoomState *state,
                                   float co2_ppm, bool co2_valid,
                                   float temperature_c, bool temperature_valid,
                                   float humidity_pct, bool humidity_valid);
void         RoomState_InvalidateScd41(RoomState *state);
void         RoomState_UpdateSht45(RoomState *state,
                                   float temperature_c, bool temperature_valid,
                                   float humidity_pct, bool humidity_valid);
void         RoomState_InvalidateSht45(RoomState *state);
void RoomState_UpdateBmp390(RoomState *state,
                            float pressure_pa, bool pressure_valid,
                            float temperature_c, bool temperature_valid);
void         RoomState_InvalidateBmp390(RoomState *state);

/* Commit a generic barometric snapshot from the ACTIVE provider, atomically:
   provider + pressure + temperature + validity update as one coherent unit so a
   consumer never sees pressure from one sensor and provider/temperature from
   another. provider must be BMP390 or BMP380; the caller passes the ACTIVE
   provider's valid last sample. If BMP390 is the provider, the legacy
   bmp390_* compatibility fields are also populated (only then). For any other
   provider they are cleared. */
void RoomState_UpdateBarometric(RoomState *state,
                                BarometerProvider provider,
                                float pressure_pa, bool pressure_valid,
                                float temperature_c, bool temperature_valid);
void         RoomState_InvalidateBarometric(RoomState *state);
void         RoomState_UpdateSgp41(RoomState *state,
                                   float voc_raw, bool voc_raw_valid,
                                   float nox_raw, bool nox_raw_valid,
                                   float voc_index, bool voc_index_valid,
                                   float nox_index, bool nox_index_valid);
void         RoomState_InvalidateSgp41(RoomState *state);
const RoomState *RoomState_Get(const RoomState *state);

#endif