#include "command_parser.h"
#include <string.h>
#include <stdio.h>

static bool MatchStr(const uint8_t *data, size_t size, size_t *pos, const char *str)
{
    size_t len = strlen(str);
    if (*pos + len > size) return false;
    if (memcmp(data + *pos, str, len) != 0) return false;
    *pos += len;
    return true;
}

static void SkipWhitespace(const uint8_t *data, size_t size, size_t *pos)
{
    while (*pos < size)
    {
        uint8_t c = data[*pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            (*pos)++;
        else
            break;
    }
}

static bool ParseString(const uint8_t *data, size_t size, size_t *pos, char *out, size_t out_max)
{
    SkipWhitespace(data, size, pos);
    if (*pos >= size || data[*pos] != '"') return false;
    (*pos)++;

    size_t out_pos = 0;
    while (*pos < size)
    {
        uint8_t c = data[*pos];
        if (c == '"') { (*pos)++; break; }
        if (out_pos < out_max - 1) out[out_pos++] = (char)c;
        (*pos)++;
    }
    out[out_pos] = '\0';
    return true;
}

static bool ParseUint(const uint8_t *data, size_t size, size_t *pos, uint32_t *val)
{
    SkipWhitespace(data, size, pos);
    *val = 0;
    bool found = false;
    while (*pos < size && data[*pos] >= '0' && data[*pos] <= '9')
    {
        *val = (*val * 10U) + (uint32_t)(data[*pos] - '0');
        (*pos)++;
        found = true;
    }
    return found;
}

static CommandType StrToType(const char *str)
{
    if (strcmp(str, "GET_STATUS") == 0)       return COMMAND_GET_STATUS;
    if (strcmp(str, "GET_CONFIG") == 0)       return COMMAND_GET_CONFIG;
    if (strcmp(str, "SET_CONFIG") == 0)       return COMMAND_SET_CONFIG;
    if (strcmp(str, "RESET_CONFIG") == 0)     return COMMAND_RESET_CONFIG;
    if (strcmp(str, "GET_IDENTITY") == 0)     return COMMAND_GET_IDENTITY;
    if (strcmp(str, "SELF_TEST") == 0)        return COMMAND_SELF_TEST;
    if (strcmp(str, "REBOOT") == 0)           return COMMAND_REBOOT;
    if (strcmp(str, "GET_CAPABILITIES") == 0) return COMMAND_GET_CAPABILITIES;
    return COMMAND_UNKNOWN;
}

bool CommandParser_Parse(const uint8_t *data, size_t size, CommandRequest *request)
{
    if ((data == NULL) || (request == NULL)) return false;

    memset(request, 0, sizeof(*request));
    request->type = COMMAND_UNKNOWN;

    size_t pos = 0;
    SkipWhitespace(data, size, &pos);

    if (!MatchStr(data, size, &pos, "{")) return false;

    char field[64];

    while (pos < size)
    {
        SkipWhitespace(data, size, &pos);
        if (pos >= size) break;

        if (data[pos] == '}') { pos++; break; }

        if (!ParseString(data, size, &pos, field, sizeof(field)))
            return false;

        SkipWhitespace(data, size, &pos);
        if (!MatchStr(data, size, &pos, ":")) return false;

        if (strcmp(field, "id") == 0)
        {
            uint32_t id;
            if (!ParseUint(data, size, &pos, &id)) return false;
            request->request_id = id;
        }
        else if (strcmp(field, "command") == 0)
        {
            char cmd[32];
            if (!ParseString(data, size, &pos, cmd, sizeof(cmd))) return false;
            request->type = StrToType(cmd);
        }
        else
        {
            SkipWhitespace(data, size, &pos);
            if (pos < size && data[pos] == '"')
            {
                char val[COMMAND_INPUT_BUFFER_SIZE];
                ParseString(data, size, &pos, val, sizeof(val));
            }
            else
            {
                uint32_t uv;
                ParseUint(data, size, &pos, &uv);
            }
        }

        SkipWhitespace(data, size, &pos);
        if (pos < size && data[pos] == ',') pos++;
    }

    return true;
}