#ifndef APP_H
#define APP_H

#include "room_sensor_types.h"
#include "i2c_bus.h"
#include "self_test.h"
#include "platform_reset.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    SYSTEM_HEALTH_BOOTING = 0,
    SYSTEM_HEALTH_OK,
    SYSTEM_HEALTH_DEGRADED,
    SYSTEM_HEALTH_FAULT
} SystemHealthState;

typedef struct
{
    DeviceRuntime light_sensor;
    DeviceRuntime display;

    SystemHealthState health;
    ResetCause reset_cause;
    bool watchdog_active;

    SelfTestReport self_test;

    uint32_t uptime_ms;
} AppStatus;

RoomSensor_Status App_Init(void);
void               App_Run(void);
void               App_GetStatus(AppStatus *status);
void               App_SetI2C(const I2cBus *bus);
void               App_DoRetry(void);

#endif