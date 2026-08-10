#include "command_response.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

static bool IsFinite(float v)
{
    return (v == v) && (v != 1.0f / 0.0f) && (v != -1.0f / 0.0f);
}

void CommandResponse_Init(CommandResponse *response, uint32_t request_id, CommandStatus status)
{
    if (response == NULL) return;
    response->request_id = request_id;
    response->status = status;
    response->overflowed = false;
    response->payload_size = 0;
}

static bool AppendChecked(CommandResponse *rsp, const char *fmt, va_list args)
{
    size_t remaining = COMMAND_RESPONSE_MAX_SIZE - rsp->payload_size;
    int n = vsnprintf((char *)rsp->payload + rsp->payload_size, remaining, fmt, args);
    if (n < 0) { rsp->overflowed = true; return false; }
    if ((size_t)n >= remaining) { rsp->overflowed = true; return false; }
    rsp->payload_size += (size_t)n;
    return true;
}

bool CommandResponse_Append(CommandResponse *response, const char *fmt, ...)
{
    if ((response == NULL) || (fmt == NULL)) return false;
    if (response->overflowed) return false;
    if (response->payload_size >= COMMAND_RESPONSE_MAX_SIZE) { response->overflowed = true; return false; }

    va_list args;
    va_start(args, fmt);
    bool ok = AppendChecked(response, fmt, args);
    va_end(args);
    return ok;
}

static void EscapeAndAppend(CommandResponse *rsp, const char *value)
{
    if (value == NULL) return;
    for (const char *p = value; *p; p++)
    {
        char buf[8];
        size_t n = 0;
        switch (*p)
        {
            case '"':  memcpy(buf, "\\\"", 2); n = 2; break;
            case '\\': memcpy(buf, "\\\\", 2); n = 2; break;
            case '\n': memcpy(buf, "\\n", 2); n = 2; break;
            case '\r': memcpy(buf, "\\r", 2); n = 2; break;
            case '\t': memcpy(buf, "\\t", 2); n = 2; break;
            default:
                if ((uint8_t)*p < 0x20) { snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)(uint8_t)*p); n = strlen(buf); }
                else { buf[0] = *p; n = 1; }
                break;
        }
        if (rsp->payload_size + n > COMMAND_RESPONSE_MAX_SIZE - 1)
        {
            rsp->overflowed = true;
            return;
        }
        memcpy(rsp->payload + rsp->payload_size, buf, n);
        rsp->payload_size += n;
    }
}

bool CommandResponse_AppendJson(CommandResponse *response, const char *key, const char *value)
{
    if (!CommandResponse_Append(response, "\"%s\":\"", key)) return false;
    EscapeAndAppend(response, value ? value : "");
    if (response->overflowed) return false;
    if (response->payload_size >= COMMAND_RESPONSE_MAX_SIZE) { response->overflowed = true; return false; }
    response->payload[response->payload_size] = ','; response->payload_size++;
    return true;
}

bool CommandResponse_AppendJsonInt(CommandResponse *response, const char *key, uint32_t value)
{
    return CommandResponse_Append(response, "\"%s\":%lu,", key, (unsigned long)value);
}

bool CommandResponse_AppendJsonFloat(CommandResponse *response, const char *key, float value, int decimals)
{
    if (!IsFinite(value))
        return CommandResponse_Append(response, "\"%s\":null,", key);
    char fmt[32];
    snprintf(fmt, sizeof(fmt), "\"%s\":%%.%df,", key, decimals);
    return CommandResponse_Append(response, fmt, (double)value);
}

bool CommandResponse_AppendJsonBool(CommandResponse *response, const char *key, bool value)
{
    return CommandResponse_Append(response, "\"%s\":%s,", key, value ? "true" : "false");
}

void CommandResponse_Finalize(CommandResponse *response)
{
    if (response == NULL) return;

    if (response->overflowed)
    {
        snprintf((char *)response->payload, COMMAND_RESPONSE_MAX_SIZE,
            "{\"id\":%lu,\"status\":\"internal_error\",\"schema\":%u,\"error\":\"response_too_large\"}\n",
            (unsigned long)response->request_id, (unsigned)COMMAND_SCHEMA_VERSION);
        response->payload_size = strlen((char *)response->payload);
        response->overflowed = false;
        return;
    }

    const char *status_str = "unknown";
    switch (response->status)
    {
        case COMMAND_STATUS_OK:                status_str = "ok"; break;
        case COMMAND_STATUS_INVALID_COMMAND:   status_str = "invalid_command"; break;
        case COMMAND_STATUS_INVALID_ARGUMENT:  status_str = "invalid_argument"; break;
        case COMMAND_STATUS_BUSY:              status_str = "busy"; break;
        case COMMAND_STATUS_NOT_SUPPORTED:     status_str = "not_supported"; break;
        case COMMAND_STATUS_CONFLICT:          status_str = "conflict"; break;
        case COMMAND_STATUS_UNAUTHORIZED:      status_str = "unauthorized"; break;
        case COMMAND_STATUS_INTERNAL_ERROR:    status_str = "internal_error"; break;
    }

    /* Calculate wrapper length first */
    char wrapper[128];
    int wn = snprintf(wrapper, sizeof(wrapper),
        "{\"id\":%lu,\"status\":\"%s\",\"schema\":%u,",
        (unsigned long)response->request_id, status_str, (unsigned)COMMAND_SCHEMA_VERSION);
    if (wn <= 0) { response->overflowed = true; return; }
    size_t wrapper_len = (size_t)wn;

    size_t close_len = 3;  /* "}\n" */

    if (response->payload_size > 0 && ((char *)response->payload)[response->payload_size - 1] == ',')
        response->payload_size--;

    size_t total = wrapper_len + response->payload_size + close_len;

    if (total >= COMMAND_RESPONSE_MAX_SIZE)
    {
        snprintf((char *)response->payload, COMMAND_RESPONSE_MAX_SIZE,
            "{\"id\":%lu,\"status\":\"internal_error\",\"schema\":%u,\"error\":\"response_too_large\"}\n",
            (unsigned long)response->request_id, (unsigned)COMMAND_SCHEMA_VERSION);
        response->payload_size = strlen((char *)response->payload);
        return;
    }

    /* Shift payload right to make room for wrapper */
    memmove(response->payload + wrapper_len, response->payload, response->payload_size);
    memcpy(response->payload, wrapper, wrapper_len);
    response->payload_size += wrapper_len;

    snprintf((char *)response->payload + response->payload_size,
             COMMAND_RESPONSE_MAX_SIZE - response->payload_size, "}\n");
    response->payload_size += close_len;
}