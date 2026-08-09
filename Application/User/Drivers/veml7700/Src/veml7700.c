#include "veml7700.h"
#include "i2c_bus.h"
#include "platform_time.h"
#include <string.h>

#define VEML7700_ALS_CONF_MASK ((3U << 11U) | (0xFU << 6U) | (3U << 4U) | (1U << 1U) | 1U)

static const float s_gain_sensitivity[4] = {
    1.0f,    /* GAIN_1   = ×1     */
    2.0f,    /* GAIN_2   = ×2     */
    0.125f,  /* GAIN_1_8 = ×1/8   */
    0.25f    /* GAIN_1_4 = ×1/4   */
};

static inline float VEML7700_GainSensitivity(VEML7700_Gain g)
{
    unsigned idx = (unsigned)g;
    return (idx < 4U) ? s_gain_sensitivity[idx] : 0.0f;
}

static float VEML7700_ItMs(VEML7700_IntegrationTime it)
{
    switch (it)
    {
        case VEML7700_IT_25_MS:  return 25.0f;
        case VEML7700_IT_50_MS:  return 50.0f;
        case VEML7700_IT_100_MS: return 100.0f;
        case VEML7700_IT_200_MS: return 200.0f;
        case VEML7700_IT_400_MS: return 400.0f;
        case VEML7700_IT_800_MS: return 800.0f;
        default:                 return 0.0f;
    }
}

static float VEML7700_ComputeResolution(VEML7700_Gain gain, VEML7700_IntegrationTime it)
{
    float sens = VEML7700_GainSensitivity(gain);
    float it_ms = VEML7700_ItMs(it);
    if ((sens <= 0.0f) || (it_ms <= 0.0f)) return 0.0f;
    return 0.0576f / sens * (100.0f / it_ms);
}

static uint16_t VEML7700_BuildConf(VEML7700_Gain g, VEML7700_IntegrationTime it, VEML7700_Persistence p)
{
    return ((uint16_t)g  << VEML7700_GAIN_BITS_POS)
         | ((uint16_t)it << VEML7700_IT_BITS_POS)
         | ((uint16_t)p  << VEML7700_PERS_BITS_POS);
}

static bool VEML7700_ApplyConfig(VEML7700_HandleTypeDef *dev, VEML7700_Gain gain, VEML7700_IntegrationTime it)
{
    uint16_t conf = VEML7700_BuildConf(gain, it, dev->persistence);
    float res = VEML7700_ComputeResolution(gain, it);
    if (res <= 0.0f) return false;

    uint8_t tx[3] = {VEML7700_REG_ALS_CONF, (uint8_t)(conf & 0xFFU), (uint8_t)(conf >> 8U)};
    if (I2cBus_Write(dev->bus, VEML7700_I2C_ADDR, tx, 3U) != DRIVER_STATUS_OK) return false;

    uint8_t rx[2];
    if (I2cBus_ReadMem(dev->bus, VEML7700_I2C_ADDR, VEML7700_REG_ALS_CONF, rx, 2U) != DRIVER_STATUS_OK) return false;

    uint16_t readback = (uint16_t)rx[1] << 8U | (uint16_t)rx[0];
    if ((readback & VEML7700_ALS_CONF_MASK) != (conf & VEML7700_ALS_CONF_MASK)) return false;

    dev->gain = gain;
    dev->integration_time = it;
    dev->als_conf_value = conf;
    dev->resolution = res;
    dev->last_it_ms = (uint32_t)(VEML7700_ItMs(it) * 1.5f);
    return true;
}

static VEML7700_IntegrationTime VEML7700_SlowerIT(VEML7700_IntegrationTime current)
{
    switch (current)
    {
        case VEML7700_IT_25_MS:  return VEML7700_IT_50_MS;
        case VEML7700_IT_50_MS:  return VEML7700_IT_100_MS;
        case VEML7700_IT_100_MS: return VEML7700_IT_200_MS;
        case VEML7700_IT_200_MS: return VEML7700_IT_400_MS;
        case VEML7700_IT_400_MS: return VEML7700_IT_800_MS;
        default:                 return VEML7700_IT_800_MS;
    }
}

static VEML7700_IntegrationTime VEML7700_FasterIT(VEML7700_IntegrationTime current)
{
    switch (current)
    {
        case VEML7700_IT_800_MS: return VEML7700_IT_400_MS;
        case VEML7700_IT_400_MS: return VEML7700_IT_200_MS;
        case VEML7700_IT_200_MS: return VEML7700_IT_100_MS;
        case VEML7700_IT_100_MS: return VEML7700_IT_50_MS;
        case VEML7700_IT_50_MS:  return VEML7700_IT_25_MS;
        default:                 return VEML7700_IT_25_MS;
    }
}

static VEML7700_Gain VEML7700_NextMoreSensitiveGain(VEML7700_Gain current)
{
    switch (current)
    {
        case VEML7700_GAIN_1_8: return VEML7700_GAIN_1_4;
        case VEML7700_GAIN_1_4: return VEML7700_GAIN_1;
        case VEML7700_GAIN_1:   return VEML7700_GAIN_2;
        default:                return VEML7700_GAIN_2;
    }
}

static VEML7700_Gain VEML7700_NextLessSensitiveGain(VEML7700_Gain current)
{
    switch (current)
    {
        case VEML7700_GAIN_2:   return VEML7700_GAIN_1;
        case VEML7700_GAIN_1:   return VEML7700_GAIN_1_4;
        case VEML7700_GAIN_1_4: return VEML7700_GAIN_1_8;
        default:                return VEML7700_GAIN_1_8;
    }
}

