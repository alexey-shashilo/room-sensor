#ifndef COMMAND_H
#define COMMAND_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "room_sensor_types.h"
#include "room_state.h"
#include "device_identity.h"
#include "self_test.h"
#include "config.h"
#include "platform_reset.h"

#define COMMAND_SCHEMA_VERSION     1U
#define COMMAND_RESPONSE_MAX_SIZE  1024U
#define COMMAND_INPUT_BUFFER_SIZE  512U

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
    COMMAND_UNKNOWN
} CommandType;

typedef enum
{
    COMMAND_STATUS_OK = 0,
    COMMAND_STATUS_INVALID_COMMAND,
    COMMAND_STATUS_INVALID_ARGUMENT,
    COMMAND_STATUS_BUSY,
    COMMAND_STATUS_NOT_SUPPORTED,
    COMMAND_STATUS_INTERNAL_ERROR
} CommandStatus;

typedef struct
{
    CommandType type;
    uint32_t    request_id;
    const uint8_t *payload;
    size_t      payload_size;
} CommandRequest;

typedef struct
{
    uint32_t     request_id;
    CommandStatus status;
    uint8_t      payload[COMMAND_RESPONSE_MAX_SIZE];
    size_t       payload_size;
} CommandResponse;

typedef struct
{
    const RoomState            *room;
    const RoomSensorConfig     *config;
    const DeviceIdentity       *identity;
    const SelfTestReport       *self_test;
    struct I2cBus              *bus;

    uint32_t uptime_ms;
    bool     watchdog_active;
    DeviceRuntime light_sensor;
    DeviceRuntime display;
    ResetCause reset_cause;
} CommandServices;

bool Command_Init(CommandServices *services);
void Command_UpdateRuntime(uint32_t uptime, bool wdg, const DeviceRuntime *light, const DeviceRuntime *disp, ResetCause rc);
void Command_Run(void);
bool Command_ProcessBuffer(const uint8_t *data, size_t size);

#endif