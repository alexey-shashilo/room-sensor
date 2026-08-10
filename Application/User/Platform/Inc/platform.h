#ifndef PLATFORM_H
#define PLATFORM_H

/* Umbrella header: Application must include only this plus module-specific
   headers. All platform API is defined here. */

#include "platform_types.h"

/* I2C abstraction (canonical: i2c_bus.h) */
#include "i2c_bus.h"

/* Time */
#include "platform_time.h"

/* Delay */
#include "platform_delay.h"

/* Watchdog */
#include "platform_watchdog.h"

/* Reset cause */
#include "platform_reset.h"

/* Unique device ID */
#include "platform_unique_id.h"

/* Flash */
#include "platform_flash.h"

/* GPIO / LED */
#include "platform_gpio.h"

/* Platform init / binding */
#include "platform_init.h"

#endif