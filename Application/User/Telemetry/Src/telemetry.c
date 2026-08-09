#include "telemetry.h"
#include "device_identity.h"
#include "platform_time.h"
#include <string.h>

static uint32_t s_sequence = 0;

bool Telemetry_CreateSnapshot(TelemetrySnapshot *snapshot)
{
    if (snapshot == NULL) return false;

    memset(snapshot, 0, sizeof(*snapshot));

    s_sequence++;
    snapshot->sequence = s_sequence;

    DeviceIdentity id;
    if (DeviceIdentity_Load(&id))
        memcpy(snapshot->device_id, id.device_uuid, 16);

    snapshot->uptime_ms = Platform_GetTickMs();
    snapshot->captured_at_ms = snapshot->uptime_ms;

    extern RoomState s_room;
    snapshot->room = s_room;

    extern SystemHealthState s_health;
    snapshot->health = s_health;

    return true;
}