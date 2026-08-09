#ifndef VEML7700_H
#define VEML7700_H

#include <stdbool.h>
#include <stdint.h>
#include "room_sensor_types.h"
#include "i2c_bus.h"

#define VEML7700_I2C_ADDR       (0x10U << 1U)

#define VEML7700_REG_ALS_CONF   0x00U
#define VEML7700_REG_ALS        0x04U

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

#define VEML7700_RANGE_COUNT       10U
#define VEML7700_RANGE_CONVERGE    3U
#define VEML7700_RANGE_LOW         80U
#define VEML7700_RANGE_HIGH        12000U
#define VEML7700_RANGE_SATURATION  64000U

typedef enum
{
    VEML7700_RANGE_STABLE = 0,
    VEML7700_RANGE_SETTLING
} VEML7700_RangeState;

typedef enum
{
    VEML7700_ADJUST_NONE = 0,
    VEML7700_ADJUST_MORE,
    VEML7700_ADJUST_LESS
} VEML7700_AdjustDirection;

typedef struct
{
    uint16_t als_raw;
    float lux;
    VEML7700_Gain gain;
    VEML7700_IntegrationTime integration_time;
    float resolution;
    bool valid;
    bool range_changed;
    bool saturated;
    bool settling;
} VEML7700_Sample;

typedef struct
{
    uint32_t read_success;
    uint32_t read_error;
    uint32_t config_error;
} VEML7700_DriverStats;

typedef struct
{
    const I2cBus *bus;
    uint8_t initialized;

    VEML7700_Gain gain;
    VEML7700_IntegrationTime integration_time;
    VEML7700_Persistence persistence;

    uint16_t als_conf_value;
    float    resolution;

    uint8_t  range_index;
    VEML7700_RangeState range_state;
    VEML7700_AdjustDirection pending_adjust;
    uint8_t  range_consecutive;
    uint32_t settle_start_ms;
    uint32_t settle_duration_ms;

    VEML7700_Sample last_valid;
    VEML7700_Sample last_attempt;
    VEML7700_DriverStats counters;
} VEML7700_HandleTypeDef;

bool VEML7700_Probe(const I2cBus *bus);
bool VEML7700_Init(VEML7700_HandleTypeDef *dev, const I2cBus *bus);
bool VEML7700_ReadWithAutoRange(VEML7700_HandleTypeDef *dev, VEML7700_Sample *sample);
bool VEML7700_GetDiagnostics(const VEML7700_HandleTypeDef *dev, VEML7700_Sample *diag);
bool VEML7700_IsInitialized(const VEML7700_HandleTypeDef *dev);

#endif