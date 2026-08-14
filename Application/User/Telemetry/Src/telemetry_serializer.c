#include "telemetry_serializer.h"
#include "hex64.h"
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

/* Write exactly one field separator (",") before a field when prepend is
   true, and none otherwise. Keeps JSON structurally valid: exactly one comma
   between fields and no trailing comma. */
static SerializeStatus AppendFieldSeparator(char *buf, size_t cap, size_t *pos,
                                             bool prepend)
{
    if (!prepend)
        return SERIALIZE_OK;
    return AppendFormat(buf, cap, pos, ",");
}

static SerializeStatus AppendMeasurement(char *buf, size_t cap, size_t *pos,
                                          const char *name, float value, bool valid,
                                          bool prepend)
{
    /* A measurement is only valid when the caller-validity flag is set AND the
       value is finite. NaN/Inf are never serialized numerically. */
    bool emit_valid = valid && IsFinite(value);
    SerializeStatus s = AppendFieldSeparator(buf, cap, pos, prepend);
    if (s != SERIALIZE_OK) return s;

    if (emit_valid)
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

/* SCD41 min/max CO2 range for plausibility (SCD4x datasheet: 0..40000 ppm
   nominal covered; values outside indicate protocol/sanity issues). */
#define SCD41_CO2_MAX_PPM 40000U

/* Serialize the SCD41 CO2 channel as an integer ppm with a defensive finite and
 * range gate. Ordering requirements (task §5):
 *   valid->finite->>=0-><= max -> then float->integer conversion.
 * NaN/Inf/negative/>max are serialized as invalid (never "0 ppm"). */
static SerializeStatus AppendCo2(char *buf, size_t cap, size_t *pos,
                                  const RoomState *room, bool prepend)
{
    if (room == NULL)
        return SERIALIZE_INVALID_ARG;

    SerializeStatus s = AppendFieldSeparator(buf, cap, pos, prepend);
    if (s != SERIALIZE_OK) return s;

    float c = room->co2_ppm;
    bool ok = room->co2_valid && IsFinite(c) &&
              (c >= 0.0f) && (c <= (float)SCD41_CO2_MAX_PPM);

    if (ok)
        return AppendFormat(buf, cap, pos,
            "    \"co2_ppm\": {\n"
            "      \"value\": %lu,\n"
            "      \"state\": \"valid\"\n"
            "    }",
            (unsigned long)c);   /* safe integer conversion after finite+range gate */
    else
        return AppendFormat(buf, cap, pos,
            "    \"co2_ppm\": {\n"
            "      \"state\": \"invalid\"\n"
            "    }");
}

/* Serialize the SCD41 channels within the room block with explicit validity
 * semantics. CO2 is rendered as an integer ppm when valid; when invalid, no
 * numeric value is emitted (never "0 ppm"). SCD41 T/RH are source-explicit
 * (local internal sensor values, NOT canonical room T/RH) and omitted when
 * invalid. */
static SerializeStatus AppendScd41Block(char *buf, size_t cap, size_t *pos,
                                        const RoomState *room)
{
    if (room == NULL)
        return SERIALIZE_INVALID_ARG;

    SerializeStatus s = AppendCo2(buf, cap, pos, room, true);
    if (s != SERIALIZE_OK) return s;

    /* SCD41 local/internal T and RH (secondary source; validity explicit). */
    s = AppendMeasurement(buf, cap, pos,
        "scd41_temperature_c",
        room->scd41_temperature_c,
        room->scd41_temperature_valid,
        true);
    if (s != SERIALIZE_OK) return s;

    s = AppendMeasurement(buf, cap, pos,
        "scd41_humidity_pct",
        room->scd41_humidity_pct,
        room->scd41_humidity_valid,
        true);
    if (s != SERIALIZE_OK) return s;

    return SERIALIZE_OK;
}

/* Serialize the SHT45 channels within the room block with explicit validity
   semantics. SHT45 is the dedicated environmental T/RH source; when invalid no
   numeric value is emitted (never a fake 0). */
static SerializeStatus AppendSht45Block(char *buf, size_t cap, size_t *pos,
                                        const RoomState *room)
{
    if (room == NULL)
        return SERIALIZE_INVALID_ARG;

    SerializeStatus s = AppendMeasurement(buf, cap, pos,
        "sht45_temperature_c",
        room->sht45_temperature_c,
        room->sht45_temperature_valid,
        true);
    if (s != SERIALIZE_OK) return s;

    s = AppendMeasurement(buf, cap, pos,
        "sht45_humidity_pct",
        room->sht45_humidity_pct,
        room->sht45_humidity_valid,
        true);
    if (s != SERIALIZE_OK) return s;

    return SERIALIZE_OK;
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

    {
        /* boot_id is serialized without any libc 64-bit printf ("%llx") because
           newlib-nano lacks %ll support; see hex64.h. Always [0-9a-f]{16}. */
        char bid[17];
        Hex64_ToLower(bid, snapshot->boot_id);
        s = AppendFormat(buf, cap, &pos, "  \"boot_id\": \"%s\",\n", bid);
        if (s != SERIALIZE_OK) return s;
    }

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
        snapshot->room.illuminance_valid,
        false);   /* first field in the room object: no leading comma */
    if (s != SERIALIZE_OK) return s;

    /* SCD41-backed channels. CO2 uses integer ppm; invalid -> no numeric value
       (never renders invalid CO2 as 0). */
    s = AppendScd41Block(buf, cap, &pos, &snapshot->room);
    if (s != SERIALIZE_OK) return s;

    /* SHT45-backed dedicated T/RH channels (separate source from SCD41). */
    s = AppendSht45Block(buf, cap, &pos, &snapshot->room);
    if (s != SERIALIZE_OK) return s;

    s = AppendFormat(buf, cap, &pos, "\n  }\n");
    if (s != SERIALIZE_OK) return s;

    s = AppendFormat(buf, cap, &pos, "}\n");
    if (s != SERIALIZE_OK) return s;

    *written = pos;
    return SERIALIZE_OK;
}