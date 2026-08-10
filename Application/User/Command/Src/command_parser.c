#include "command_parser.h"
#include "command.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>

static uint8_t HexVal(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    return 0xFF;
}

static bool ParseEntityId(uint8_t out[16], const char *str, size_t len)
{
    if (len != 32) return false;
    for (size_t i = 0; i < 16; i++)
    {
        uint8_t hi = HexVal(str[i * 2]);
        uint8_t lo = HexVal(str[i * 2 + 1]);
        if (hi == 0xFF || lo == 0xFF) return false;
        out[i] = (uint8_t)(hi << 4) | lo;
    }
    return true;
}

static CommandType StrToType(const char *str)
{
    if (strcmp(str, "GET_STATUS") == 0)              return COMMAND_GET_STATUS;
    if (strcmp(str, "GET_CONFIG") == 0)              return COMMAND_GET_CONFIG;
    if (strcmp(str, "SET_CONFIG") == 0)              return COMMAND_SET_CONFIG;
    if (strcmp(str, "RESET_CONFIG") == 0)            return COMMAND_RESET_CONFIG;
    if (strcmp(str, "GET_IDENTITY") == 0)            return COMMAND_GET_IDENTITY;
    if (strcmp(str, "SELF_TEST") == 0)               return COMMAND_SELF_TEST;
    if (strcmp(str, "REBOOT") == 0)                  return COMMAND_REBOOT;
    if (strcmp(str, "GET_CAPABILITIES") == 0)        return COMMAND_GET_CAPABILITIES;
    if (strcmp(str, "GET_MANIFEST") == 0)            return COMMAND_GET_MANIFEST;
    if (strcmp(str, "REGISTER_DEVICE") == 0)         return COMMAND_REGISTER_DEVICE;
    if (strcmp(str, "UNREGISTER_DEVICE") == 0)       return COMMAND_UNREGISTER_DEVICE;
    if (strcmp(str, "FACTORY_RESET") == 0)           return COMMAND_FACTORY_RESET;
    if (strcmp(str, "GET_PROVISIONING_STATUS") == 0) return COMMAND_GET_PROVISIONING_STATUS;
    if (strcmp(str, "ASSIGN_LOCATION") == 0)         return COMMAND_ASSIGN_LOCATION;
    return COMMAND_UNKNOWN;
}

static void SkipW(const uint8_t *d, size_t sz, size_t *p)
{
    while (*p < sz && (d[*p] == ' ' || d[*p] == '\t' || d[*p] == '\n' || d[*p] == '\r'))
        (*p)++;
}

static bool CharAt(const uint8_t *d, size_t sz, size_t *p, uint8_t expected)
{
    SkipW(d, sz, p);
    if (*p >= sz || d[*p] != expected) return false;
    (*p)++;
    return true;
}

static bool ParseString(const uint8_t *d, size_t sz, size_t *p, char *out, size_t out_max)
{
    SkipW(d, sz, p);
    if (*p >= sz || d[*p] != '"') return false;
    (*p)++;

    size_t o = 0;
    while (*p < sz)
    {
        uint8_t c = d[*p];
        if (c == '"') { (*p)++; out[o] = '\0'; return true; }
        if (c == '\\')
        {
            (*p)++;
            if (*p >= sz) return false;
            uint8_t esc = d[*p];
            if (esc == '"') c = '"';
            else if (esc == '\\') c = '\\';
            else if (esc == 'n') c = '\n';
            else if (esc == 'r') c = '\r';
            else if (esc == 't') c = '\t';
            else return false;
        }
        else if (c < 0x20)
        {
            return false;
        }
        if (o >= out_max - 1) return false;
        out[o++] = (char)c;
        (*p)++;
    }
    return false;
}

static bool ParseUint(const uint8_t *d, size_t sz, size_t *p, uint32_t *val)
{
    SkipW(d, sz, p);
    if (*p >= sz || d[*p] < '0' || d[*p] > '9') return false;

    uint32_t v = 0;
    while (*p < sz && d[*p] >= '0' && d[*p] <= '9')
    {
        uint8_t digit = d[*p] - '0';
        if (v > (UINT32_MAX - digit) / 10U) return false;  /* overflow */
        v = v * 10U + digit;
        (*p)++;
    }
    *val = v;
    return true;
}

