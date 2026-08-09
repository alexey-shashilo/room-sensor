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

    uint32_t timestamp_ms;
} RoomState;

void         RoomState_Init(RoomState *state);
void         RoomState_UpdateIlluminance(RoomState *state, float lux, bool valid);
const RoomState *RoomState_Get(const RoomState *state);

#endif