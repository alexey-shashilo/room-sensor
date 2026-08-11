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
    snapshot->boot_id = input->boot_id;
    /* BOOT-RELATIVE monotonic timestamps. Room Sensor v1 has no wall clock /
       UTC source, so BOTH uptime_ms and captured_at_ms are elapsed-milliseconds
       since App boot (the same boot-relative value). A future synchronized UTC
       "captured_at" is a separate concern and not conflated here. */
    snapshot->uptime_ms = input->uptime_ms;
    snapshot->captured_at_ms = input->uptime_ms;
    snapshot->room = *input->room;
    snapshot->health = input->health;

    return true;
}