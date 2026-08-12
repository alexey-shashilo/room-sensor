#include "device_identity.h"
#include "storage.h"
#include "platform_unique_id.h"
#include <string.h>
#include <stdio.h>

#define IDENTITY_NAMESPACE "ClimateHub.RoomSensor.Identity.v2"

typedef struct
{
    uint32_t schema_version;
    uint8_t  device_uuid[DEVICE_UUID_SIZE];
    uint32_t hardware_revision;
} __attribute__((packed)) IdentityStorageV1;

_Static_assert(sizeof(IdentityStorageV1) == 24, "IdentityStorageV1 size mismatch");

static StorageReadStatus s_load_status = STORAGE_READ_NOT_FOUND;
/* Current persistence status of the identity record. Updated by the last
   DeviceIdentity_Load() and every DeviceIdentity_Save() so it is never stale:
   a successful durable save -> OK; NOT_FOUND/blank -> NOT_FOUND; corrupt or
   failed write -> CORRUPT/IO_ERROR. Historical load status remains available
   via DeviceIdentity_GetLoadStatus(). */
static StorageReadStatus s_persistence_status = STORAGE_READ_NOT_FOUND;
/* Redundancy (A/B mirror) health of the identity record, separate from read
   status. A readable VALID+IO record reads OK yet reports DEGRADED_IO. Kept
   current on every DeviceIdentity_Load / DeviceIdentity_Save. */
static StorageHealth s_storage_health = STORAGE_HEALTH_HEALTHY;
/* Result of the LAST identity write attempt — a separate fact from current
   readability and from A/B health. A failed write does NOT mean the existing
   VALID record was lost or became CORRUPT. */
static StorageWriteStatus s_last_write_status = STORAGE_WRITE_OK;

static void DeviceIdentity_RefreshHealth(void);
static void DeviceIdentity_RefreshReadState(void);

static void DeriveUuid(uint8_t uuid[DEVICE_UUID_SIZE])
{
    uint8_t uid[PLATFORM_UNIQUE_ID_SIZE];

    if (!Platform_GetUniqueId(uid, sizeof(uid)))
    {
        memset(uuid, 0, DEVICE_UUID_SIZE);
        return;
    }

    memset(uuid, 0, DEVICE_UUID_SIZE);

    size_t ns_len = strlen(IDENTITY_NAMESPACE);
    for (size_t i = 0; i < ns_len; i++)
    {
        unsigned shift = (unsigned)((i * 7U) & 15U);
        uuid[shift] ^= (uint8_t)IDENTITY_NAMESPACE[i];
    }

    for (size_t i = 0; i < PLATFORM_UNIQUE_ID_SIZE; i++)
    {
        unsigned shift = (unsigned)((i * 5U) & 15U);
        uuid[shift] ^= uid[i];
        uuid[(shift + 3U) & 15U] ^= (uid[i] << 2U) | (uid[i] >> 6U);
        uuid[(shift + 11U) & 15U] ^= uid[i] ^ 0xAAU;
    }

    uuid[6] = (uuid[6] & 0x0FU) | 0x80U;
    uuid[8] = (uuid[8] & 0x3FU) | 0x80U;
}

bool DeviceIdentity_Validate(const DeviceIdentity *id)
{
    if (id == NULL) return false;

    uint8_t zero[DEVICE_UUID_SIZE] = {0};
    if (memcmp(id->device_uuid, zero, DEVICE_UUID_SIZE) == 0) return false;

    uint8_t ff[DEVICE_UUID_SIZE];
    memset(ff, 0xFF, DEVICE_UUID_SIZE);
    if (memcmp(id->device_uuid, ff, DEVICE_UUID_SIZE) == 0) return false;

    if ((id->device_uuid[6] & 0xF0U) != 0x80U) return false;
    if ((id->device_uuid[8] & 0xC0U) != 0x80U) return false;
    if (id->hardware_revision == 0) return false;

    return true;
}

bool DeviceIdentity_Derive(DeviceIdentity *id)
{
    if (id == NULL) return false;

    memset(id, 0, sizeof(*id));
    DeriveUuid(id->device_uuid);
    id->hardware_revision = ROOM_SENSOR_HW_REVISION;

    return DeviceIdentity_Validate(id);
}

static bool Identity_FromStorage(DeviceIdentity *runtime, const IdentityStorageV1 *stored)
{
    if ((runtime == NULL) || (stored == NULL)) return false;
    if (stored->schema_version != IDENTITY_SCHEMA_VERSION) return false;

    memcpy(runtime->device_uuid, stored->device_uuid, DEVICE_UUID_SIZE);
    runtime->hardware_revision = stored->hardware_revision;

    return DeviceIdentity_Validate(runtime);
}

bool DeviceIdentity_Load(DeviceIdentity *id)
{
    if (id == NULL) return false;

    StoragePayload payload;
    StorageReadStatus rs = Storage_Read(RECORD_TYPE_IDENTITY, &payload);
    s_load_status = rs;
    s_persistence_status = rs;
    DeviceIdentity_RefreshHealth();

    if (rs != STORAGE_READ_OK)
    {
        /* NOT_FOUND / CORRUPT / IO_ERROR all mean the record is not usable;
           the caller derives a runtime identity and reports degradation via
           DeviceIdentity_GetLoadStatus(). No failure is treated as a fresh
           first boot unless it was genuinely NOT_FOUND. */
        return false;
    }

    if (payload.size != sizeof(IdentityStorageV1))
    {
        s_load_status = STORAGE_READ_CORRUPT;
        s_persistence_status = STORAGE_READ_CORRUPT;
        return false;
    }

    IdentityStorageV1 stored;
    memcpy(&stored, payload.data, sizeof(stored));

    if (!Identity_FromStorage(id, &stored))
    {
        s_load_status = STORAGE_READ_CORRUPT;
        s_persistence_status = STORAGE_READ_CORRUPT;
        return false;
    }
    return true;
}

