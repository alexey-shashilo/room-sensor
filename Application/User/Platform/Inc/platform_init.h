#ifndef PLATFORM_INIT_H
#define PLATFORM_INIT_H

#include <stdbool.h>
#include "i2c_bus.h"

bool Platform_Init(void);
void Platform_RegisterI2c(const I2cBus *bus);
const I2cBus *Platform_GetI2c(void);
void Platform_ErrorHandler(void);

#endif