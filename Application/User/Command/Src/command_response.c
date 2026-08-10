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
    response->payload_size = 0;
}

bool CommandResponse_Append(CommandResponse *response, const char *fmt, ...)
{
    if ((response == NULL) || (fmt == NULL)) return false;
    if (response->payload_size >= COMMAND_RESPONSE_MAX_SIZE) return false;

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf((char *)response->payload + response->payload_size,
                      COMMAND_RESPONSE_MAX_SIZE - response->payload_size,
                      fmt, args);
    va_end(args);

    if (n < 0) return false;
    response->payload_size += (size_t)n;
    if (response->payload_size > COMMAND_RESPONSE_MAX_SIZE)
        response->payload_size = COMMAND_RESPONSE_MAX_SIZE;
    return true;
}

bool CommandResponse_AppendJson(CommandResponse *response, const char *key, const char *value)
{
    return CommandResponse_Append(response, "\"%s\":\"%s\",", key, value ? value : "");
}

bool CommandResponse_AppendJsonInt(CommandResponse *response, const char *key, uint32_t value)
{
    return CommandResponse_Append(response, "\"%s\":%lu,", key, (unsigned long)value);
}

bool CommandResponse_AppendJsonFloat(CommandResponse *response, const char *key, float value, int decimals)
{
    if (!IsFinite(value))
        return CommandResponse_Append(response, "\"%s\":null,", key);

    char fmt_val[32];
    char fmt_key[128];
    snprintf(fmt_val, sizeof(fmt_val), "%%.%df", decimals);
    snprintf(fmt_key, sizeof(fmt_key), "\"%s\":%s,", key, fmt_val);
    return CommandResponse_Append(response, "\"%s\":", key) &&
           CommandResponse_Append(response, fmt_val, (double)value);
}

bool CommandResponse_AppendJsonBool(CommandResponse *response, const char *key, bool value)
{
    return CommandResponse_Append(response, "\"%s\":%s,", key, value ? "true" : "false");
}

void CommandResponse_Finalize(CommandResponse *response)
{
    if (response == NULL) return;

    char tmp[COMMAND_RESPONSE_MAX_SIZE];
    size_t tmp_size = 0;

    const char *status_str = "unknown";
    switch (response->status)
    {
        case COMMAND_STATUS_OK:                status_str = "ok"; break;
        case COMMAND_STATUS_INVALID_COMMAND:   status_str = "invalid_command"; break;
        case COMMAND_STATUS_INVALID_ARGUMENT:  status_str = "invalid_argument"; break;
        case COMMAND_STATUS_BUSY:              status_str = "busy"; break;
        case COMMAND_STATUS_NOT_SUPPORTED:     status_str = "not_supported"; break;
        case COMMAND_STATUS_CONFLICT:          status_str = "conflict"; break;
        case COMMAND_STATUS_INTERNAL_ERROR:    status_str = "internal_error"; break;
    }

    int n = snprintf(tmp, sizeof(tmp),
        "{\"id\":%lu,\"status\":\"%s\",\"schema\":%u,",
        (unsigned long)response->request_id,
        status_str,
        (unsigned)COMMAND_SCHEMA_VERSION);

    if (n > 0) tmp_size = (size_t)n;

    size_t payload_len = response->payload_size;
    size_t copy = payload_len;
    if (tmp_size + copy + 2 > COMMAND_RESPONSE_MAX_SIZE)
        copy = COMMAND_RESPONSE_MAX_SIZE - tmp_size - 2;

    if (copy > 0)
        memcpy(tmp + tmp_size, response->payload, copy);
    tmp_size += copy;

    if (tmp_size > 0 && tmp[tmp_size - 1] == ',')
        tmp_size--;

    n = snprintf(tmp + tmp_size, sizeof(tmp) - tmp_size, "}\n");
    if (n > 0) tmp_size += (size_t)n;

    memcpy(response->payload, tmp, tmp_size);
    response->payload_size = tmp_size;
}