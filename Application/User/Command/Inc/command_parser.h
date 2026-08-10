#ifndef COMMAND_PARSER_H
#define COMMAND_PARSER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "command.h"

bool CommandParser_Parse(const uint8_t *data, size_t size, CommandRequest *request);

#endif