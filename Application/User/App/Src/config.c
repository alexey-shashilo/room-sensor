#include "config.h"
#include "storage.h"
#include <string.h>
#include <math.h>

#define Q16_SCALE 65536.0f

static RoomSensorConfig s_config;
static StorageReadStatus s_storage_status = STORAGE_READ_NOT_FOUND;
/* Redundancy (A/B mirror) health of the config record, kept current on every
   storage operation. Read status and health are separate: a readable VALID+IO
   record reads OK yet reports DEGRADED_IO. */
static StorageHealth s_storage_health = STORAGE_HEALTH_HEALTHY;
/* Result of the LAST write attempt (a separate fact from current readability
   and from redundancy health). A failed write does NOT imply persisted data is
   lost or corrupt. */
static StorageWriteStatus s_last_write_status = STORAGE_WRITE_OK;

static void Config_RefreshHealth(void);
static void Config_RefreshReadState(void);

static float Q16ToFloat(uint32_t q16)
{
    return (float)((int32_t)q16) / Q16_SCALE;
}

static uint32_t FloatToQ16(float v)
{
    float scaled = v * Q16_SCALE;
    if (scaled > 2147483647.0f) return 0x7FFFFFFFU;
    if (scaled < -2147483648.0f) return 0x80000000U;
    return (uint32_t)((int32_t)(scaled));
}

static bool IsFinite(float v)
{
    return (v == v) && (v != 1.0f / 0.0f) && (v != -1.0f / 0.0f);
}

void Config_LoadDefaults(void)
{
    memset(&s_config, 0, sizeof(s_config));

    s_config.storage.version              = CONFIG_SCHEMA_VERSION;
    s_config.storage.light_period_ms      = 500U;
    s_config.storage.display_period_ms    = 500U;
    s_config.storage.diagnostics_period_ms = 10000U;
    s_config.storage.retry_period_ms      = 5000U;
    s_config.storage.telemetry_period_ms  = 5000U;
    s_config.storage.light_calibration_q16 = FloatToQ16(1.0f);

    s_config.runtime.light_calibration_factor = 1.0f;
}

bool Config_Validate(const ConfigStorageV1 *storage)
{
    if (storage == NULL) return false;
    if (storage->version != CONFIG_SCHEMA_VERSION) return false;
    if (storage->light_period_ms < 50U || storage->light_period_ms > 60000U) return false;
    if (storage->display_period_ms < 50U || storage->display_period_ms > 60000U) return false;
    if (storage->diagnostics_period_ms < 1000U || storage->diagnostics_period_ms > 3600000U) return false;
    if (storage->retry_period_ms < 1000U || storage->retry_period_ms > 600000U) return false;
    if (storage->telemetry_period_ms < 1000U || storage->telemetry_period_ms > 3600000U) return false;

    float cal = Q16ToFloat(storage->light_calibration_q16);
    if (!IsFinite(cal)) return false;
    if (cal < 0.01f || cal > 100.0f) return false;

    return true;
}

bool Config_Load(void)
{
    StoragePayload payload;
    StorageReadStatus rs = Storage_Read(RECORD_TYPE_CONFIG, &payload);
    s_storage_status = rs;
    Config_RefreshHealth();

    if (rs == STORAGE_READ_NOT_FOUND)
        return false;  /* fresh/never-written: safe defaults are used */
    if (rs == STORAGE_READ_CORRUPT)
        return false;  /* corrupt record: safe defaults used, storage degraded */
    if (rs == STORAGE_READ_IO_ERROR)
        return false;  /* Flash failure: safe defaults used, report failure */
    if (rs != STORAGE_READ_OK)
        return false;

    ConfigStorageV1 candidate;
    if (payload.size != sizeof(candidate))
    {
        s_storage_status = STORAGE_READ_CORRUPT;
        return false;
    }

    memcpy(&candidate, payload.data, sizeof(candidate));

    if (!Config_Validate(&candidate))
    {
        s_storage_status = STORAGE_READ_CORRUPT;
        return false;
    }

    /* Ignore persisted validity flags; the storage CRC guarantees the full
       record (including any flags) is intact as-written. */
    s_config.storage = candidate;
    s_config.runtime.light_calibration_factor = Q16ToFloat(candidate.light_calibration_q16);
    s_storage_status = STORAGE_READ_OK;
    return true;
}

StorageReadStatus Config_GetStorageStatus(void)
{
    return s_storage_status;
}

StorageHealth Config_GetStorageHealth(void)
{
    return s_storage_health;
}

/* Refresh redundancy health from the actual Storage_GetHealth snapshot without
   mutating the runtime config value. Called after every storage-bearing
   operation (load/save/apply/reset/recovery). */
static void Config_RefreshHealth(void)
{
    if (Storage_IsInitialized())
        s_storage_health = Storage_GetHealth(RECORD_TYPE_CONFIG);
}

/* Refresh the CURRENT readable persistence state (and redundancy health) with a
   non-destructive Storage_Read snapshot. It does NOT mutate the runtime config
   value (s_config). Crucially, a failed write to the inactive mirror does NOT
   destroy the old VALID record, so the read status must reflect the ACTUAL
   readable persistent data — never blindly forced to CORRUPT because the last
   write failed. */
