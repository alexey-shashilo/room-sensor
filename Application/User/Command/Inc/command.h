#ifndef COMMAND_H
#define COMMAND_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "room_sensor_types.h"
#include "room_state.h"
#include "config.h"
#include "device_identity.h"
#include "self_test.h"
#include "platform_reset.h"
#include "command_runtime_status.h"

#define COMMAND_SCHEMA_VERSION       1U
#define COMMAND_RESPONSE_MAX_SIZE    1024U
#define COMMAND_INPUT_BUFFER_SIZE    512U

typedef enum
{
    COMMAND_NONE = 0,
    COMMAND_GET_STATUS,
    COMMAND_GET_CONFIG,
    COMMAND_SET_CONFIG,
    COMMAND_RESET_CONFIG,
    COMMAND_GET_IDENTITY,
    COMMAND_SELF_TEST,
    COMMAND_REBOOT,
    COMMAND_GET_CAPABILITIES,
    COMMAND_GET_MANIFEST,
    COMMAND_REGISTER_DEVICE,
    COMMAND_UNREGISTER_DEVICE,
    COMMAND_FACTORY_RESET,
    COMMAND_GET_PROVISIONING_STATUS,
    COMMAND_ASSIGN_LOCATION,
    COMMAND_UNKNOWN
} CommandType;

typedef enum
{
    COMMAND_STATUS_OK = 0,
    COMMAND_STATUS_INVALID_COMMAND,
    COMMAND_STATUS_INVALID_ARGUMENT,
    COMMAND_STATUS_BUSY,
    COMMAND_STATUS_NOT_SUPPORTED,
    COMMAND_STATUS_CONFLICT,
    COMMAND_STATUS_INTERNAL_ERROR,
    COMMAND_STATUS_UNAUTHORIZED
} CommandStatus;

typedef struct
{
    bool has_light_period_ms;
    uint32_t light_period_ms;

    bool has_display_period_ms;
    uint32_t display_period_ms;

    bool has_telemetry_period_ms;
    uint32_t telemetry_period_ms;

    bool has_light_calibration;
    float light_calibration;

    bool has_installation_id;
    uint8_t installation_id[16];

    bool has_building_id;
    uint8_t building_id[16];

    bool has_room_id;
    uint8_t room_id[16];
} CommandArguments;

typedef struct
{
    CommandType       type;
    uint32_t          request_id;
    CommandArguments  args;
    bool              has_request_id;
} CommandRequest;

typedef struct
{
    uint32_t     request_id;
    CommandStatus status;
    bool         overflowed;
    uint8_t      payload[COMMAND_RESPONSE_MAX_SIZE];
    size_t       payload_size;
} CommandResponse;

typedef enum
{
    COMMAND_SECURITY_READ_ONLY,
    COMMAND_SECURITY_DIAGNOSTIC_ACTION,
    COMMAND_SECURITY_CONFIG_MUTATION,
    COMMAND_SECURITY_PROVISIONING_MUTATION,
    COMMAND_SECURITY_DESTRUCTIVE,
    COMMAND_SECURITY_INVALID
} CommandSecurityClass;

typedef enum
{
    COMMAND_SOURCE_UNTRUSTED = 0,          /* default = fail closed */
    COMMAND_SOURCE_TRUSTED_LOCAL,
    COMMAND_SOURCE_AUTHENTICATED_REMOTE
} CommandSourceTrust;

typedef struct
{
    const RoomState        *room;
    const RoomSensorConfig *config;
    const DeviceIdentity   *identity;
    SelfTestReport *self_test;
    struct I2cBus          *bus;

    uint32_t uptime_ms;
    bool     watchdog_active;
    DeviceRuntime light_sensor;
    DeviceRuntime display;
    DeviceRuntime co2_sensor;
    ResetCause reset_cause;

    /* Authoritative current runtime-status snapshot, filled by App (owning the
       underlying state) before Command_Run(). GET_STATUS MUST use this for
       storage/config/identity/provisioning health instead of the SelfTestReport
       (which only records what a previous diagnostic observed). Command has NO
       dependency on App concrete types; this is a plain portable DTO. */
    const CommandRuntimeStatus *runtime_status;
} CommandServices;

typedef struct
{
    const uint8_t *data;
    size_t size;
    CommandSourceTrust trust;
} CommandInput;

bool Command_Init(CommandServices *services);
void Command_UpdateRuntime(uint32_t uptime, bool wdg, const DeviceRuntime *light, const DeviceRuntime *disp, const DeviceRuntime *co2, ResetCause rc);
void Command_Run(void);
/* Legacy entry point retained for backward compatibility/tests. It enqueues a
   command as COMMAND_SOURCE_UNTRUSTED (fail closed). Prefer the message-scoped
   command entry point Command_ProcessInput() for production transport. */
bool Command_ProcessBuffer(const uint8_t *data, size_t size);
bool Command_ProcessInput(const CommandInput *input);
CommandSecurityClass Command_GetSecurityClass(CommandType type);
bool CommandAuthorization_IsAllowed(CommandType type, CommandSourceTrust trust);

#endif