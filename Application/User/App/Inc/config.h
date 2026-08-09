#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    uint32_t version;

    uint32_t light_period_ms;
    uint32_t display_period_ms;
    uint32_t diagnostics_period_ms;
    uint32_t retry_period_ms;
    uint32_t telemetry_period_ms;

    float light_calibration_factor;
} RoomSensorConfig;

void                 Config_LoadDefaults(void);
bool                 Config_Load(void);
bool                 Config_Save(void);
bool                 Config_ResetToDefaults(void);
const RoomSensorConfig *Config_Get(void);
bool                 Config_Validate(const RoomSensorConfig *config);

#endif