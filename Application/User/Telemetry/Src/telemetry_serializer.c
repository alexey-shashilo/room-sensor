#include "telemetry_serializer.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
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

static bool IsFinite(float v)
{
    return (v == v) && (v != 1.0f / 0.0f) && (v != -1.0f / 0.0f);
}

static SerializeStatus AppendFormat(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
{
    if ((buf == NULL) || (pos == NULL) || (fmt == NULL))
        return SERIALIZE_INVALID_ARG;
    if (*pos >= cap)
        return SERIALIZE_BUFFER_TOO_SMALL;

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + *pos, cap - *pos, fmt, args);
    va_end(args);

    if (n < 0)
        return SERIALIZE_ERROR;
    if ((size_t)n >= (cap - *pos))
        return SERIALIZE_BUFFER_TOO_SMALL;

    *pos += (size_t)n;
    return SERIALIZE_OK;
}

static SerializeStatus AppendId(char *buf, size_t cap, size_t *pos, const uint8_t id[16])
{
    return AppendFormat(buf, cap, pos,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        (unsigned)id[0], (unsigned)id[1], (unsigned)id[2], (unsigned)id[3],
        (unsigned)id[4], (unsigned)id[5],
        (unsigned)id[6], (unsigned)id[7],
        (unsigned)id[8], (unsigned)id[9],
        (unsigned)id[10], (unsigned)id[11], (unsigned)id[12],
        (unsigned)id[13], (unsigned)id[14], (unsigned)id[15]);
}

static SerializeStatus AppendMeasurement(char *buf, size_t cap, size_t *pos,
                                          const char *name, float value, bool valid)
{
    if (valid && IsFinite(value))
        return AppendFormat(buf, cap, pos,
            "      \"%s\": {\n"
            "        \"value\": %.1f,\n"
            "        \"state\": \"valid\"\n"
            "      }", name, (double)value);
    else
        return AppendFormat(buf, cap, pos,
            "      \"%s\": {\n"
            "        \"state\": \"invalid\"\n"
            "      }", name);
}

SerializeStatus Telemetry_Serialize(
    const TelemetrySnapshot *snapshot,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *written)
{
    if (written != NULL) *written = 0;

    if ((snapshot == NULL) || (buffer == NULL) || (written == NULL))
        return SERIALIZE_INVALID_ARG;

    char *buf = (char *)buffer;
    size_t cap = buffer_size;
    size_t pos = 0;
    SerializeStatus s;

    s = AppendFormat(buf, cap, &pos, "{\n");
    if (s != SERIALIZE_OK) return s;

    s = AppendFormat(buf, cap, &pos, "  \"schema\": %u,\n", (unsigned)TELEMETRY_SCHEMA_VERSION);
    if (s != SERIALIZE_OK) return s;

    s = AppendFormat(buf, cap, &pos, "  \"device_id\": \"");
    if (s != SERIALIZE_OK) return s;

    s = AppendId(buf, cap, &pos, snapshot->device_id);
    if (s != SERIALIZE_OK) return s;

    s = AppendFormat(buf, cap, &pos, "\",\n");
    if (s != SERIALIZE_OK) return s;

    s = AppendFormat(buf, cap, &pos,
        "  \"seq\": %lu,\n"
        "  \"uptime_ms\": %lu,\n"
        "  \"captured_at_ms\": %lu,\n"
        "  \"health\": \"%s\",\n",
        (unsigned long)snapshot->sequence,
        (unsigned long)snapshot->uptime_ms,
        (unsigned long)snapshot->captured_at_ms,
        HealthStr(snapshot->health));
    if (s != SERIALIZE_OK) return s;

    s = AppendFormat(buf, cap, &pos, "  \"room\": {\n");
    if (s != SERIALIZE_OK) return s;

    s = AppendMeasurement(buf, cap, &pos,
        "illuminance_lux",
        snapshot->room.illuminance_lux,
        snapshot->room.illuminance_valid);
    if (s != SERIALIZE_OK) return s;

    s = AppendFormat(buf, cap, &pos, "\n  }\n");
    if (s != SERIALIZE_OK) return s;

    s = AppendFormat(buf, cap, &pos, "}\n");
    if (s != SERIALIZE_OK) return s;

    *written = pos;
    return SERIALIZE_OK;
}