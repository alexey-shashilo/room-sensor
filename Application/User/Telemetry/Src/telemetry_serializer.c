#include "telemetry_serializer.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static const char *HealthStr(SystemHealthState h)
{
    switch (h)
    {
        case SYSTEM_HEALTH_BOOTING:  return "booting";
        case SYSTEM_HEALTH_OK:       return "ok";
        case SYSTEM_HEALTH_DEGRADED: return "degraded";
        case SYSTEM_HEALTH_FAULT:    return "fault";
        default:                     return "unknown";
    }
}

static void AppendId(char *buf, size_t *pos, size_t max, const uint8_t id[16])
{
    int n = snprintf(buf + *pos, max - *pos,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        (unsigned)id[0], (unsigned)id[1], (unsigned)id[2], (unsigned)id[3],
        (unsigned)id[4], (unsigned)id[5],
        (unsigned)id[6], (unsigned)id[7],
        (unsigned)id[8], (unsigned)id[9],
        (unsigned)id[10], (unsigned)id[11], (unsigned)id[12],
        (unsigned)id[13], (unsigned)id[14], (unsigned)id[15]);
    if (n > 0) *pos += (size_t)n;
}

static bool IsFinite(float v)
{
    return (v == v) && (v != 1.0f / 0.0f) && (v != -1.0f / 0.0f);
}

static void AppendMeasurement(char *buf, size_t *pos, size_t max,
                              const char *name, float value, bool valid)
{
    int n;

    if (valid && IsFinite(value))
    {
        n = snprintf(buf + *pos, max - *pos,
            "      \"%s\": {\n"
            "        \"value\": %.1f,\n"
            "        \"state\": \"valid\"\n"
            "      }", name, (double)value);
    }
    else
    {
        n = snprintf(buf + *pos, max - *pos,
            "      \"%s\": {\n"
            "        \"state\": \"invalid\"\n"
            "      }", name);
    }

    if (n > 0) *pos += (size_t)n;
}

SerializeStatus Telemetry_Serialize(
    const TelemetrySnapshot *snapshot,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *written)
{
    if ((snapshot == NULL) || (buffer == NULL) || (written == NULL))
        return SERIALIZE_INVALID_ARG;

    char *buf = (char *)buffer;
    size_t max = buffer_size;
    size_t pos = 0;
    int n;

    if (max < 64) return SERIALIZE_BUFFER_TOO_SMALL;

    n = snprintf(buf + pos, max - pos, "{\n");
    if (n > 0) pos += (size_t)n;

    n = snprintf(buf + pos, max - pos, "  \"schema\": %u,\n", (unsigned)TELEMETRY_SCHEMA_VERSION);
    if (n > 0) pos += (size_t)n;

    n = snprintf(buf + pos, max - pos, "  \"device_id\": \"");
    if (n > 0) pos += (size_t)n;
    if (pos >= max) return SERIALIZE_BUFFER_TOO_SMALL;
    AppendId(buf, &pos, max, snapshot->device_id);

    n = snprintf(buf + pos, max - pos, "\",\n");
    if (n > 0) pos += (size_t)n;

    n = snprintf(buf + pos, max - pos,
        "  \"seq\": %lu,\n"
        "  \"uptime_ms\": %lu,\n"
        "  \"captured_at_ms\": %lu,\n"
        "  \"health\": \"%s\",\n",
        (unsigned long)snapshot->sequence,
        (unsigned long)snapshot->uptime_ms,
        (unsigned long)snapshot->captured_at_ms,
        HealthStr(snapshot->health));
    if (n > 0) pos += (size_t)n;
    if (pos >= max) return SERIALIZE_BUFFER_TOO_SMALL;

    n = snprintf(buf + pos, max - pos, "  \"room\": {\n");
    if (n > 0) pos += (size_t)n;
    if (pos >= max) return SERIALIZE_BUFFER_TOO_SMALL;

    AppendMeasurement(buf, &pos, max,
        "illuminance_lux",
        snapshot->room.illuminance_lux,
        snapshot->room.illuminance_valid);

    n = snprintf(buf + pos, max - pos, "\n  },\n");
    if (n > 0) pos += (size_t)n;

    n = snprintf(buf + pos, max - pos, "  \"session\": %lu\n",
        (unsigned long)(snapshot->uptime_ms / 60000U));
    if (n > 0) pos += (size_t)n;

    n = snprintf(buf + pos, max - pos, "}\n");
    if (n > 0) pos += (size_t)n;

    if (pos >= max) return SERIALIZE_BUFFER_TOO_SMALL;

    *written = pos;
    return SERIALIZE_OK;
}