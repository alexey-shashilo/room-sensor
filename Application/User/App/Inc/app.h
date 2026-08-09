#ifndef APP_H
#define APP_H

#include "room_sensor_types.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    DeviceRuntime light_sensor;
    DeviceRuntime display;

    float  illuminance_lux;
    bool   illuminance_valid;

    uint32_t uptime_ms;
} AppStatus;

RoomSensor_Status App_Init(void);
void               App_Run(void);
void               App_GetStatus(AppStatus *status);
void               App_SetI2C(void *bus);

#endif