#include "device_identity.h"
#include "storage.h"
#include "platform_unique_id.h"
#include <string.h>
#include <stdio.h>

static void DeriveUuid(uint8_t uuid[16])
{
    uint8_t uid[PLATFORM_UNIQUE_ID_SIZE];
    if (!Platform_GetUniqueId(uid, sizeof(uid)))
    {
        memset(uuid, 0, sizeof(uid));
        return;
    }

    /* Deterministic hash: mix uid bytes into 128-bit result */
    /* Simple mixing: XOR-rotate each UID byte through the UUID */
    memset(uuid, 0, 16);
    for (size_t i = 0; i < PLATFORM_UNIQUE_ID_SIZE; i++)
    {
        unsigned shift = (unsigned)((i * 5U) & 15U);
        uuid[shift]     ^= uid[i];
        uuid[(shift + 1U) & 15U] ^= (uid[i] << 3U) | (uid[i] >> 5U);
        uuid[(shift + 7U) & 15U] ^= uid[i] ^ 0x55U;
    }

    /* UUID version 4: byte 6, top nibble = 0100 */
    uuid[6] = (uuid[6] & 0x0FU) | 0x40U;

    /* UUID variant RFC 4122: byte 8, top bits = 10 */
    uuid[8] = (uuid[8] & 0x3FU) | 0x80U;
}

bool DeviceIdentity_Validate(const DeviceIdentity *id)
{
    if (id == NULL) return false;

    /* UUID must not be all zero or all FF */
    uint8_t zero[16] = {0};
    if (memcmp(id->device_uuid, zero, 16) == 0) return false;

    uint8_t ff[16];
    memset(ff, 0xFF, 16);
    if (memcmp(id->device_uuid, ff, 16) == 0) return false;

    /* UUID version bits: byte 6 top nibble must be 0100 (version 4) */
    if ((id->device_uuid[6] & 0xF0U) != 0x40U) return false;

    /* UUID variant bits: byte 8 top bits must be 10xxxxxx (RFC 4122) */
    if ((id->device_uuid[8] & 0xC0U) != 0x80U) return false;

    if (id->hardware_revision == 0) return false;
    if (id->identity_schema_version == 0) return false;

    return true;
}

bool DeviceIdentity_Load(DeviceIdentity *id)
{
    if (id == NULL) return false;

    StoragePayload payload;
    if (!Storage_Read(RECORD_TYPE_IDENTITY, &payload))
        return false;

    if (payload.size != sizeof(DeviceIdentity))
        return false;

    DeviceIdentity candidate;
    memcpy(&candidate, payload.data, sizeof(candidate));

    if (!DeviceIdentity_Validate(&candidate))
        return false;

    *id = candidate;
    return true;
}

bool DeviceIdentity_Generate(DeviceIdentity *id)
{
    if (id == NULL) return false;

    memset(id, 0, sizeof(*id));
    DeriveUuid(id->device_uuid);
    id->hardware_revision = 1;
    id->identity_schema_version = IDENTITY_SCHEMA_VERSION;

    if (!DeviceIdentity_Validate(id))
        return false;

    return Storage_Write(RECORD_TYPE_IDENTITY, (const uint8_t *)id, sizeof(*id));
}

bool DeviceIdentity_Save(const DeviceIdentity *id)
{
    if (id == NULL) return false;
    if (!DeviceIdentity_Validate(id)) return false;
    return Storage_Write(RECORD_TYPE_IDENTITY, (const uint8_t *)id, sizeof(*id));
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