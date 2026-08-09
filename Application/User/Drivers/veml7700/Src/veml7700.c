#include "veml7700.h"
#include "i2c_bus.h"
#include "platform_time.h"
#include <string.h>

#define VEML7700_ALS_CONF_MASK ((3U << 11U) | (0xFU << 6U) | (3U << 4U) | (1U << 1U) | 1U)

typedef struct
{
    VEML7700_Gain gain;
    VEML7700_IntegrationTime it;
    float sensitivity;
    float resolution;
} RangeEntry;

static const RangeEntry s_ranges[VEML7700_RANGE_COUNT] = {
    { VEML7700_GAIN_1_8, VEML7700_IT_25_MS,   3.125f,  1.843200f },
    { VEML7700_GAIN_1_8, VEML7700_IT_50_MS,   6.25f,   0.921600f },
    { VEML7700_GAIN_1_4, VEML7700_IT_50_MS,  12.5f,    0.460800f },
    { VEML7700_GAIN_1,   VEML7700_IT_25_MS,  25.0f,    0.230400f },
    { VEML7700_GAIN_1,   VEML7700_IT_50_MS,  50.0f,    0.115200f },
    { VEML7700_GAIN_1,   VEML7700_IT_100_MS, 100.0f,   0.057600f },
    { VEML7700_GAIN_1,   VEML7700_IT_200_MS, 200.0f,   0.028800f },
    { VEML7700_GAIN_2,   VEML7700_IT_200_MS, 400.0f,   0.014400f },
    { VEML7700_GAIN_2,   VEML7700_IT_400_MS, 800.0f,   0.007200f },
    { VEML7700_GAIN_2,   VEML7700_IT_800_MS, 1600.0f,  0.003600f },
};

#ifdef VEML7700_UNIT_TEST
uint8_t   VEML7700_UT_GetRangeCount(void)       { return VEML7700_RANGE_COUNT; }
uint8_t   VEML7700_UT_GetRangeGain(uint8_t i)   { return (uint8_t)s_ranges[i].gain; }
uint8_t   VEML7700_UT_GetRangeIt(uint8_t i)     { return (uint8_t)s_ranges[i].it; }
float     VEML7700_UT_GetRangeSens(uint8_t i)   { return s_ranges[i].sensitivity; }
float     VEML7700_UT_GetRangeRes(uint8_t i)    { return s_ranges[i].resolution; }
#endif

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

static uint16_t VEML7700_BuildConf(VEML7700_Gain g, VEML7700_IntegrationTime it, VEML7700_Persistence p)
{
    return ((uint16_t)g  << VEML7700_GAIN_BITS_POS)
         | ((uint16_t)it << VEML7700_IT_BITS_POS)
         | ((uint16_t)p  << VEML7700_PERS_BITS_POS);
}

static bool VEML7700_ApplyIndex(VEML7700_HandleTypeDef *dev, uint8_t idx)
{
    if (idx >= VEML7700_RANGE_COUNT) return false;

    const RangeEntry *r = &s_ranges[idx];
    uint16_t conf = VEML7700_BuildConf(r->gain, r->it, dev->persistence);

    uint8_t tx[3] = {VEML7700_REG_ALS_CONF, (uint8_t)(conf & 0xFFU), (uint8_t)(conf >> 8U)};
    if (I2cBus_Write(dev->bus, VEML7700_I2C_ADDR, tx, 3U) != DRIVER_STATUS_OK) return false;

    uint8_t rx[2];
    if (I2cBus_ReadMem(dev->bus, VEML7700_I2C_ADDR, VEML7700_REG_ALS_CONF, rx, 2U) != DRIVER_STATUS_OK) return false;

    uint16_t readback = (uint16_t)rx[1] << 8U | (uint16_t)rx[0];
    if ((readback & VEML7700_ALS_CONF_MASK) != (conf & VEML7700_ALS_CONF_MASK))
    {
        return false;
    }

    dev->range_index = idx;
    dev->gain = r->gain;
    dev->integration_time = r->it;
    dev->als_conf_value = conf;
    dev->resolution = r->resolution;
    dev->settle_duration_ms = (uint32_t)(VEML7700_ItMs(r->it) * 1.5f);
    dev->settle_start_ms = 0;

    return true;
}

static void VEML7700_EnterSettling(VEML7700_HandleTypeDef *dev)
{
    dev->range_state = VEML7700_RANGE_SETTLING;
    dev->range_consecutive = 0;
    dev->pending_adjust = VEML7700_ADJUST_NONE;
    dev->settle_start_ms = Platform_GetTickMs();
}

static void VEML7700_ExitSettling(VEML7700_HandleTypeDef *dev)
{
    dev->range_state = VEML7700_RANGE_STABLE;
    dev->range_consecutive = 0;
    dev->pending_adjust = VEML7700_ADJUST_NONE;
}

