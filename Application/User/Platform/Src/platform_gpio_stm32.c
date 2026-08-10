#include "platform_gpio.h"
#include "stm32g4xx_nucleo.h"

bool Platform_LedInit(PlatformLed led)
{
    if (led == PLATFORM_LED_GREEN)
    {
        BSP_LED_Init(LED_GREEN);
        return true;
    }
    return false;
}

void Platform_LedOn(PlatformLed led)
{
    if (led == PLATFORM_LED_GREEN)
        BSP_LED_On(LED_GREEN);
}

void Platform_LedOff(PlatformLed led)
{
    if (led == PLATFORM_LED_GREEN)
        BSP_LED_Off(LED_GREEN);
}

void Platform_LedToggle(PlatformLed led)
{
    if (led == PLATFORM_LED_GREEN)
        BSP_LED_Toggle(LED_GREEN);
}