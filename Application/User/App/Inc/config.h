#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

typedef struct
{
    uint32_t light_period_ms;
    uint32_t display_period_ms;
    uint32_t retry_period_ms;
    uint32_t diag_period_ms;
} RoomSensorConfig;

void                 Config_LoadDefaults(void);
const RoomSensorConfig *Config_Get(void);

#endif