#ifndef PLATFORM_GPIO_H
#define PLATFORM_GPIO_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    PLATFORM_LED_GREEN = 0,
    PLATFORM_LED_COUNT
} PlatformLed;

bool Platform_LedInit(PlatformLed led);
void Platform_LedOn(PlatformLed led);
void Platform_LedOff(PlatformLed led);
void Platform_LedToggle(PlatformLed led);

#endif