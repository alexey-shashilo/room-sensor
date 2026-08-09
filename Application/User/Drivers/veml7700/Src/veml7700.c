#include "veml7700.h"
#include "i2c_bus.h"
#include <string.h>

static const float s_lux_scales[4][16] = {
    { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 0.0f, 1.8432f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 0.0f, 0.9216f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 0.0f, 0.2304f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 0.0f, 0.4608f, 0.0f, 0.0f, 0.0f },
};

static float VEML7700_ComputeLuxScale(VEML7700_Gain gain, VEML7700_IntegrationTime it)
{
    if ((unsigned)gain > 3U) return 0.0f;
    if ((unsigned)it > 0xFU) return 0.0f;
    return s_lux_scales[(unsigned)gain][(unsigned)it];
}

static uint16_t VEML7700_BuildConf(VEML7700_Gain gain, VEML7700_IntegrationTime it, VEML7700_Persistence pers)
{
    uint16_t conf = 0U;
    conf |= ((uint16_t)gain << VEML7700_GAIN_BITS_POS);
    conf |= ((uint16_t)it   << VEML7700_IT_BITS_POS);
    conf |= ((uint16_t)pers << VEML7700_PERS_BITS_POS);
    return conf;
}

bool VEML7700_Probe(VEML7700_HandleTypeDef *dev, void *i2c_bus)
{
    if ((dev == NULL) || (i2c_bus == NULL)) return false;
    const I2cBus *bus = (const I2cBus *)i2c_bus;
    return (I2cBus_Probe(bus, VEML7700_I2C_ADDR) == DRIVER_OK);
}

bool VEML7700_Init(VEML7700_HandleTypeDef *dev, void *i2c_bus,
                   VEML7700_Gain gain,
                   VEML7700_IntegrationTime it,
                   VEML7700_Persistence pers)
{
    if ((dev == NULL) || (i2c_bus == NULL)) return false;

    memset(dev, 0, sizeof(*dev));
    dev->i2c_bus = i2c_bus;

    const I2cBus *bus = (const I2cBus *)i2c_bus;

    if (I2cBus_Probe(bus, VEML7700_I2C_ADDR) != DRIVER_OK)
    {
        return false;
    }

    dev->gain = gain;
    dev->integration_time = it;
    dev->persistence = pers;
    dev->als_conf_value = VEML7700_BuildConf(gain, it, pers);

    dev->lux_scale = VEML7700_ComputeLuxScale(gain, it);
    if (dev->lux_scale <= 0.0f)
    {
        return false;
    }

    uint8_t tx[3];
    tx[0] = VEML7700_REG_ALS_CONF;
    tx[1] = (uint8_t)(dev->als_conf_value & 0xFFU);
    tx[2] = (uint8_t)(dev->als_conf_value >> 8U);

    if (I2cBus_Write(bus, VEML7700_I2C_ADDR, tx, 3U) != DRIVER_OK)
    {
        dev->counters.init_error_count++;
        return false;
    }

    uint8_t rx[2];
    if (I2cBus_ReadMem(bus, VEML7700_I2C_ADDR, VEML7700_REG_ALS_CONF, rx, 2U) != DRIVER_OK)
    {
        dev->counters.init_error_count++;
        return false;
    }

    uint16_t readback = (uint16_t)rx[1] << 8U | (uint16_t)rx[0];
    uint16_t mask = (3U << 11U) | (0xFU << 6U) | (3U << 4U) | (1U << 1U) | 1U;
    if ((readback & mask) != (dev->als_conf_value & mask))
    {
        dev->counters.init_error_count++;
        return false;
    }

    dev->initialized = 1U;
    return true;
}

bool VEML7700_ReadRaw(VEML7700_HandleTypeDef *dev, uint16_t *raw)
{
    if ((dev == NULL) || (raw == NULL)) return false;
    if (dev->initialized == 0U) return false;

    const I2cBus *bus = (const I2cBus *)dev->i2c_bus;
    uint8_t rx[2];

    if (I2cBus_ReadMem(bus, VEML7700_I2C_ADDR, VEML7700_REG_ALS, rx, 2U) != DRIVER_OK)
    {
        dev->counters.read_error_count++;
        return false;
    }

    *raw = (uint16_t)rx[1] << 8U | (uint16_t)rx[0];
    dev->counters.read_success_count++;
    return true;
}

bool VEML7700_ReadLux(VEML7700_HandleTypeDef *dev, float *lux)
{
    uint16_t raw;
    if (!VEML7700_ReadRaw(dev, &raw)) return false;
    *lux = (float)raw * dev->lux_scale;
    return true;
}

bool VEML7700_IsInitialized(const VEML7700_HandleTypeDef *dev)
{
    if (dev == NULL) return false;
    return (dev->initialized != 0U);
}