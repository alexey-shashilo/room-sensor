#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>
#include <stdbool.h>
#include "room_state.h"
#include "room_sensor_types.h"
/* Telemetry depends only on lower-level portable types (RoomState,
   SystemHealthState from room_sensor_types.h). It MUST NOT include app.h. */

/* Schema v5: added BMP390 barometric pressure and its internal temperature to
   the room object. v4 -> v5 is a wire-format change (new fields), so the version
   is incremented and GET_CAPABILITIES reports it automatically.
   History:
     v2: baseline room metrics (illuminance).
     v3: added SCD41 CO2, temperature and RH channels (explicit validity).
     v4: added SHT45 temperature and RH channels (dedicated environmental T/RH).
     v5: added BMP390 barometric pressure (Pa) and sensor-internal temperature. */
#define TELEMETRY_SCHEMA_VERSION 5U

typedef struct
{
    uint8_t device_id[16];
    uint64_t boot_id;
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
    uint64_t boot_id;
    const RoomState *room;
    SystemHealthState health;
    uint32_t uptime_ms;
} TelemetrySnapshotInput;

bool Telemetry_CreateSnapshot(TelemetrySnapshot *snapshot, const TelemetrySnapshotInput *input);

#endif