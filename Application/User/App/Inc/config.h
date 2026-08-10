#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#define CONFIG_SCHEMA_VERSION 1U

typedef struct
{
    uint32_t version;

    uint32_t light_period_ms;
    uint32_t display_period_ms;
    uint32_t diagnostics_period_ms;
    uint32_t retry_period_ms;
    uint32_t telemetry_period_ms;

    uint32_t light_calibration_q16;
} __attribute__((packed)) ConfigStorageV1;

_Static_assert(sizeof(ConfigStorageV1) == 28, "ConfigStorageV1 size mismatch");

typedef struct
{
    float light_calibration_factor;
} RoomSensorRuntimeConfig;

typedef struct
{
    ConfigStorageV1 storage;
    RoomSensorRuntimeConfig runtime;
} RoomSensorConfig;

typedef enum
{
    CONFIG_APPLY_OK = 0,
    CONFIG_APPLY_INVALID,
    CONFIG_APPLY_PERSIST_FAILED
} ConfigApplyStatus;

void               Config_LoadDefaults(void);
bool               Config_Load(void);
bool               Config_Save(void);
bool               Config_SaveCandidate(const RoomSensorConfig *candidate);
bool               Config_ResetToDefaults(void);
ConfigApplyStatus  Config_ApplyPersistent(const RoomSensorConfig *candidate);
const RoomSensorConfig *Config_Get(void);
bool               Config_Validate(const ConfigStorageV1 *storage);

#endif