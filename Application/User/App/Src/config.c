#include "config.h"

static RoomSensorConfig s_config;

void Config_LoadDefaults(void)
{
    s_config.light_period_ms   = 500U;
    s_config.display_period_ms = 500U;
    s_config.retry_period_ms   = 5000U;
    s_config.diag_period_ms    = 10000U;
}

const RoomSensorConfig *Config_Get(void)
{
    return &s_config;
}