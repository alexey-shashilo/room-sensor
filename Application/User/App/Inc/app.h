#ifndef APP_H
#define APP_H

#include "room_sensor_types.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    float  illuminance_lux;
    bool   illuminance_valid;

    DeviceState veml7700_state;
    DeviceState display_state;

    DeviceCounters veml7700_counters;
    DeviceCounters display_counters;
} App_Status;

RoomSensor_Status App_Init(void);
void               App_Run(void);
void               App_GetStatus(App_Status *status);
void               App_SetI2C(void *bus);

#endif