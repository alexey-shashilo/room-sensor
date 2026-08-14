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

const RoomState *RoomState_Get(const RoomState *state)
{
    return state;
}