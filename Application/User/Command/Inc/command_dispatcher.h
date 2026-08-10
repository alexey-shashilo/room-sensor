#ifndef COMMAND_DISPATCHER_H
#define COMMAND_DISPATCHER_H

#include "command.h"

bool CommandDispatcher_Dispatch(
    const CommandRequest *request,
    CommandResponse *response,
    const CommandServices *services);

#endif