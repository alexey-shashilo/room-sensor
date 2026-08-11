#include "provisioning.h"
#include "provisioning_storage.h"
#include "storage.h"
#include <string.h>

static DeviceRegistration  s_current;
static ProvisioningStatus  s_status;
static StorageReadStatus   s_storage_status = STORAGE_READ_NOT_FOUND;
static uint32_t            s_revision = 0;

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
    if (len != 2U * ENTITY_ID_SIZE) return false;

    for (size_t i = 0; i < ENTITY_ID_SIZE; i++)
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
    if (max_len < (2U * ENTITY_ID_SIZE + 1U)) return;
    const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < ENTITY_ID_SIZE; i++)
    {
        out[i * 2] = hex[(id->bytes[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[id->bytes[i] & 0x0F];
    }
    out[2U * ENTITY_ID_SIZE] = '\0';
}

bool Provisioning_ValidEntityId(const EntityId *id)
{
    if (id == NULL) return false;
    if (EntityId_IsZero(id)) return false;

    uint8_t ff[ENTITY_ID_SIZE];
    memset(ff, 0xFF, ENTITY_ID_SIZE);
    return memcmp(id->bytes, ff, ENTITY_ID_SIZE) != 0;
}

/* Strict canonical validation of a candidate DeviceRegistration.
   Validity flags must exactly match the actual IDs per the canonical rules:
   - unregistered: no flags set, all IDs zero.
   - registered:   installation_valid && valid installation_id.
   - building_valid: requires registered + installation_valid + valid building.
   - room_valid:    requires building_valid + valid room id.
   - building_valid == false implies room_valid == false.
   - a level's ID must be present iff its flag is set. */
static bool ValidateFlags(const DeviceRegistration *in)
{
    if (in == NULL) return false;

    bool has_inst = Provisioning_ValidEntityId(&in->installation_id);
    bool has_bld  = Provisioning_ValidEntityId(&in->building_id);
    bool has_room = Provisioning_ValidEntityId(&in->room_id);

    if (!in->registered)
    {
        if (in->installation_valid || in->building_valid || in->room_valid)
            return false;
        return !(has_inst || has_bld || has_room);
    }

    if (!in->installation_valid) return false;
    if (!has_inst) return false;

    if (in->building_valid)
    {
        if (!has_bld) return false;
        if (in->room_valid)
            return has_room;
        return !has_room;  /* building only: room id must be zero */
    }

    /* building_valid == false: room_valid must be false and no ids. */
    if (in->room_valid) return false;
    if (has_bld || has_room) return false;
    return true;
}

/* Derive a canonical runtime registration from raw stored fields, taking the
   validity flags FROM the actual IDs (never trusting persisted flags). */
static void DeriveCanonical(bool registered,
                            const EntityId *inst,
                            const EntityId *bld,
                            const EntityId *room,
                            DeviceRegistration *out)
{
    memset(out, 0, sizeof(*out));
    out->registered = registered;
    if (!registered) return;

    out->installation_id = *inst;
    out->installation_valid = Provisioning_ValidEntityId(inst);
    if (Provisioning_ValidEntityId(bld))
    {
        out->building_id = *bld;
        out->building_valid = true;
        if (Provisioning_ValidEntityId(room))
        {
            out->room_id = *room;
            out->room_valid = true;
        }
    }
}

bool Provisioning_ValidateRegistration(const DeviceRegistration *reg)
{
    return ValidateFlags(reg);
}

bool Provisioning_IsRegistered(const DeviceRegistration *reg)
{
    if (reg == NULL) return false;
    return reg->registered && Provisioning_ValidEntityId(&reg->installation_id);
}

bool Provisioning_IsOperational(const DeviceRegistration *reg)
{
    if (reg == NULL) return false;
    return reg->registered &&
           Provisioning_ValidEntityId(&reg->installation_id) &&
           Provisioning_ValidEntityId(&reg->building_id) &&
           Provisioning_ValidEntityId(&reg->room_id);
}

static bool Registration_SaveRaw(const DeviceRegistration *reg, bool force)
{
    /* Fail closed unless storage state is known OR this is an explicit
       recovery (force). If storage is corrupt/IO, ordinary mutations are
       prohibited until storage is recovered. */
    if (!force &&
        s_storage_status != STORAGE_READ_OK &&
        s_storage_status != STORAGE_READ_NOT_FOUND)
        return false;

    if (!ValidateFlags(reg)) return false;

    RegistrationStorageV1 stored;
    memset(&stored, 0, sizeof(stored));
    stored.schema_version = REGISTRATION_SCHEMA_VERSION;
    stored.revision = s_revision + 1;
    stored.registered = reg->registered;
    stored.installation_id = reg->installation_id;
    stored.building_id = reg->building_id;
    stored.room_id = reg->room_id;

    if (!Storage_Write(RECORD_TYPE_REGISTRATION, (const uint8_t *)&stored, sizeof(stored)))
        return false;

    /* Commit runtime ONLY on successful persistence. */
    s_revision = stored.revision;
    s_storage_status = STORAGE_READ_OK;
    s_current = *reg;
    Provisioning_GetStatus(&s_current, &s_status);
    return true;
}

bool Provisioning_Save(const DeviceRegistration *reg)
{
    if (reg == NULL) return false;
    return Registration_SaveRaw(reg, false);
}

bool Provisioning_Load(DeviceRegistration *reg)
{
    if (reg == NULL) return false;
    *reg = s_current;
    return (s_storage_status == STORAGE_READ_OK ||
            s_storage_status == STORAGE_READ_NOT_FOUND);
}

const ProvisioningRuntime *Provisioning_GetRuntime(void)
{
    static ProvisioningRuntime rt;
    rt.current = s_current;
    rt.status = s_status;
    rt.storage_status = s_storage_status;
    return &rt;
}

bool Provisioning_Clear(void)
{
    DeviceRegistration blank;
    memset(&blank, 0, sizeof(blank));
    blank.registered = false;

    /* Explicit recovery path: allow clearing even if storage is corrupt. */
    return Registration_SaveRaw(&blank, true);
}

bool Provisioning_Init(void)
{
    s_revision = 0;
    memset(&s_current, 0, sizeof(s_current));
    memset(&s_status, 0, sizeof(s_status));
    s_storage_status = STORAGE_READ_NOT_FOUND;

    StoragePayload payload;
    StorageReadStatus rs = Storage_Read(RECORD_TYPE_REGISTRATION, &payload);

    if (rs == STORAGE_READ_NOT_FOUND)
    {
        /* absent registration is valid (unprovisioned) */
        memset(&s_current, 0, sizeof(s_current));
        s_current.registered = false;
        s_storage_status = STORAGE_READ_NOT_FOUND;
        s_revision = 0;
        Provisioning_GetStatus(&s_current, &s_status);
        return true;
    }

    if (rs != STORAGE_READ_OK)
    {
        /* CORRUPT or IO_ERROR — fail closed. Runtime stays unregistered,
           s_storage_status records the fault so mutations are prohibited. */
        memset(&s_current, 0, sizeof(s_current));
        s_storage_status = rs;
        s_revision = 0;
        s_status.state = PROVISIONING_ERROR;
        return false;
    }

    if (payload.size != sizeof(RegistrationStorageV1))
    {
        s_storage_status = STORAGE_READ_CORRUPT;
        s_status.state = PROVISIONING_ERROR;
        return false;
    }

    RegistrationStorageV1 stored;
    memcpy(&stored, payload.data, sizeof(stored));

    if (stored.schema_version != REGISTRATION_SCHEMA_VERSION)
    {
        s_storage_status = STORAGE_READ_CORRUPT;
        s_status.state = PROVISIONING_ERROR;
        return false;
    }

    DeriveCanonical(stored.registered,
                    &stored.installation_id,
                    &stored.building_id,
                    &stored.room_id,
                    &s_current);

    /* A persisted "registered" record requires a valid installation ID;
       otherwise the record is contradictory / corrupt. */
    if (s_current.registered && !s_current.installation_valid)
    {
        memset(&s_current, 0, sizeof(s_current));
        s_storage_status = STORAGE_READ_CORRUPT;
        s_status.state = PROVISIONING_ERROR;
        return false;
    }

    s_revision = stored.revision;
    s_storage_status = STORAGE_READ_OK;
    Provisioning_GetStatus(&s_current, &s_status);
    return true;
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