static void VEML7700_EnterSettling(VEML7700_HandleTypeDef *dev)
{
    dev->range_state = VEML7700_RANGE_SETTLING;
    dev->range_consecutive = 0U;
    dev->range_settle_until_ms = Platform_GetTickMs() + dev->last_it_ms;
}

static void VEML7700_IncreaseSensitivity(VEML7700_HandleTypeDef *dev)
{
    VEML7700_IntegrationTime it = dev->integration_time;
    VEML7700_Gain gain = dev->gain;

    if (it != VEML7700_IT_800_MS)
    {
        it = VEML7700_SlowerIT(it);
    }
    else
    {
        VEML7700_Gain next = VEML7700_NextMoreSensitiveGain(gain);
        if (next == gain) return;
        gain = next;
        it = VEML7700_IT_25_MS;
    }

    if (VEML7700_ApplyConfig(dev, gain, it))
        VEML7700_EnterSettling(dev);
}

static void VEML7700_DecreaseSensitivity(VEML7700_HandleTypeDef *dev)
{
    VEML7700_IntegrationTime it = dev->integration_time;
    VEML7700_Gain gain = dev->gain;

    if (it != VEML7700_IT_25_MS)
    {
        it = VEML7700_FasterIT(it);
    }
    else
    {
        VEML7700_Gain next = VEML7700_NextLessSensitiveGain(gain);
        if (next == gain) return;
        gain = next;
        it = VEML7700_IT_800_MS;
    }

    if (VEML7700_ApplyConfig(dev, gain, it))
        VEML7700_EnterSettling(dev);
}

bool VEML7700_Probe(const I2cBus *bus)
{
    if (bus == NULL) return false;
    return (I2cBus_Probe(bus, VEML7700_I2C_ADDR) == DRIVER_STATUS_OK);
}

bool VEML7700_Init(VEML7700_HandleTypeDef *dev, const I2cBus *bus)
{
    if ((dev == NULL) || (bus == NULL)) return false;

    memset(dev, 0, sizeof(*dev));
    dev->bus = bus;

    if (I2cBus_Probe(bus, VEML7700_I2C_ADDR) != DRIVER_STATUS_OK) return false;

    dev->persistence = VEML7700_PERS_1;

    if (!VEML7700_ApplyConfig(dev, VEML7700_GAIN_1, VEML7700_IT_100_MS))
    {
        dev->counters.config_error_count++;
        return false;
    }

    VEML7700_EnterSettling(dev);
    dev->initialized = 1U;
    return true;
}

bool VEML7700_ReadWithAutoRange(VEML7700_HandleTypeDef *dev, VEML7700_Sample *sample)
{
    if ((dev == NULL) || (sample == NULL)) return false;
    if (dev->initialized == 0U) return false;

    memset(sample, 0, sizeof(*sample));
    sample->gain = dev->gain;
    sample->integration_time = dev->integration_time;
    sample->resolution = dev->resolution;
    sample->valid = false;
    sample->range_changed = false;

    uint8_t rx[2];
    if (I2cBus_ReadMem(dev->bus, VEML7700_I2C_ADDR, VEML7700_REG_ALS, rx, 2U) != DRIVER_STATUS_OK)
    {
        dev->counters.read_error_count++;
        return false;
    }

    uint16_t raw = (uint16_t)rx[1] << 8U | (uint16_t)rx[0];
    sample->als_raw = raw;

    uint32_t now = Platform_GetTickMs();

    if (raw >= VEML7700_RANGE_SATURATION)
    {
        VEML7700_DecreaseSensitivity(dev);
        sample->range_changed = true;
        dev->counters.read_success_count++;
        return true;
    }

    if (dev->range_state == VEML7700_RANGE_SETTLING)
    {
        uint32_t elapsed = now - (dev->range_settle_until_ms - dev->last_it_ms);
        if (elapsed < dev->last_it_ms)
        {
            dev->counters.read_success_count++;
            return true;
        }
        dev->range_state = VEML7700_RANGE_STABLE;
        dev->range_consecutive = 0U;
    }

    if (raw < VEML7700_RANGE_LOW_THRESHOLD)
    {
        dev->range_consecutive++;
        if (dev->range_consecutive >= VEML7700_RANGE_CONVERGE_SAMPLES)
        {
            VEML7700_IncreaseSensitivity(dev);
            sample->range_changed = true;
            return true;
        }
    }
    else if (raw > VEML7700_RANGE_HIGH_THRESHOLD)
    {
        dev->range_consecutive++;
        if (dev->range_consecutive >= VEML7700_RANGE_CONVERGE_SAMPLES)
        {
            VEML7700_DecreaseSensitivity(dev);
            sample->range_changed = true;
            return true;
        }
    }
    else
    {
        dev->range_consecutive = 0U;
    }

    sample->lux = (float)raw * dev->resolution;
    sample->valid = true;

    dev->last_sample = *sample;
    dev->counters.read_success_count++;

    return true;
}

bool VEML7700_GetDiagnostics(const VEML7700_HandleTypeDef *dev, VEML7700_Sample *diag)
{
    if ((dev == NULL) || (diag == NULL)) return false;
    *diag = dev->last_sample;
    return diag->valid;
}

bool VEML7700_IsInitialized(const VEML7700_HandleTypeDef *dev)
{
    if (dev == NULL) return false;
    return (dev->initialized != 0U);
}