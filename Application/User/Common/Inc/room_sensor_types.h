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
    DEVICE_STATE_ERROR
} DeviceState;

typedef enum
{
    DRIVER_OK = 0,
    DRIVER_ERROR_ARGUMENT,
    DRIVER_ERROR_BUS,
    DRIVER_ERROR_TIMEOUT,
    DRIVER_ERROR_NOT_FOUND,
    DRIVER_ERROR_VERIFY
} DriverStatus;

typedef struct
{
    uint32_t read_success_count;
    uint32_t read_error_count;
    uint32_t init_error_count;
    uint32_t recovery_count;
    uint32_t last_success_ms;
    uint32_t last_error_ms;
} DeviceCounters;

#endif