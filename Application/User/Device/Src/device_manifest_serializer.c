#include "device_manifest_serializer.h"
#include "platform_hardware.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define ROOM_SENSOR_HARDWARE_PLATFORM  "stm32g4"
#define ROOM_SENSOR_HARDWARE_BOARD     "nucleo-g474re"
#define ROOM_SENSOR_HARDWARE_REVISION  1U

static ManifestSerializeStatus AppendFormat(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
{
    if ((buf == NULL) || (pos == NULL) || (fmt == NULL))
        return MANIFEST_SERIALIZE_INVALID_ARG;
    if (*pos >= cap)
        return MANIFEST_SERIALIZE_BUFFER_TOO_SMALL;

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + *pos, cap - *pos, fmt, args);
    va_end(args);

    if (n < 0) return MANIFEST_SERIALIZE_INVALID_ARG;
    if ((size_t)n >= (cap - *pos)) return MANIFEST_SERIALIZE_BUFFER_TOO_SMALL;
    *pos += (size_t)n;
    return MANIFEST_SERIALIZE_OK;
}

static ManifestSerializeStatus AppendStr(char *buf, size_t cap, size_t *pos, const char *key, const char *val)
{
    return AppendFormat(buf, cap, pos, "    \"%s\": \"%s\",\n", key, val ? val : "");
}

static ManifestSerializeStatus AppendBool(char *buf, size_t cap, size_t *pos, const char *key, bool val)
{
    return AppendFormat(buf, cap, pos, "    \"%s\": %s,\n", key, val ? "true" : "false");
}

static ManifestSerializeStatus AppendBoolLast(char *buf, size_t cap, size_t *pos, const char *key, bool val)
{
    return AppendFormat(buf, cap, pos, "    \"%s\": %s\n", key, val ? "true" : "false");
}

static ManifestSerializeStatus AppendUint(char *buf, size_t cap, size_t *pos, const char *key, uint32_t val)
{
    return AppendFormat(buf, cap, pos, "    \"%s\": %lu,\n", key, (unsigned long)val);
}

