#ifndef I2C_BUS_STM32_H
#define I2C_BUS_STM32_H

#include "i2c_bus.h"
#include "stm32g4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

void I2cBus_Stm32_Init(I2cBus *bus, I2C_HandleTypeDef *hi2c, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif