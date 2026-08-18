#include "room_state.h"
#include "platform_time.h"
#include <stddef.h>
#include <string.h>

void RoomState_Init(RoomState *state)
{
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    /* CO2/SCD41 channels start invalid (not-yet-measured), never 0 ppm. */
}

void RoomState_UpdateIlluminance(RoomState *state, float lux, bool valid)
{
    if (state == NULL) return;
    state->illuminance_lux = lux;
    state->illuminance_valid = valid;
    state->timestamp_ms = Platform_GetTickMs();
}

/* Commit a fully-valid SCD41 sample into the room state. Validity is explicit
   per channel. No partial commitment: the caller (runtime) only passes fully
   CRC-valid data. invalid/not-ready samples are handled via
   RoomState_InvalidateScd41, never by passing valid=false here (which keeps
   last-good numeric values available for diagnostics while clearing validity). */
void RoomState_UpdateScd41(RoomState *state,
                           float co2_ppm, bool co2_valid,
                           float temperature_c, bool temperature_valid,
                           float humidity_pct, bool humidity_valid)
{
    if (state == NULL) return;
    state->co2_ppm = co2_ppm;
    state->co2_valid = co2_valid;
    state->scd41_temperature_c = temperature_c;
    state->scd41_temperature_valid = temperature_valid;
    state->scd41_humidity_pct = humidity_pct;
    state->scd41_humidity_valid = humidity_valid;
    state->timestamp_ms = Platform_GetTickMs();
}

/* Invalidate all SCD41 channels (sensor missing / stale / startup). The numeric
   last values are retained for diagnostics; validity is cleared so nothing
   serializes/renders "not measured" as 0 ppm. */
void RoomState_InvalidateScd41(RoomState *state)
{
    if (state == NULL) return;
    state->co2_valid = false;
    state->scd41_temperature_valid = false;
    state->scd41_humidity_valid = false;
    state->timestamp_ms = Platform_GetTickMs();
}

/* Commit a fully-valid SHT45 sample into the room state. Explicit per-channel
   validity; no partial commitment (the runtime only passes fully CRC-valid
   data). Invalid/not-ready is handled via RoomState_InvalidateSht45. */
void RoomState_UpdateSht45(RoomState *state,
                           float temperature_c, bool temperature_valid,
                           float humidity_pct, bool humidity_valid)
{
    if (state == NULL) return;
    state->sht45_temperature_c = temperature_c;
    state->sht45_temperature_valid = temperature_valid;
    state->sht45_humidity_pct = humidity_pct;
    state->sht45_humidity_valid = humidity_valid;
    state->timestamp_ms = Platform_GetTickMs();
}

/* Invalidate both SHT45 channels (sensor missing / stale / startup). Numeric
   last values retained for diagnostics; validity cleared. */
void RoomState_InvalidateSht45(RoomState *state)
{
    if (state == NULL) return;
    state->sht45_temperature_valid = false;
    state->sht45_humidity_valid = false;
    state->timestamp_ms = Platform_GetTickMs();
}

/* Commit a fully-valid BMP390 sample (pressure Pa + sensor-internal temp). */
void RoomState_UpdateBmp390(RoomState *state, float pressure_pa, bool pressure_valid,
                            float temperature_c, bool temperature_valid)
{
    if (state == NULL) return;
    state->bmp390_pressure_pa = pressure_pa;
    state->bmp390_pressure_valid = pressure_valid;
    state->bmp390_temperature_c = temperature_c;
    state->bmp390_temperature_valid = temperature_valid;
    state->timestamp_ms = Platform_GetTickMs();
}

void RoomState_InvalidateBmp390(RoomState *state)
{
    if (state == NULL) return;
    state->bmp390_pressure_valid = false;
    state->bmp390_temperature_valid = false;
    state->timestamp_ms = Platform_GetTickMs();
}

/* Commit a generic barometric snapshot atomically. The provider + both generic
   channels + validities are written as one unit (no partial/mixed snapshot).
   Legacy bmp390_* compatibility fields are populated ONLY when the active
   provider is BMP390 (so BMP380 data never appears under bmp390 names). */
void RoomState_UpdateBarometric(RoomState *state,
                                BarometerProvider provider,
                                float pressure_pa, bool pressure_valid,
                                float temperature_c, bool temperature_valid)
{
    if (state == NULL) return;

    state->barometric_provider = provider;
    state->barometric_pressure_pa = pressure_pa;
    state->barometric_pressure_valid = pressure_valid &&
                                       (provider != BAROMETER_PROVIDER_NONE);
    state->barometric_temperature_c = temperature_c;
    state->barometric_temperature_valid = temperature_valid &&
                                          (provider != BAROMETER_PROVIDER_NONE);

    /* Legacy BMP390 compatibility fields: valid ONLY for a real BMP390 provider.
       BMP380 data is never placed under bmp390 names. */
    if (provider == BAROMETER_PROVIDER_BMP390)
    {
        state->bmp390_pressure_pa = pressure_pa;
        state->bmp390_pressure_valid = pressure_valid && state->barometric_pressure_valid;
        state->bmp390_temperature_c = temperature_c;
        state->bmp390_temperature_valid = temperature_valid && state->barometric_temperature_valid;
    }
    else
    {
        state->bmp390_pressure_valid = false;
        state->bmp390_temperature_valid = false;
    }

    state->timestamp_ms = Platform_GetTickMs();
}

void RoomState_InvalidateBarometric(RoomState *state)
{
    if (state == NULL) return;
    state->barometric_provider = BAROMETER_PROVIDER_NONE;
    state->barometric_pressure_valid = false;
    state->barometric_temperature_valid = false;
    state->bmp390_pressure_valid = false;
    state->bmp390_temperature_valid = false;
    state->timestamp_ms = Platform_GetTickMs();
}

/* Commit a fully-valid SGP41 sample into the room state. Explicit per-channel
   validity; no partial commitment. Invalid/not-ready is handled via
   RoomState_InvalidateSgp41, never by passing valid=false here (which would
   wipe last-good numeric values the dispatcher may need for diagnostics while
   keeping validity explicit). */
void RoomState_UpdateSgp41(RoomState *state,
                           float voc_raw, bool voc_raw_valid,
                           float nox_raw, bool nox_raw_valid,
                           float voc_index, bool voc_index_valid,
                           float nox_index, bool nox_index_valid)
{
    if (state == NULL) return;
    state->voc_raw = voc_raw;
    state->voc_raw_valid = voc_raw_valid;
    state->nox_raw = nox_raw;
    state->nox_raw_valid = nox_raw_valid;
    state->voc_index = voc_index;
    state->voc_index_valid = voc_index_valid;
    state->nox_index = nox_index;
    state->nox_index_valid = nox_index_valid;
    state->timestamp_ms = Platform_GetTickMs();
}

void RoomState_InvalidateSgp41(RoomState *state)
{
    if (state == NULL) return;
    state->voc_raw_valid = false;
    state->nox_raw_valid = false;
    state->voc_index_valid = false;
    state->nox_index_valid = false;
    state->timestamp_ms = Platform_GetTickMs();
}

const RoomState *RoomState_Get(const RoomState *state)
{
    return state;
}