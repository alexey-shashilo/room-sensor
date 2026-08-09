#include "device_identity.h"
#include "storage.h"
#include "platform_time.h"
#include <string.h>
#include <stdio.h>

static void DeriveUuid(uint8_t uuid[16])
{
#ifdef VEML7700_UNIT_TEST
    for (int i = 0; i < 16; i++) uuid[i] = (uint8_t)(i + 1);
#else
    uint32_t uid[3];
    uid[0] = *(const uint32_t *)0x1FFF7590;
    uid[1] = *(const uint32_t *)0x1FFF7594;
    uid[2] = *(const uint32_t *)0x1FFF7598;
    memcpy(uuid, uid, 12);
    uuid[12] = (uint8_t)(Platform_GetTickMs() & 0xFFU);
    uuid[13] = (uint8_t)((Platform_GetTickMs() >> 8U) & 0xFFU);
    uuid[14] = 0x00U;
    uuid[15] = 0x00U;
#endif

    uuid[7] = (uuid[7] & 0x0FU) | 0x40U;
    uuid[9] = (uuid[9] & 0x3FU) | 0x80U;
}

bool DeviceIdentity_Load(DeviceIdentity *id)
{
    if (id == NULL) return false;

    StoragePayload payload;
    if (!Storage_Read(RECORD_TYPE_IDENTITY, &payload))
        return false;

    if (payload.size != sizeof(DeviceIdentity))
        return false;

    memcpy(id, payload.data, sizeof(*id));
    return true;
}

bool DeviceIdentity_Generate(DeviceIdentity *id)
{
    if (id == NULL) return false;

    memset(id, 0, sizeof(*id));
    DeriveUuid(id->device_uuid);
    id->hardware_revision = 1;
    id->firmware_config_version = 1;

    return Storage_Write(RECORD_TYPE_IDENTITY, (const uint8_t *)id, sizeof(*id));
}

bool DeviceIdentity_Save(const DeviceIdentity *id)
{
    if (id == NULL) return false;
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