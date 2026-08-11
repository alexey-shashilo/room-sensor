#include "command_parser.h"
#include "command.h"
#include "provisioning.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>

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

/* ---- Strict JSON string (no control chars, limited escapes) ---- */
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
            else return false;  /* reject \u and other unhandled escapes */
            if (c == '\n' || c == '\r') return false;
        }
        else if (c < 0x20)
        {
            return false;  /* unescaped control character */
        }
        if (o >= out_max - 1) return false;
        out[o++] = (char)c;
        (*p)++;
    }
    return false;
}

/* ---- Strict JSON integer lexical form (uint, no sign permitted) ----
   int = "0" / ( digit1-9 *DIGIT )
   No leading '+', no fraction, no exponent. */
static bool ParseUint(const uint8_t *d, size_t sz, size_t *p, uint32_t *val)
{
    SkipW(d, sz, p);
    size_t s = *p;
    if (*p >= sz) return false;

    if (d[*p] == '-' || d[*p] == '+') return false;  /* reject sign */

    if (d[*p] == '.' || d[*p] == 'e' || d[*p] == 'E') return false;

    if (d[*p] < '0' || d[*p] > '9') return false;

    /* disallow leading zero followed by more digits (strict JSON grammar) */
    if (d[*p] == '0')
    {
        (*p)++;
        if (*p < sz && d[*p] >= '0' && d[*p] <= '9') return false;
        *val = 0;
    }
    else
    {
        uint32_t v = 0;
        while (*p < sz && d[*p] >= '0' && d[*p] <= '9')
        {
            uint8_t digit = d[*p] - '0';
            if (v > (UINT32_MAX - digit) / 10U) return false;  /* overflow */
            v = v * 10U + digit;
            (*p)++;
        }
        *val = v;
    }

    /* value already consumed digits; ensure no fraction/exponent follows */
    if (*p < sz && (d[*p] == '.' || d[*p] == 'e' || d[*p] == 'E'))
        return false;

    if (*p == s) return false;
    return true;
}

/* ---- Strict JSON number grammar before conversion ----
   number = [ minus ] int [ frac ] [ exp ]
   int  = "0" / ( digit1-9 *DIGIT )
   frac = "." 1*DIGIT
   exp  = ( "e" / "E" ) [ plus / minus ] 1*DIGIT

   Rejects: +1, .5, 1., 01, 1e, 1e+, --1.  Accepts: 0, -1, 1, 1.0, 0.5,
   1e3, -2.5e-4.
*/
static bool ScanNumberRegion(const uint8_t *d, size_t sz, size_t *p, size_t *start)
{
    *start = *p;
    size_t i = *p;

    if (i < sz && d[i] == '-') i++;

    /* integer part (required) */
    if (i >= sz) return false;
    if (d[i] >= '1' && d[i] <= '9')
    {
        i++;
        while (i < sz && d[i] >= '0' && d[i] <= '9') i++;
    }
    else if (d[i] == '0')
    {
        i++;
    }
    else
    {
        return false;  /* '.', '+', letter, or '--' */
    }

    /* fraction */
    if (i < sz && d[i] == '.')
    {
        i++;
        if (i >= sz || d[i] < '0' || d[i] > '9') return false;  /* '1.' */
        while (i < sz && d[i] >= '0' && d[i] <= '9') i++;
    }

    /* exponent */
    if (i < sz && (d[i] == 'e' || d[i] == 'E'))
    {
        i++;
        if (i < sz && (d[i] == '+' || d[i] == '-')) i++;
        if (i >= sz || d[i] < '0' || d[i] > '9') return false;  /* '1e', '1e+' */
        while (i < sz && d[i] >= '0' && d[i] <= '9') i++;
    }

    *p = i;
    return true;
}

static bool ParseFloat(const uint8_t *d, size_t sz, size_t *p, float *val)
{
    SkipW(d, sz, p);
    size_t start;
    if (!ScanNumberRegion(d, sz, p, &start)) return false;

    size_t len = *p - start;
    if (len == 0) return false;

    char buf[64];
    if (len >= sizeof(buf)) return false;
    memcpy(buf, d + start, len);
    buf[len] = '\0';

    char *endptr;
    errno = 0;
    float v = strtof(buf, &endptr);
    if (errno != 0 || endptr != buf + len) return false;
    if (!isfinite(v)) return false;

    *val = v;
    return true;
}

/* ---- Command-specific argument contracts ----
   Unexpected known fields for a command are rejected (INVALID_ARGUMENT),
   never silently ignored. */
