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
static CommandSourceTrust s_pending_trust = COMMAND_SOURCE_UNTRUSTED;

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

void Command_UpdateRuntime(uint32_t uptime, bool wdg, const DeviceRuntime *light, const DeviceRuntime *disp, const DeviceRuntime *co2, ResetCause rc)
{
    s_services.uptime_ms = uptime;
    s_services.watchdog_active = wdg;
    if (light) s_services.light_sensor = *light;
    if (disp)  s_services.display = *disp;
    if (co2)   s_services.co2_sensor = *co2;
    s_services.reset_cause = rc;
}

void Command_SetPort(const CommunicationPort *port)
{
    s_cmd_port = port;
}

static bool EnqueueCommand(const uint8_t *data, size_t size, CommandSourceTrust trust)
{
    if (!s_initialized || data == NULL || size == 0) return false;
    if (s_has_pending) return false;

    size_t copy = size;
    if (copy > COMMAND_INPUT_BUFFER_SIZE - 1)
        return false;

    memcpy(s_input_buf, data, copy);
    s_input_buf[copy] = '\0';
    s_input_pos = copy;
    s_has_pending = true;
    s_pending_trust = trust;
    return true;
}

bool Command_ProcessBuffer(const uint8_t *data, size_t size)
{
    return EnqueueCommand(data, size, COMMAND_SOURCE_UNTRUSTED);
}

bool Command_ProcessInput(const CommandInput *input)
{
    if (input == NULL) return false;
    return EnqueueCommand(input->data, input->size, input->trust);
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

    /* Authorization per message — trust is part of the input, not global state */
    if (!CommandAuthorization_IsAllowed(request.type, s_pending_trust))
    {
        CommandResponse_Init(&response, request.request_id, COMMAND_STATUS_UNAUTHORIZED);
        CommandResponse_Append(&response, "\"error\":\"unauthorized\"");
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

CommandSecurityClass Command_GetSecurityClass(CommandType type)
{
    switch (type)
    {
        case COMMAND_GET_STATUS:
        case COMMAND_GET_CONFIG:
        case COMMAND_GET_IDENTITY:
        case COMMAND_GET_CAPABILITIES:
        case COMMAND_GET_MANIFEST:
        case COMMAND_GET_PROVISIONING_STATUS:
            return COMMAND_SECURITY_READ_ONLY;

        case COMMAND_SELF_TEST:
            return COMMAND_SECURITY_DIAGNOSTIC_ACTION;

        case COMMAND_SET_CONFIG:
        case COMMAND_RESET_CONFIG:
            return COMMAND_SECURITY_CONFIG_MUTATION;

        case COMMAND_REGISTER_DEVICE:
        case COMMAND_ASSIGN_LOCATION:
            return COMMAND_SECURITY_PROVISIONING_MUTATION;

        case COMMAND_UNREGISTER_DEVICE:
        case COMMAND_REBOOT:
        case COMMAND_FACTORY_RESET:
            return COMMAND_SECURITY_DESTRUCTIVE;

        case COMMAND_UNKNOWN:
        default:
            return COMMAND_SECURITY_INVALID;
    }
}

bool CommandAuthorization_IsAllowed(CommandType type, CommandSourceTrust trust)
{
    CommandSecurityClass cls = Command_GetSecurityClass(type);

    if (cls == COMMAND_SECURITY_INVALID)
        return false;

    switch (trust)
    {
        case COMMAND_SOURCE_TRUSTED_LOCAL:
            return true;

        case COMMAND_SOURCE_AUTHENTICATED_REMOTE:
            return (cls == COMMAND_SECURITY_READ_ONLY ||
                    cls == COMMAND_SECURITY_DIAGNOSTIC_ACTION ||
                    cls == COMMAND_SECURITY_CONFIG_MUTATION ||
                    cls == COMMAND_SECURITY_PROVISIONING_MUTATION);

        case COMMAND_SOURCE_UNTRUSTED:
        default:
            return (cls == COMMAND_SECURITY_READ_ONLY);
    }
}