#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>
#include <stdbool.h>
#include "room_state.h"
#include "app.h"

#define TELEMETRY_SCHEMA_VERSION 1U

typedef struct
{
    uint8_t device_id[16];
    uint32_t sequence;
    uint32_t uptime_ms;
    uint32_t captured_at_ms;

    RoomState room;
    SystemHealthState health;

} TelemetrySnapshot;

typedef struct
{
    TelemetrySnapshot latest;
    bool pending;
} TelemetryBuffer;

typedef struct
{
    const uint8_t *device_id;
    const RoomState *room;
    SystemHealthState health;
    uint32_t uptime_ms;
} TelemetrySnapshotInput;

bool Telemetry_CreateSnapshot(TelemetrySnapshot *snapshot, const TelemetrySnapshotInput *input);

#endif