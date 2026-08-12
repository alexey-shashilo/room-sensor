#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "storage.h"

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

/* Current known persistence state. OK = a durable record is present (loaded
   as healthy OR last write succeeded), NOT_FOUND = fresh/blank (defaults used),
   CORRUPT = corrupt record (safe defaults used, storage degraded),
   IO_ERROR = Flash read/write failure. Updated by Config_Load and by every
   Config_Save / Config_Reset path so the result is never stale. */
StorageReadStatus Config_GetStorageStatus(void);

/* Redundancy (A/B mirror) health of the persisted config record, separate from
   read status. A readable VALID+IO record reads OK but reports DEGRADED_IO.
   Updated by Config_Load and every Config_Save / Config_Reset path. */
StorageHealth Config_GetStorageHealth(void);

/* Result of the LAST write attempt (OK / INVALID_ARGUMENT / UNSAFE_STATE /
   IO_ERROR / VERIFY_FAILED). This is a separate fact from current readability
   and from A/B redundancy health: a failed write does NOT mean persisted data
   was lost or corrupt. Runtime config is only updated on confirmed success. */
StorageWriteStatus Config_GetLastWriteStatus(void);

/* Non-mutating diagnostic inspection of the persisted config record. It reads
   and validates into a LOCAL candidate and returns the observed state without
   touching the global runtime config or the Config_GetStorageStatus() result.
   Intended for a side-effect-free self-test / diagnostic command.
   OK = persisted healthy, NOT_FOUND = blank/first-boot,
   CORRUPT = corrupt record, IO_ERROR = Flash failure. */
StorageReadStatus Config_SelfCheck(void);

/* Config-owned mirror establishment. This is the ONLY way App/boot may repair
   the config record's A/B mirrors: it calls Storage_EnsureRedundancy internally
   and then non-destructively refreshes BOTH the current readable status
   (Config_GetStorageStatus) and the redundancy health (Config_GetStorageHealth)
   from actual Flash, so the module's cached state can never disagree with the
   physical mirror after a repair. It mutates the persisted mirror if and only
   if there is a valid source and a degraded peer (never erases a valid source,
   never persists the runtime value). Returns the Storage repair classification
   (DONE / NOT_NEEDED / NOT_FOUND / REFUSED); health reflects the outcome in
   every case. */
StorageRepairStatus Config_EnsureRedundancy(void);

#endif