ManifestSerializeStatus DeviceManifest_Serialize(
    const DeviceManifest *manifest,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *written)
{
    if (written) *written = 0;
    if ((manifest == NULL) || (buffer == NULL) || (written == NULL))
        return MANIFEST_SERIALIZE_INVALID_ARG;

    char *buf = (char *)buffer;
    size_t cap = buffer_size;
    size_t pos = 0;
    ManifestSerializeStatus s;

    s = AppendFormat(buf, cap, &pos, "{\n");
    if (s != MANIFEST_SERIALIZE_OK) return s;

    s = AppendFormat(buf, cap, &pos, "  \"schema\": %u,\n", (unsigned)DEVICE_MANIFEST_SCHEMA_VERSION);
    if (s != MANIFEST_SERIALIZE_OK) return s;

    s = AppendFormat(buf, cap, &pos, "  \"device_type\": \"%s\",\n", ROOM_SENSOR_DEVICE_TYPE);
    if (s != MANIFEST_SERIALIZE_OK) return s;

    s = AppendFormat(buf, cap, &pos, "  \"model\": \"%s\",\n", ROOM_SENSOR_MODEL);
    if (s != MANIFEST_SERIALIZE_OK) return s;

    /* device_id UUID */
    s = AppendFormat(buf, cap, &pos, "  \"device_id\": \"");
    if (s != MANIFEST_SERIALIZE_OK) return s;

    const uint8_t *u = manifest->identity.device_uuid;
    s = AppendFormat(buf, cap, &pos,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        (unsigned)u[0],(unsigned)u[1],(unsigned)u[2],(unsigned)u[3],
        (unsigned)u[4],(unsigned)u[5],(unsigned)u[6],(unsigned)u[7],
        (unsigned)u[8],(unsigned)u[9],(unsigned)u[10],(unsigned)u[11],
        (unsigned)u[12],(unsigned)u[13],(unsigned)u[14],(unsigned)u[15]);
    if (s != MANIFEST_SERIALIZE_OK) return s;
    s = AppendFormat(buf, cap, &pos, "\",\n");
    if (s != MANIFEST_SERIALIZE_OK) return s;

    /* boot_id */
    s = AppendFormat(buf, cap, &pos, "  \"boot_id\": \"%016llx\",\n",
        (unsigned long long)manifest->session.boot_id);
    if (s != MANIFEST_SERIALIZE_OK) return s;

    /* firmware */
    s = AppendFormat(buf, cap, &pos, "  \"firmware\": {\n");
    if (s != MANIFEST_SERIALIZE_OK) return s;
    s = AppendStr(buf, cap, &pos, "version", manifest->firmware.version_string);
    if (s != MANIFEST_SERIALIZE_OK) return s;
    s = AppendStr(buf, cap, &pos, "git", manifest->firmware.git_commit);
    if (s != MANIFEST_SERIALIZE_OK) return s;
    s = AppendStr(buf, cap, &pos, "build", manifest->firmware.build_type);
    if (s == MANIFEST_SERIALIZE_OK && pos > 2 && buf[pos-2] == ',') { pos -= 2; buf[pos] = '\n'; pos++; }
    s = AppendFormat(buf, cap, &pos, "  },\n");
    if (s != MANIFEST_SERIALIZE_OK) return s;

    /* hardware */
    PlatformHardwareInfo hw;
    Platform_GetHardwareInfo(&hw);
    s = AppendFormat(buf, cap, &pos, "  \"hardware\": {\n");
    if (s != MANIFEST_SERIALIZE_OK) return s;
    s = AppendUint(buf, cap, &pos, "revision", ROOM_SENSOR_HARDWARE_REVISION);
    if (s != MANIFEST_SERIALIZE_OK) return s;
    s = AppendStr(buf, cap, &pos, "platform", hw.platform_family ? hw.platform_family : "unknown");
    if (s != MANIFEST_SERIALIZE_OK) return s;
    s = AppendStr(buf, cap, &pos, "board", hw.board_name ? hw.board_name : "unknown");
    if (s == MANIFEST_SERIALIZE_OK && pos > 2 && buf[pos-2] == ',') { pos -= 2; buf[pos] = '\n'; pos++; }
    s = AppendFormat(buf, cap, &pos, "  },\n");
    if (s != MANIFEST_SERIALIZE_OK) return s;

    /* protocols */
    s = AppendFormat(buf, cap, &pos, "  \"protocols\": {\n");
    if (s != MANIFEST_SERIALIZE_OK) return s;
    s = AppendUint(buf, cap, &pos, "telemetry", manifest->protocols.telemetry_schema);
    if (s != MANIFEST_SERIALIZE_OK) return s;
    s = AppendUint(buf, cap, &pos, "commands", manifest->protocols.command_schema);
    if (s != MANIFEST_SERIALIZE_OK) return s;
    s = AppendUint(buf, cap, &pos, "config", manifest->protocols.config_schema);
    if (s == MANIFEST_SERIALIZE_OK && pos > 2 && buf[pos-2] == ',') { pos -= 2; buf[pos] = '\n'; pos++; }
    s = AppendFormat(buf, cap, &pos, "  },\n");
    if (s != MANIFEST_SERIALIZE_OK) return s;

    /* capabilities */
    s = AppendFormat(buf, cap, &pos, "  \"capabilities\": {\n");
    if (s != MANIFEST_SERIALIZE_OK) return s;
    const DeviceCapabilities *c = &manifest->capabilities;
    s = AppendBool(buf, cap, &pos, "illuminance", c->illuminance);
    if (s != MANIFEST_SERIALIZE_OK) return s;
    s = AppendBool(buf, cap, &pos, "temperature", c->temperature);
    if (s != MANIFEST_SERIALIZE_OK) return s;
    s = AppendBool(buf, cap, &pos, "humidity", c->relative_humidity);
    if (s != MANIFEST_SERIALIZE_OK) return s;
    s = AppendBool(buf, cap, &pos, "co2", c->co2);
    if (s != MANIFEST_SERIALIZE_OK) return s;
    s = AppendBool(buf, cap, &pos, "pressure", c->pressure);
    if (s != MANIFEST_SERIALIZE_OK) return s;
    s = AppendBool(buf, cap, &pos, "voc", c->voc);
    if (s != MANIFEST_SERIALIZE_OK) return s;
    s = AppendBool(buf, cap, &pos, "presence", c->presence);
    if (s != MANIFEST_SERIALIZE_OK) return s;
    s = AppendBoolLast(buf, cap, &pos, "display", c->display);
    if (s != MANIFEST_SERIALIZE_OK) return s;

    s = AppendFormat(buf, cap, &pos, "  }\n");
    if (s != MANIFEST_SERIALIZE_OK) return s;

    s = AppendFormat(buf, cap, &pos, "}\n");
    if (s != MANIFEST_SERIALIZE_OK) return s;

    *written = pos;
    return MANIFEST_SERIALIZE_OK;
}