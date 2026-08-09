#include "room_state.h"
#include "platform_time.h"
#include <stddef.h>

void RoomState_Init(RoomState *state)
{
    if (state == NULL) return;
    state->illuminance_lux = 0.0f;
    state->illuminance_valid = false;
    state->timestamp_ms = 0;
}

void RoomState_UpdateIlluminance(RoomState *state, float lux, bool valid)
{
    if (state == NULL) return;
    state->illuminance_lux = lux;
    state->illuminance_valid = valid;
    state->timestamp_ms = Platform_GetTickMs();
}

const RoomState *RoomState_Get(const RoomState *state)
{
    return state;
}