static bool ParseFloat(const uint8_t *d, size_t sz, size_t *p, float *val)
{
    SkipW(d, sz, p);
    size_t start = *p;

    while (*p < sz && (d[*p] == '-' || d[*p] == '+' || d[*p] == '.' || (d[*p] >= '0' && d[*p] <= '9') || d[*p] == 'e' || d[*p] == 'E'))
        (*p)++;

    size_t len = *p - start;
    if (len == 0 || len > 127) return false;

    char buf[128];
    memcpy(buf, d + start, len);
    buf[len] = '\0';

    /* Reject NaN/Inf */
    if (strchr(buf, 'n') || strchr(buf, 'N') || strchr(buf, 'i') || strchr(buf, 'I'))
        return false;

    char *endptr;
    errno = 0;
    float v = strtof(buf, &endptr);
    if (errno != 0 || endptr != buf + len) return false;
    if (!isfinite(v)) return false;

    *val = v;
    return true;
}


bool CommandParser_Parse(const uint8_t *data, size_t size, CommandRequest *request)
{
    if ((data == NULL) || (request == NULL)) return false;
    if (size == 0) return false;

    memset(request, 0, sizeof(*request));
    request->type = COMMAND_UNKNOWN;

    size_t pos = 0;
    SkipW(data, size, &pos);

    if (!CharAt(data, size, &pos, '{')) return false;

    bool has_id = false;
    bool has_cmd = false;
    char field[64];

    while (pos < size)
    {
        SkipW(data, size, &pos);
        if (pos >= size) return false;

        if (data[pos] == '}') { pos++; break; }

        if (!ParseString(data, size, &pos, field, sizeof(field)))
            return false;

        if (!CharAt(data, size, &pos, ':'))
            return false;

        if (strcmp(field, "id") == 0)
        {
            if (has_id) return false;
            if (!ParseUint(data, size, &pos, &request->request_id)) return false;
            has_id = true;
            request->has_request_id = true;
        }
        else if (strcmp(field, "command") == 0)
        {
            if (has_cmd) return false;
            char cmd[32];
            if (!ParseString(data, size, &pos, cmd, sizeof(cmd))) return false;
            request->type = StrToType(cmd);
            has_cmd = true;
        }
        else if (strcmp(field, "light_period_ms") == 0)
        {
            if (request->args.has_light_period_ms) return false;
            if (!ParseUint(data, size, &pos, &request->args.light_period_ms)) return false;
            request->args.has_light_period_ms = true;
        }
        else if (strcmp(field, "display_period_ms") == 0)
        {
            if (request->args.has_display_period_ms) return false;
            if (!ParseUint(data, size, &pos, &request->args.display_period_ms)) return false;
            request->args.has_display_period_ms = true;
        }
        else if (strcmp(field, "telemetry_period_ms") == 0)
        {
            if (request->args.has_telemetry_period_ms) return false;
            if (!ParseUint(data, size, &pos, &request->args.telemetry_period_ms)) return false;
            request->args.has_telemetry_period_ms = true;
        }
        else if (strcmp(field, "light_calibration") == 0)
        {
            if (request->args.has_light_calibration) return false;
            if (!ParseFloat(data, size, &pos, &request->args.light_calibration)) return false;
            request->args.has_light_calibration = true;
        }
        else if (strcmp(field, "installation_id") == 0)
        {
            if (request->args.has_installation_id) return false;
            char hex[64];
            if (!ParseString(data, size, &pos, hex, sizeof(hex))) return false;
            if (!ParseEntityId(request->args.installation_id, hex, strlen(hex))) return false;
            request->args.has_installation_id = true;
        }
        else if (strcmp(field, "building_id") == 0)
        {
            if (request->args.has_building_id) return false;
            char hex[64];
            if (!ParseString(data, size, &pos, hex, sizeof(hex))) return false;
            if (!ParseEntityId(request->args.building_id, hex, strlen(hex))) return false;
            request->args.has_building_id = true;
        }
        else if (strcmp(field, "room_id") == 0)
        {
            if (request->args.has_room_id) return false;
            char hex[64];
            if (!ParseString(data, size, &pos, hex, sizeof(hex))) return false;
            if (!ParseEntityId(request->args.room_id, hex, strlen(hex))) return false;
            request->args.has_room_id = true;
        }
        else
        {
            return false;  /* unknown field */
        }

        SkipW(data, size, &pos);
        if (pos < size && data[pos] == ',') { pos++; continue; }
    }

    /* Trailing non-whitespace is rejected */
    SkipW(data, size, &pos);
    if (pos != size) return false;

    return has_cmd && has_id;
}
