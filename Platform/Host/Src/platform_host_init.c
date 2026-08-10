/* Host Platform — Init binding */

#include "host_platform.h"
#include "platform_init.h"
#include "i2c_bus.h"

static const I2cBus *s_registered_i2c = NULL;

bool Platform_Init(void) { return true; }
void Platform_RegisterI2c(const I2cBus *bus) { s_registered_i2c = bus; }
const I2cBus *Platform_GetI2c(void) { return s_registered_i2c; }
void Platform_ErrorHandler(void) { }