/* Host Platform — GPIO / LED */

#include "host_platform.h"
#include "platform_gpio.h"

static bool s_led_on = false;

bool HostLed_GetState(void) { return s_led_on; }

bool Platform_LedInit(PlatformLed led) { (void)led; s_led_on = false; return true; }
void Platform_LedOn(PlatformLed led) { (void)led; s_led_on = true; }
void Platform_LedOff(PlatformLed led) { (void)led; s_led_on = false; }
void Platform_LedToggle(PlatformLed led) { (void)led; s_led_on = !s_led_on; }