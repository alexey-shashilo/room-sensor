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

    bool ok = Storage_Write(RECORD_TYPE_IDENTITY, (const uint8_t *)&stored, sizeof(stored));
    /* A successful durable save means the current persistence state is OK. */
    s_persistence_status = ok ? STORAGE_READ_OK : STORAGE_READ_IO_ERROR;
    return ok;
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