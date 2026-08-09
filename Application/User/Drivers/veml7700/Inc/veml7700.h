#ifndef VEML7700_H
#define VEML7700_H

#include "stm32g4xx_hal.h"
#include <stdbool.h>

#define VEML7700_I2C_ADDR  (0x10U << 1U)
#define VEML7700_I2C_TIMEOUT_MS 100U

typedef struct
{
    I2C_HandleTypeDef *hi2c;
    uint8_t           initialized;
} VEML7700_HandleTypeDef;

bool VEML7700_Init(VEML7700_HandleTypeDef *dev, I2C_HandleTypeDef *hi2c);
bool VEML7700_ReadLux(VEML7700_HandleTypeDef *dev, float *lux);
bool VEML7700_ReadAlsRaw(VEML7700_HandleTypeDef *dev, uint16_t *raw);
bool VEML7700_IsInitialized(const VEML7700_HandleTypeDef *dev);

#endif