StorageReadStatus DeviceIdentity_GetLoadStatus(void)
{
    return s_load_status;
}

StorageReadStatus DeviceIdentity_GetPersistenceStatus(void)
{
    return s_persistence_status;
}

StorageHealth DeviceIdentity_GetStorageHealth(void)
{
    return s_storage_health;
}

/* Refresh redundancy health from the actual Storage_GetHealth snapshot without
   mutating the runtime identity. Called after every storage-bearing op. */
static void DeviceIdentity_RefreshHealth(void)
{
    if (Storage_IsInitialized())
        s_storage_health = Storage_GetHealth(RECORD_TYPE_IDENTITY);
}

/* Non-destructively re-derive current readable identity persistence state (and
   A/B health) from actual Flash. Does NOT mutate the runtime identity. */
static void DeviceIdentity_RefreshReadState(void)
{
    if (!Storage_IsInitialized())
    {
        s_persistence_status = STORAGE_READ_IO_ERROR;
        return;
    }
    StoragePayload payload;
    s_persistence_status = Storage_Read(RECORD_TYPE_IDENTITY, &payload);
    DeviceIdentity_RefreshHealth();
}

StorageWriteStatus DeviceIdentity_GetLastWriteStatus(void)
{
    return s_last_write_status;
}

StorageRepairStatus DeviceIdentity_EnsureRedundancy(void)
{
    if (!Storage_IsInitialized())
    {
        s_persistence_status = STORAGE_READ_IO_ERROR;
        s_storage_health = STORAGE_HEALTH_IO_ERROR;
        return STORAGE_REPAIR_REFUSED;
    }

    StorageRepairStatus repair = Storage_EnsureRedundancy(RECORD_TYPE_IDENTITY);

    /* Non-destructively re-derive the CURRENT readable persistence state and
       mirror health from actual Flash, so the module's cached state reflects
       the repair outcome in every case (DONE, NOT_NEEDED, NOT_FOUND, REFUSED).
       This keeps DeviceIdentity_GetPersistenceStatus() /
       DeviceIdentity_GetStorageHealth() in step with the physical mirrors after
       a direct Storage repair. */
    DeviceIdentity_RefreshReadState();
    return repair;
}

StorageReadStatus DeviceIdentity_SelfCheck(void)
{
    if (!Storage_IsInitialized())
        return STORAGE_READ_IO_ERROR;

    /* Non-mutating diagnostic inspection of the persisted identity record.
       Reads and validates into a LOCAL candidate without touching the global
       runtime identity, load status, or persistence status. */
    StoragePayload payload;
    StorageReadStatus rs = Storage_Read(RECORD_TYPE_IDENTITY, &payload);
    if (rs == STORAGE_READ_NOT_FOUND) return STORAGE_READ_NOT_FOUND;
    if (rs == STORAGE_READ_CORRUPT)   return STORAGE_READ_CORRUPT;
    if (rs == STORAGE_READ_IO_ERROR)  return STORAGE_READ_IO_ERROR;
    if (rs != STORAGE_READ_OK)        return rs;

    if (payload.size != sizeof(IdentityStorageV1))
        return STORAGE_READ_CORRUPT;

    IdentityStorageV1 stored;
    memcpy(&stored, payload.data, sizeof(stored));

    DeviceIdentity local;
    if (!Identity_FromStorage(&local, &stored))
        return STORAGE_READ_CORRUPT;

    return STORAGE_READ_OK;
}

bool DeviceIdentity_Save(const DeviceIdentity *id)
{
    if (id == NULL) return false;
    if (!DeviceIdentity_Validate(id)) return false;

    IdentityStorageV1 stored;
    stored.schema_version = IDENTITY_SCHEMA_VERSION;
    memcpy(stored.device_uuid, id->device_uuid, DEVICE_UUID_SIZE);
    stored.hardware_revision = id->hardware_revision;

    StorageWriteStatus ws = Storage_WriteEx(RECORD_TYPE_IDENTITY,
                                            (const uint8_t *)&stored, sizeof(stored));
    /* Record the EXACT write result, observable via
       DeviceIdentity_GetLastWriteStatus(). */
    s_last_write_status = ws;

    if (ws == STORAGE_WRITE_OK)
        s_persistence_status = STORAGE_READ_OK;
    else
        /* A failed write does NOT mean the existing VALID record is corrupt or
           lost. Non-destructively refresh the ACTUAL readable state. */
        DeviceIdentity_RefreshReadState();

    DeviceIdentity_RefreshHealth();
    return (ws == STORAGE_WRITE_OK);
}

void DeviceIdentity_GetShortId(const DeviceIdentity *id, char *out, size_t max_len)
{
    if ((id == NULL) || (out == NULL) || (max_len == 0)) return;

    snprintf(out, max_len, "%02X%02X%02X%02X%02X%02X",
             (unsigned)id->device_uuid[0],
             (unsigned)id->device_uuid[1],
             (unsigned)id->device_uuid[2],
             (unsigned)id->device_uuid[3],
             (unsigned)id->device_uuid[4],
             (unsigned)id->device_uuid[5]);
}