static bool ValidateArgs(const CommandRequest *r)
{
    /* Which arguments are presently populated? */
    bool has_any =
        r->args.has_light_period_ms || r->args.has_display_period_ms ||
        r->args.has_telemetry_period_ms || r->args.has_light_calibration ||
        r->args.has_installation_id || r->args.has_building_id || r->args.has_room_id;

    switch (r->type)
    {
        case COMMAND_GET_STATUS:
        case COMMAND_GET_CONFIG:
        case COMMAND_GET_IDENTITY:
        case COMMAND_GET_CAPABILITIES:
        case COMMAND_GET_MANIFEST:
        case COMMAND_GET_PROVISIONING_STATUS:
        case COMMAND_SELF_TEST:
        case COMMAND_RESET_CONFIG:
        case COMMAND_FACTORY_RESET:
        case COMMAND_UNREGISTER_DEVICE:
        case COMMAND_REBOOT:
            /* read-only / diagnostic / destructive: no arbitrary arguments */
            return !has_any;

        case COMMAND_SET_CONFIG:
            /* only config fields; reject entity IDs */
            if (r->args.has_installation_id || r->args.has_building_id || r->args.has_room_id)
                return false;
            /* empty mutation is rejected (documented policy) */
            return
                r->args.has_light_period_ms || r->args.has_display_period_ms ||
                r->args.has_telemetry_period_ms || r->args.has_light_calibration;

        case COMMAND_REGISTER_DEVICE:
            /* installation_id only */
            return r->args.has_installation_id &&
                   !r->args.has_building_id && !r->args.has_room_id &&
                   !r->args.has_light_period_ms && !r->args.has_display_period_ms &&
                   !r->args.has_telemetry_period_ms && !r->args.has_light_calibration;

        case COMMAND_ASSIGN_LOCATION:
            /* installation_id + building_id + room_id exactly */
            return r->args.has_installation_id &&
                   r->args.has_building_id &&
                   r->args.has_room_id &&
                   !r->args.has_light_period_ms && !r->args.has_display_period_ms &&
                   !r->args.has_telemetry_period_ms && !r->args.has_light_calibration;

        case COMMAND_UNKNOWN:
            /* Unknown command names are not an argument-contract violation.
               Allow the message to reach authorization, which fails closed
               (COMMAND_SECURITY_INVALID -> UNAUTHORIZED). */
            return true;
    }
}

bool CommandParser_Parse(const uint8_t *data, size_t size, CommandRequest *request)
{
    if ((data == NULL) || (request == NULL)) return false;
    if (size == 0) return false;

    memset(request, 0, sizeof(*request));
    request->type = COMMAND_UNKNOWN;

    size_t pos = 0;
    SkipW(data, size, &pos);

    if (!(pos < size && data[pos] == '{')) return false;
    pos++;

    bool has_id = false;
    bool has_cmd = false;
    char field[64];
    bool expect_field = false;  /* after a value, a comma must follow before another field */

    while (true)
    {
        SkipW(data, size, &pos);

        /* '}' closes the object only if we are NOT expecting a field after a
           comma, i.e. trailing comma like {"a":1,} must be rejected. */
        if (pos >= size) return false;
        if (data[pos] == '}')
        {
            if (expect_field) return false;  /* trailing comma before '}' */
            pos++;
            break;
        }

        /* A field name (string) is required here. This rejects a leading
           comma (object starts with ',' plus also a lone ','), a double
           comma, and a segment not starting with a string. */
        if (!ParseString(data, size, &pos, field, sizeof(field)))
            return false;

        /* expect ':' after field name — missing colon rejected */
        SkipW(data, size, &pos);
        if (!(pos < size && data[pos] == ':')) return false;
        pos++;
        SkipW(data, size, &pos);

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
            EntityId id;
            if (!EntityId_Parse(&id, hex, strlen(hex))) return false;
            memcpy(request->args.installation_id, id.bytes, ENTITY_ID_SIZE);
            request->args.has_installation_id = true;
        }
        else if (strcmp(field, "building_id") == 0)
        {
            if (request->args.has_building_id) return false;
            char hex[64];
            if (!ParseString(data, size, &pos, hex, sizeof(hex))) return false;
            EntityId id;
            if (!EntityId_Parse(&id, hex, strlen(hex))) return false;
            memcpy(request->args.building_id, id.bytes, ENTITY_ID_SIZE);
            request->args.has_building_id = true;
        }
        else if (strcmp(field, "room_id") == 0)
        {
            if (request->args.has_room_id) return false;
            char hex[64];
            if (!ParseString(data, size, &pos, hex, sizeof(hex))) return false;
            EntityId id;
            if (!EntityId_Parse(&id, hex, strlen(hex))) return false;
            memcpy(request->args.room_id, id.bytes, ENTITY_ID_SIZE);
            request->args.has_room_id = true;
        }
        else
        {
            return false;  /* unknown field */
        }

        /* After a parsed value: skip ws, expect either ',' (more fields) or
           '}' (end). Anything else is missing comma / trailing garbage. */
        SkipW(data, size, &pos);
        if (pos < size && data[pos] == ',')
        {
            pos++;
            expect_field = true;   /* a field MUST follow the comma */
            continue;
        }
        if (pos < size && data[pos] == '}')
        {
            expect_field = false;
            continue;  /* top handles '}' termination */
        }
        return false;  /* missing comma or trailing garbage */
    }

    /* Trailing non-whitespace is rejected. */
    SkipW(data, size, &pos);
    if (pos != size) return false;

    if (!has_cmd || !has_id) return false;

    return ValidateArgs(request);
}