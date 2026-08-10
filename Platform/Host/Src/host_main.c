/* Host main stub — verifies portable core compiles and runs on host */

#include "app.h"
#include "room_state.h"
#include "config.h"
#include "device_identity.h"
#include "communication.h"
#include "communication_debug.h"
#include "i2c_bus.h"
#include "platform_init.h"
#include "platform_time.h"
#include "platform_unique_id.h"
#include "host_platform.h"
#include <stdio.h>

int main(void)
{
    /* Initialize host platform */
    HostFlash_Init();
    HostI2c_RegisterDevice(0x78);
    HostI2c_RegisterDevice(0x20);
    HostTime_Set(0);

    /* Create I2C bus */
    I2cBus i2c_bus;
    HostPlatform_GetI2cBus(&i2c_bus);
    Platform_RegisterI2c(&i2c_bus);
    App_SetI2C(&i2c_bus);
    Communication_Init();

    /* Communication debug port */
    CommunicationPort debug_port;
    CommunicationDebug_Init(&debug_port);
    Communication_SetPort(&debug_port);

    /* Boot application */
    if (App_Init() != ROOM_SENSOR_OK)
    {
        printf("App_Init failed\n");
        return 1;
    }

    printf("=== Host virtual device running ===\n");

    /* Run application with virtual time */
    for (int i = 0; i < 100; i++)
    {
        App_Run();
        HostTime_Advance(500);
    }

    printf("=== Host simulation complete ===\n");

    AppStatus status;
    App_GetStatus(&status);
    printf("Health: %d, Light sensor: %d, Display: %d\n",
           (int)status.health,
           (int)status.light_sensor.state,
           (int)status.display.state);

    return 0;
}