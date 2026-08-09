#ifndef ROOM_SENSOR_TYPES_H
#define ROOM_SENSOR_TYPES_H

#include <stdint.h>
#include <stdbool.h>

typedef enum
{
    ROOM_SENSOR_OK       = 0,
    ROOM_SENSOR_ERROR    = -1,
    ROOM_SENSOR_TIMEOUT  = -2,
    ROOM_SENSOR_BUSY     = -3,
    ROOM_SENSOR_NOT_FOUND = -4
} RoomSensor_Status;

typedef enum
{
    DEVICE_STATE_UNKNOWN = 0,
    DEVICE_STATE_NOT_FOUND,
    DEVICE_STATE_PROBING,
    DEVICE_STATE_INITIALIZING,
    DEVICE_STATE_READY,
    DEVICE_STATE_ERROR,
    DEVICE_STATE_RECOVERING
} DeviceState;

typedef enum
{
    DRIVER_STATUS_OK = 0,
    DRIVER_STATUS_INVALID_ARG,
    DRIVER_STATUS_BUS_ERROR,
    DRIVER_STATUS_TIMEOUT,
    DRIVER_STATUS_NOT_FOUND,
    DRIVER_STATUS_VERIFY_ERROR,
    DRIVER_STATUS_NOT_READY
} DriverStatus;

#define CONSECUTIVE_ERROR_THRESHOLD 3U

typedef struct
{
    DeviceState state;

    uint32_t init_attempts;
    uint32_t init_failures;

    uint32_t operation_successes;
    uint32_t operation_failures;

    uint32_t recovery_count;
    uint32_t consecutive_errors;

    uint32_t last_success_ms;
    uint32_t last_failure_ms;
} DeviceRuntime;

#endif