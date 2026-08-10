#ifndef COMMAND_RESPONSE_H
#define COMMAND_RESPONSE_H

#include <stdint.h>
#include <stddef.h>
#include "command.h"

void CommandResponse_Init(CommandResponse *response, uint32_t request_id, CommandStatus status);
bool CommandResponse_Append(CommandResponse *response, const char *fmt, ...);
bool CommandResponse_AppendJson(CommandResponse *response, const char *key, const char *value);
bool CommandResponse_AppendJsonInt(CommandResponse *response, const char *key, uint32_t value);
bool CommandResponse_AppendJsonFloat(CommandResponse *response, const char *key, float value, int decimals);
bool CommandResponse_AppendJsonBool(CommandResponse *response, const char *key, bool value);
void CommandResponse_Finalize(CommandResponse *response);

#endif