static bool VEML7700_SetMoreSensitive(VEML7700_HandleTypeDef *dev)
{
    if (dev->range_index >= VEML7700_RANGE_COUNT - 1U) return false;
    if (!VEML7700_ApplyIndex(dev, dev->range_index + 1U)) return false;
    VEML7700_EnterSettling(dev);
    return true;
}

static bool VEML7700_SetLessSensitive(VEML7700_HandleTypeDef *dev)
{
    if (dev->range_index == 0U) return false;
    if (!VEML7700_ApplyIndex(dev, dev->range_index - 1U)) return false;
    VEML7700_EnterSettling(dev);
    return true;
}

static void VEML7700_FinalizeAttempt(VEML7700_HandleTypeDef *dev, VEML7700_Sample *sample)
{
    dev->last_attempt = *sample;
    if (sample->valid)
        dev->last_valid = *sample;
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
    dev->persistence = VEML7700_PERS_1;

    if (I2cBus_Probe(bus, VEML7700_I2C_ADDR) != DRIVER_STATUS_OK) return false;
    if (!VEML7700_ApplyIndex(dev, 4U))
    {
        dev->counters.config_error++;
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

    uint8_t rx[2];
    if (I2cBus_ReadMem(dev->bus, VEML7700_I2C_ADDR, VEML7700_REG_ALS, rx, 2U) != DRIVER_STATUS_OK)
    {
        dev->counters.read_error++;
        return false;
    }

    uint16_t raw = (uint16_t)rx[1] << 8U | (uint16_t)rx[0];
    sample->als_raw = raw;

    /* --- Settling check first: no autorange decisions on stale data --- */
    if (dev->range_state == VEML7700_RANGE_SETTLING)
    {
        sample->settling = true;
        uint32_t elapsed = Platform_GetTickMs() - dev->settle_start_ms;
        if (elapsed < dev->settle_duration_ms)
        {
            dev->counters.read_success++;
            VEML7700_FinalizeAttempt(dev, sample);
            return true;
        }
        VEML7700_ExitSettling(dev);
    }

    /* --- Saturation --- */
    if (raw >= VEML7700_RANGE_SATURATION)
    {
        sample->saturated = true;
        if (VEML7700_SetLessSensitive(dev))
            sample->range_changed = true;
        dev->counters.read_success++;
        VEML7700_FinalizeAttempt(dev, sample);
        return true;
    }

    /* --- Hysteresis and range-change decision --- */
    {
        bool should_change = false;

        if (raw < VEML7700_RANGE_LOW)
        {
            if (dev->pending_adjust != VEML7700_ADJUST_MORE)
            {
                dev->pending_adjust = VEML7700_ADJUST_MORE;
                dev->range_consecutive = 1;
            }
            else
            {
                dev->range_consecutive++;
            }

            if (dev->range_consecutive >= VEML7700_RANGE_CONVERGE)
            {
                if (VEML7700_SetMoreSensitive(dev))
                {
                    sample->range_changed = true;
                    sample->settling = true;
                    sample->valid = false;
                    should_change = true;
                }
                else
                {
                    /* at max sensitivity — reset and compute lux below */
                    dev->range_consecutive = 0;
                    dev->pending_adjust = VEML7700_ADJUST_NONE;
                }
            }
        }
        else if (raw > VEML7700_RANGE_HIGH)
        {
            if (dev->pending_adjust != VEML7700_ADJUST_LESS)
            {
                dev->pending_adjust = VEML7700_ADJUST_LESS;
                dev->range_consecutive = 1;
            }
            else
            {
                dev->range_consecutive++;
            }

            if (dev->range_consecutive >= VEML7700_RANGE_CONVERGE)
            {
                if (VEML7700_SetLessSensitive(dev))
                {
                    sample->range_changed = true;
                    sample->settling = true;
                    sample->valid = false;
                    should_change = true;
                }
                else
                {
                    /* at min sensitivity — reset and compute lux below */
                    dev->range_consecutive = 0;
                    dev->pending_adjust = VEML7700_ADJUST_NONE;
                }
            }
        }
        else
        {
            dev->range_consecutive = 0;
            dev->pending_adjust = VEML7700_ADJUST_NONE;
        }

        if (should_change)
        {
            dev->counters.read_success++;
            VEML7700_FinalizeAttempt(dev, sample);
            return true;
        }
    }

    /* --- Compute valid lux (current config unchanged) --- */
    sample->lux = (float)raw * dev->resolution;
    sample->valid = true;

    dev->counters.read_success++;
    VEML7700_FinalizeAttempt(dev, sample);
    return true;
}

bool VEML7700_GetDiagnostics(const VEML7700_HandleTypeDef *dev, VEML7700_Sample *diag)
{
    if ((dev == NULL) || (diag == NULL)) return false;
    *diag = dev->last_attempt;
    return true;
}

bool VEML7700_IsInitialized(const VEML7700_HandleTypeDef *dev)
{
    if (dev == NULL) return false;
    return (dev->initialized != 0U);
}