#ifndef VEML7700_H
#define VEML7700_H

#include <stdbool.h>
#include <stdint.h>
#include "room_sensor_types.h"

#define VEML7700_I2C_ADDR       (0x10U << 1U)

#define VEML7700_REG_ALS_CONF   0x00U
#define VEML7700_REG_ALS        0x04U
#define VEML7700_REG_WH         0x01U

#define VEML7700_GAIN_BITS_POS  11U
#define VEML7700_IT_BITS_POS    6U
#define VEML7700_PERS_BITS_POS  4U

typedef enum
{
    VEML7700_GAIN_1   = 0U,
    VEML7700_GAIN_2   = 1U,
    VEML7700_GAIN_1_8 = 2U,
    VEML7700_GAIN_1_4 = 3U
} VEML7700_Gain;

typedef enum
{
    VEML7700_IT_25_MS  = 0xCU,
    VEML7700_IT_50_MS  = 0x8U,
    VEML7700_IT_100_MS = 0x0U,
    VEML7700_IT_200_MS = 0x1U,
    VEML7700_IT_400_MS = 0x2U,
    VEML7700_IT_800_MS = 0x3U
} VEML7700_IntegrationTime;

typedef enum
{
    VEML7700_PERS_1 = 0U,
    VEML7700_PERS_2 = 1U,
    VEML7700_PERS_4 = 2U,
    VEML7700_PERS_8 = 3U
} VEML7700_Persistence;

#define VEML7700_RANGE_CONVERGE_SAMPLES 3U
#define VEML7700_RANGE_LOW_THRESHOLD    80U
#define VEML7700_RANGE_HIGH_THRESHOLD   12000U
#define VEML7700_RANGE_SATURATION       64000U

typedef struct
{
    VEML7700_Gain            gain;
    VEML7700_IntegrationTime integration_time;

    uint16_t als_conf_value;
    float    resolution;            
    uint16_t max_raw;            
} VEML7700_RangeConfig;

typedef enum
{
    VEML7700_RANGE_STABLE = 0,
    VEML7700_RANGE_INCREASING,
    VEML7700_RANGE_DECREASING,
    VEML7700_RANGE_SETTLING
} VEML7700_RangeState;

typedef struct
{
    uint16_t als_raw;
    float lux;
    VEML7700_Gain gain;
    VEML7700_IntegrationTime integration_time;
    float resolution;
    bool valid;
    bool range_changed;
} VEML7700_Sample;

typedef struct
{
    void *i2c_bus;
    uint8_t initialized;

    VEML7700_Gain gain;
    VEML7700_IntegrationTime integration_time;
    VEML7700_Persistence persistence;

    uint16_t als_conf_value;
    float    resolution;

    VEML7700_RangeState range_state;
    uint8_t  range_consecutive;
    uint32_t range_settle_until_ms;
    uint32_t last_it_ms;

    VEML7700_Sample last_sample;

    DeviceCounters counters;
} VEML7700_HandleTypeDef;

bool VEML7700_Init(VEML7700_HandleTypeDef *dev, void *i2c_bus);
bool VEML7700_Probe(VEML7700_HandleTypeDef *dev, void *i2c_bus);
bool VEML7700_ReadWithAutoRange(VEML7700_HandleTypeDef *dev, VEML7700_Sample *sample);
bool VEML7700_GetDiagnostics(const VEML7700_HandleTypeDef *dev, VEML7700_Sample *diag);
bool VEML7700_IsInitialized(const VEML7700_HandleTypeDef *dev);

#endif