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
    SENSOR_ID_VEML7700 = 0,
    SENSOR_ID_SCD41,
    SENSOR_ID_SHT45,
    SENSOR_ID_SGP41,
    SENSOR_ID_BMP390,
    SENSOR_ID_SSD1306,
    SENSOR_ID_LD2450,
    SENSOR_ID_COUNT
} SensorId;

#endif