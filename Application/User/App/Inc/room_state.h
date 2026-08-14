#ifndef ROOM_STATE_H
#define ROOM_STATE_H

#include <stdbool.h>
#include <stdint.h>

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

    /* BMP390 barometric pressure (Pa) + its internal temperature (degC). The
       pressure is the primary measured value; BMP390 temperature exists mainly
       for pressure compensation and is NOT treated as the canonical room T/RH
       source (display/ROOM T/RH remain SHT45 primary, SCD41 fallback). */
    float  bmp390_pressure_pa;
    bool   bmp390_pressure_valid;

    float  bmp390_temperature_c;
    bool   bmp390_temperature_valid;

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
void         RoomState_UpdateBmp390(RoomState *state,
                                    float pressure_pa, bool pressure_valid,
                                    float temperature_c, bool temperature_valid);
void         RoomState_InvalidateBmp390(RoomState *state);
const RoomState *RoomState_Get(const RoomState *state);

#endif