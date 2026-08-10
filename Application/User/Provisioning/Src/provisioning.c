#include "provisioning.h"
#include "provisioning_storage.h"
#include "storage.h"
#include <string.h>
#include <ctype.h>

static uint32_t s_revision = 0;

bool EntityId_IsZero(const EntityId *id)
{
    if (id == NULL) return true;
    uint8_t zero[ENTITY_ID_SIZE] = {0};
    return memcmp(id->bytes, zero, ENTITY_ID_SIZE) == 0;
}

static int HexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool EntityId_Parse(EntityId *out, const char *hex, size_t len)
{
    if (out == NULL || hex == NULL) return false;
    if (len != 32) return false;

    for (size_t i = 0; i < 16; i++)
    {
        int hi = HexVal(hex[i * 2]);
        int lo = HexVal(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out->bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

void EntityId_Format(const EntityId *id, char *out, size_t max_len)
{
    if (id == NULL || out == NULL) return;
    if (max_len < 33) return;
    const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < 16; i++)
    {
        out[i * 2] = hex[(id->bytes[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[id->bytes[i] & 0x0F];
    }
    out[32] = '\0';
}

static bool ValidId(const EntityId *id)
{
    if (id == NULL) return false;
    if (EntityId_IsZero(id)) return false;

    /* reject all-FF */
    uint8_t ff[ENTITY_ID_SIZE];
    memset(ff, 0xFF, ENTITY_ID_SIZE);
    return memcmp(id->bytes, ff, ENTITY_ID_SIZE) != 0;
}

bool Provisioning_ValidateRegistration(const DeviceRegistration *reg)
{
    if (reg == NULL) return false;
    if (!reg->registered) return true;  /* unregistered is valid */
    if (!ValidId(&reg->installation_id)) return false;
    return true;
}

bool Provisioning_IsRegistered(const DeviceRegistration *reg)
{
    if (reg == NULL) return false;
    return reg->registered && ValidId(&reg->installation_id);
}

bool Provisioning_IsOperational(const DeviceRegistration *reg)
{
    if (reg == NULL) return false;
    return reg->registered &&
           ValidId(&reg->installation_id) &&
           ValidId(&reg->building_id) &&
           ValidId(&reg->room_id);
}

bool Provisioning_Load(DeviceRegistration *reg)
{
    if (reg == NULL) return false;

    memset(reg, 0, sizeof(*reg));

    StoragePayload payload;
    StorageReadStatus rs = Storage_Read(RECORD_TYPE_REGISTRATION, &payload);
    if (rs == STORAGE_READ_NOT_FOUND)
    {
        memset(reg, 0, sizeof(*reg));
        reg->registered = false;
        return true;  /* absent registration is valid (unprovisioned) */
    }

    if (rs != STORAGE_READ_OK)
    {
        /* CORRUPT or IO_ERROR — fail closed */
        memset(reg, 0, sizeof(*reg));
        return false;
    }

    if (payload.size != sizeof(RegistrationStorageV1))
        return false;

    RegistrationStorageV1 stored;
    memcpy(&stored, payload.data, sizeof(stored));

    if (stored.schema_version != REGISTRATION_SCHEMA_VERSION)
        return false;

    reg->registered = stored.registered;
    reg->installation_id = stored.installation_id;
    reg->building_id = stored.building_id;
    reg->room_id = stored.room_id;
    reg->installation_valid = ValidId(&stored.installation_id);
    reg->building_valid = ValidId(&stored.building_id);
    reg->room_valid = ValidId(&stored.room_id);
    s_revision = stored.revision;

    return true;
}

bool Provisioning_Save(const DeviceRegistration *reg)
{
    if (reg == NULL) return false;
    if (!Provisioning_ValidateRegistration(reg)) return false;

    RegistrationStorageV1 stored;
    memset(&stored, 0, sizeof(stored));
    stored.schema_version = REGISTRATION_SCHEMA_VERSION;
    stored.revision = s_revision + 1;
    stored.registered = reg->registered;
    stored.installation_id = reg->installation_id;
    stored.building_id = reg->building_id;
    stored.room_id = reg->room_id;

    bool ok = Storage_Write(RECORD_TYPE_REGISTRATION, (const uint8_t *)&stored, sizeof(stored));
    if (ok) s_revision = stored.revision;
    return ok;
}

bool Provisioning_Clear(void)
{
    RegistrationStorageV1 stored;
    memset(&stored, 0, sizeof(stored));
    stored.schema_version = REGISTRATION_SCHEMA_VERSION;
    stored.revision = s_revision + 1;
    stored.registered = false;
    bool ok = Storage_Write(RECORD_TYPE_REGISTRATION, (const uint8_t *)&stored, sizeof(stored));
    if (ok) s_revision = stored.revision;
    return ok;
}

void Provisioning_GetStatus(const DeviceRegistration *reg, ProvisioningStatus *status)
{
    if (reg == NULL || status == NULL) return;

    status->registered = reg->registered;
    status->installation_valid = reg->installation_valid;
    status->building_valid = reg->building_valid;
    status->room_valid = reg->room_valid;
    status->revision = s_revision;

    if (!reg->registered)
        status->state = PROVISIONING_DISCOVERABLE;
    else if (!reg->installation_valid)
        status->state = PROVISIONING_ERROR;
    else if (!reg->building_valid || !reg->room_valid)
        status->state = PROVISIONING_CONFIGURATION_PENDING;
    else
        status->state = PROVISIONING_OPERATIONAL;
}

bool Provisioning_Init(void)
{
    return true;
}