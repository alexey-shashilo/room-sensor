#include "telemetry.h"
#include <string.h>

static uint32_t s_sequence = 0;

bool Telemetry_CreateSnapshot(TelemetrySnapshot *snapshot, const TelemetrySnapshotInput *input)
{
    if ((snapshot == NULL) || (input == NULL)) return false;
    if (input->device_id == NULL) return false;
    if (input->room == NULL) return false;

    memset(snapshot, 0, sizeof(*snapshot));

    s_sequence++;
    snapshot->sequence = s_sequence;

    memcpy(snapshot->device_id, input->device_id, 16);
    snapshot->uptime_ms = input->uptime_ms;
    snapshot->captured_at_ms = input->uptime_ms;
    snapshot->room = *input->room;
    snapshot->health = input->health;

    return true;
}