static void Config_RefreshReadState(void)
{
    if (!Storage_IsInitialized())
    {
        s_storage_status = STORAGE_READ_IO_ERROR;
        return;
    }
    StoragePayload payload;
    StorageReadStatus rs = Storage_Read(RECORD_TYPE_CONFIG, &payload);
    s_storage_status = rs;
    Config_RefreshHealth();
}

StorageWriteStatus Config_GetLastWriteStatus(void)
{
    return s_last_write_status;
}

StorageRepairStatus Config_EnsureRedundancy(void)
{
    if (!Storage_IsInitialized())
    {
        s_storage_status = STORAGE_READ_IO_ERROR;
        s_storage_health = STORAGE_HEALTH_IO_ERROR;
        return STORAGE_REPAIR_REFUSED;
    }

    StorageRepairStatus repair = Storage_EnsureRedundancy(RECORD_TYPE_CONFIG);

    /* Non-destructively re-derive the CURRENT readable state and mirror health
       from actual Flash, so the module's cached state reflects the repair
       outcome in every case (DONE, NOT_NEEDED, NOT_FOUND, REFUSED). This is
       what keeps Config_GetStorageStatus() / Config_GetStorageHealth() in step
       with the physical mirrors after a direct Storage repair. */
    Config_RefreshReadState();
    return repair;
}

StorageReadStatus Config_SelfCheck(void)
{
    if (!Storage_IsInitialized())
        return STORAGE_READ_IO_ERROR;

    /* Non-mutating diagnostic inspection. Reads the persisted record into a
       local candidate and validates it. It does NOT touch the global runtime
       config (s_config) nor the runtime persistence status (s_storage_status),
       so a diagnostic command never changes live configuration. */

    StoragePayload payload;
    StorageReadStatus rs = Storage_Read(RECORD_TYPE_CONFIG, &payload);
    if (rs == STORAGE_READ_NOT_FOUND) return STORAGE_READ_NOT_FOUND;
    if (rs == STORAGE_READ_CORRUPT)   return STORAGE_READ_CORRUPT;
    if (rs == STORAGE_READ_IO_ERROR)  return STORAGE_READ_IO_ERROR;
    if (rs != STORAGE_READ_OK)        return rs;

    if (payload.size != sizeof(ConfigStorageV1))
        return STORAGE_READ_CORRUPT;

    ConfigStorageV1 candidate;
    memcpy(&candidate, payload.data, sizeof(candidate));

    if (!Config_Validate(&candidate))
        return STORAGE_READ_CORRUPT;

    return STORAGE_READ_OK;
}

bool Config_SaveCandidate(const RoomSensorConfig *candidate)
{
    if (candidate == NULL) return false;
    if (!Config_Validate(&candidate->storage)) return false;

    ConfigStorageV1 storage = candidate->storage;
    storage.version = CONFIG_SCHEMA_VERSION;
    storage.light_calibration_q16 = FloatToQ16(candidate->runtime.light_calibration_factor);

    StorageWriteStatus ws = Storage_WriteEx(RECORD_TYPE_CONFIG,
                                            (const uint8_t *)&storage, sizeof(storage));
    /* Record the EXACT write result; it stays observable via
       Config_GetLastWriteStatus() (OK / INVALID_ARGUMENT / UNSAFE_STATE /
       IO_ERROR / VERIFY_FAILED). */
    s_last_write_status = ws;

    if (ws == STORAGE_WRITE_OK)
    {
        /* Successful durable write: current readable state is OK. The runtime
           config value is committed by the CALLER (Config_Save / Config_ApplyPersistent),
           not here. */
        s_storage_status = STORAGE_READ_OK;
    }
    else
    {
        /* A FAILED write does NOT imply the persisted data was lost or that the
           old VALID record became CORRUPT. Non-destructively re-derive the ACTUAL
           readable state of whatever still exists on Flash. */
        Config_RefreshReadState();
    }

    /* StorageHealth always reflects the CURRENT A/B mirror, whether the write
       succeeded, failed, or was refused. */
    Config_RefreshHealth();
    return (ws == STORAGE_WRITE_OK);
}

bool Config_Save(void)
{
    return Config_SaveCandidate(&s_config);
}

ConfigApplyStatus Config_ApplyPersistent(const RoomSensorConfig *candidate)
{
    if (candidate == NULL) return CONFIG_APPLY_INVALID;
    if (!Config_Validate(&candidate->storage)) return CONFIG_APPLY_INVALID;

    /* Persist candidate first; only commit runtime on success */
    if (!Config_SaveCandidate(candidate))
        return CONFIG_APPLY_PERSIST_FAILED;

    s_config = *candidate;
    return CONFIG_APPLY_OK;
}

bool Config_ResetToDefaults(void)
{
    RoomSensorConfig defaults;
    memset(&defaults, 0, sizeof(defaults));

    defaults.storage.version = CONFIG_SCHEMA_VERSION;
    defaults.storage.light_period_ms = 500U;
    defaults.storage.display_period_ms = 500U;
    defaults.storage.diagnostics_period_ms = 10000U;
    defaults.storage.retry_period_ms = 5000U;
    defaults.storage.telemetry_period_ms = 5000U;
    defaults.storage.light_calibration_q16 = FloatToQ16(1.0f);
    defaults.runtime.light_calibration_factor = 1.0f;

    if (!Config_Validate(&defaults.storage)) return false;

    if (!Config_SaveCandidate(&defaults))
        return false;

    s_config = defaults;
    return true;
}

const RoomSensorConfig *Config_Get(void)
{
    return &s_config;
}