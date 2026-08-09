#include "veml7700.h"

#define REG_ALS_CONF        0x00U
#define REG_ALS             0x04U
#define REG_WH              0x01U

#define ALS_CONF_DEFAULT    0x0000U

#define ALS_CONF_GAIN_1_8   (3U << 11U)
#define ALS_CONF_GAIN_1_4   (2U << 11U)
#define ALS_CONF_GAIN_1     (0U << 11U)
#define ALS_CONF_GAIN_2     (1U << 11U)

#define ALS_CONF_IT_25MS    (0U << 6U)
#define ALS_CONF_IT_50MS    (1U << 6U)
#define ALS_CONF_IT_100MS   (2U << 6U)
#define ALS_CONF_IT_200MS   (3U << 6U)

#define ALS_CONF_PERS_1     (0U << 4U)
#define ALS_CONF_PERS_2     (1U << 4U)
#define ALS_CONF_PERS_4     (2U << 4U)
#define ALS_CONF_PERS_8     (3U << 4U)

#define ALS_CONF_ACTIVE     (ALS_CONF_GAIN_1_8 | ALS_CONF_IT_25MS | ALS_CONF_PERS_1)

static const float ALS_LUX_SCALE = 0.4608f;

bool VEML7700_Init(
    VEML7700_HandleTypeDef *dev,
    I2C_HandleTypeDef *hi2c)
{
    if ((dev == NULL) || (hi2c == NULL))
    {
        return false;
    }

    dev->hi2c = hi2c;
    dev->initialized = 0U;

    uint16_t als_conf = ALS_CONF_ACTIVE;
    uint8_t tx[3];
    tx[0] = REG_ALS_CONF;
    tx[1] = (uint8_t)(als_conf & 0xFFU);
    tx[2] = (uint8_t)(als_conf >> 8U);

    if (HAL_I2C_Master_Transmit(dev->hi2c, VEML7700_I2C_ADDR, tx, 3U, VEML7700_I2C_TIMEOUT_MS) != HAL_OK)
    {
        return false;
    }

    HAL_Delay(50);

    dev->initialized = 1U;

    return true;
}

bool VEML7700_ReadLux(
    VEML7700_HandleTypeDef *dev,
    float *lux)
{
    uint8_t rx[2];

    if ((dev == NULL) || (lux == NULL))
    {
        return false;
    }

    if (dev->initialized == 0U)
    {
        return false;
    }

    if (HAL_I2C_Mem_Read(dev->hi2c, VEML7700_I2C_ADDR, REG_ALS, I2C_MEMADD_SIZE_8BIT, rx, 2U, VEML7700_I2C_TIMEOUT_MS) != HAL_OK)
    {
        return false;
    }

    uint16_t raw = (uint16_t)rx[1] << 8U | (uint16_t)rx[0];

    *lux = (float)raw * ALS_LUX_SCALE;

    return true;
}

bool VEML7700_ReadAlsRaw(VEML7700_HandleTypeDef *dev, uint16_t *raw)
{
    uint8_t rx[2];

    if ((dev == NULL) || (raw == NULL)) return false;
    if (dev->initialized == 0U) return false;

    if (HAL_I2C_Mem_Read(dev->hi2c, VEML7700_I2C_ADDR, REG_ALS, I2C_MEMADD_SIZE_8BIT, rx, 2U, VEML7700_I2C_TIMEOUT_MS) != HAL_OK) return false;

    *raw = (uint16_t)rx[1] << 8U | (uint16_t)rx[0];
    return true;
}

bool VEML7700_IsInitialized(const VEML7700_HandleTypeDef *dev)
{
    if (dev == NULL)
    {
        return false;
    }

    return (dev->initialized != 0U);
}