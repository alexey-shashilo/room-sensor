#include "command.h"
#include "command_parser.h"
#include "command_dispatcher.h"
#include "command_response.h"
#include "communication_port.h"
#include <string.h>

static bool s_busy = false;
static CommandServices s_services;
static bool s_initialized = false;

static uint8_t s_input_buf[COMMAND_INPUT_BUFFER_SIZE];
static size_t s_input_pos = 0;
static bool s_has_pending = false;

static const CommunicationPort *s_cmd_port = NULL;

bool Command_Init(CommandServices *services)
{
    if (services == NULL) return false;
    s_services = *services;
    s_busy = false;
    s_input_pos = 0;
    s_has_pending = false;
    s_initialized = true;
    return true;
}

void Command_UpdateRuntime(uint32_t uptime, bool wdg, const DeviceRuntime *light, const DeviceRuntime *disp, ResetCause rc)
{
    s_services.uptime_ms = uptime;
    s_services.watchdog_active = wdg;
    if (light) s_services.light_sensor = *light;
    if (disp)  s_services.display = *disp;
    s_services.reset_cause = rc;
}

void Command_SetPort(const CommunicationPort *port)
{
    s_cmd_port = port;
}

bool Command_ProcessBuffer(const uint8_t *data, size_t size)
{
    if (!s_initialized || data == NULL || size == 0) return false;
    if (s_has_pending) return false;

    size_t copy = size;
    if (copy > COMMAND_INPUT_BUFFER_SIZE - 1)
    {
        /* Request too large — reject */
        return false;
    }

    memcpy(s_input_buf, data, copy);
    s_input_buf[copy] = '\0';
    s_input_pos = copy;
    s_has_pending = true;
    return true;
}

void Command_Run(void)
{
    if (!s_initialized) return;
    if (!s_has_pending) return;
    if (s_busy) return;

    s_busy = true;

    CommandRequest request;
    CommandResponse response;

    if (!CommandParser_Parse(s_input_buf, s_input_pos, &request))
    {
        CommandResponse_Init(&response, 0, COMMAND_STATUS_INVALID_ARGUMENT);
        CommandResponse_Append(&response, "\"error\":\"parse_failed\"");
        CommandResponse_Finalize(&response);

        if (s_cmd_port)
            s_cmd_port->send(s_cmd_port->context, response.payload, response.payload_size);

        s_has_pending = false;
        s_busy = false;
        return;
    }

    CommandDispatcher_Dispatch(&request, &response, &s_services);

    if (s_cmd_port)
        s_cmd_port->send(s_cmd_port->context, response.payload, response.payload_size);

    memset(s_input_buf, 0, s_input_pos);
    s_input_pos = 0;
    s_has_pending = false;
    s_busy = false;
}