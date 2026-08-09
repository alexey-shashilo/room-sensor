#include "config.h"
#include "storage.h"
#include <string.h>
#include <math.h>

#define CONFIG_VERSION 1U

static RoomSensorConfig s_config;

static bool IsFinite(float v)
{
    return (v == v) && (v != 1.0f / 0.0f) && (v != -1.0f / 0.0f);
}

void Config_LoadDefaults(void)
{
    s_config.version = CONFIG_VERSION;
    s_config.light_period_ms        = 500U;
    s_config.display_period_ms      = 500U;
    s_config.diagnostics_period_ms  = 10000U;
    s_config.retry_period_ms        = 5000U;
    s_config.telemetry_period_ms    = 5000U;
    s_config.light_calibration_factor = 1.0f;
}

bool Config_Validate(const RoomSensorConfig *config)
{
    if (config == NULL) return false;
    if (config->version == 0) return false;
    if (config->light_period_ms < 50U || config->light_period_ms > 60000U) return false;
    if (config->display_period_ms < 50U || config->display_period_ms > 60000U) return false;
    if (config->diagnostics_period_ms < 1000U || config->diagnostics_period_ms > 3600000U) return false;
    if (config->retry_period_ms < 1000U || config->retry_period_ms > 600000U) return false;
    if (config->telemetry_period_ms < 1000U || config->telemetry_period_ms > 3600000U) return false;

    float cal = config->light_calibration_factor;
    if (!IsFinite(cal)) return false;
    if (cal < 0.01f || cal > 100.0f) return false;

    return true;
}

bool Config_Load(void)
{
    StoragePayload payload;
    if (!Storage_Read(RECORD_TYPE_CONFIG, &payload))
        return false;

    RoomSensorConfig candidate;
    if (payload.size != sizeof(candidate))
        return false;

    memcpy(&candidate, payload.data, sizeof(candidate));

    if (Config_Validate(&candidate))
    {
        s_config = candidate;
        return true;
    }

    return false;
}

bool Config_Save(void)
{
    s_config.version = CONFIG_VERSION;
    return Storage_Write(RECORD_TYPE_CONFIG, (const uint8_t *)&s_config, sizeof(s_config));
}

bool Config_ResetToDefaults(void)
{
    Config_LoadDefaults();
    return Config_Save();
}

const RoomSensorConfig *Config_Get(void)
{
    return &s_config;
}