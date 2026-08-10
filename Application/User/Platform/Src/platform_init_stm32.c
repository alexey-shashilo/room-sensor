#include "platform_init.h"
#include "i2c_bus.h"
#include "platform_gpio.h"
#include "stm32g4xx_hal.h"

/* Forward declarations from main.c (CubeMX generated) */
void SystemClock_Config(void);
void MX_GPIO_Init(void);
void MX_I2C1_Init(void);

/* The I2C bus object is registered by main.c via Platform_RegisterI2cBus */
static const I2cBus *s_registered_i2c = NULL;

const I2cBus *Platform_GetI2c(void)
{
    return s_registered_i2c;
}

void Platform_RegisterI2c(const I2cBus *bus)
{
    s_registered_i2c = bus;
}

bool Platform_Init(void)
{
    /* Clock, GPIO, I2C are configured by CubeMX in main.c before Platform_Init.
       This function is the binding point between CubeMX objects and Platform API. */

    return true;
}

void Platform_ErrorHandler(void)
{
    __disable_irq();
    Platform_LedOn(PLATFORM_LED_GREEN);
    while (1)
    {
    }
}