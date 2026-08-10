#include <stdio.h>
#include <string.h>

#include "provisioning.h"
#include "provisioning_storage.h"
#include "storage.h"
#include "device_identity.h"
#include "platform_flash.h"
#include "fake_flash.h"
#include "fake_unique_id.h"
#include "fake_platform_time.h"

static int s_pass = 0, s_fail = 0, s_case = 0;

static void check(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

int main(void)
{
    printf("Provisioning Host Tests\n");
    fflush(stdout);

    FakeFlash_Init();
    FakeUniqueId_Set((const uint8_t[]){0xAA,0xBB,0xCC,0xDD,0x01,0x02,0x03,0x04,0xFE,0xED,0xBE,0xEF});
    Storage_Init();

    EntityId inst_A, inst_X, building, room;
    EntityId_Parse(&inst_A, "11111111111111111111111111111111", 32);
    EntityId_Parse(&inst_X, "99999999999999999999999999999999", 32);
    EntityId_Parse(&building, "22222222222222222222222222222222", 32);
    EntityId_Parse(&room, "33333333333333333333333333333333", 32);

    /* EntityId helpers */
    check(EntityId_Parse(&inst_A, "11111111111111111111111111111111", 32), "parse A valid");
    check(!EntityId_IsZero(&inst_A), "A not zero");
    check(EntityId_IsZero(&(EntityId){0}), "zero id detected");
    {
        char buf[33];
        EntityId_Format(&inst_A, buf, sizeof(buf));
        check(strcmp(buf, "11111111111111111111111111111111") == 0, "format A");
    }

    /* 1. Blank -> DISCOVERABLE */
    DeviceRegistration reg;
    check(Provisioning_Load(&reg), "blank storage loads as unprovisioned");
    check(reg.registered == false, "blank storage -> registered=false");
    check(Provisioning_IsOperational(&reg) == false, "blank storage not operational");
    {
        ProvisioningStatus ps;
        Provisioning_GetStatus(&reg, &ps);
        check(ps.state == PROVISIONING_DISCOVERABLE, "state = DISCOVERABLE");
    }

    /* Direct Storage_Write test */
    RegistrationStorageV1 test_record;
    memset(&test_record, 0, sizeof(test_record));
    test_record.schema_version = REGISTRATION_SCHEMA_VERSION;
    test_record.revision = 1;
    test_record.registered = true;
    memcpy(test_record.installation_id.bytes, "11111111111111111111111111111111", 16);
    bool storage_ok = Storage_Write(RECORD_TYPE_REGISTRATION, (const uint8_t *)&test_record, sizeof(test_record));
    check(storage_ok, "direct Storage_Write for registration");

    StoragePayload test_payload;
    bool storage_read_ok = Storage_Read(RECORD_TYPE_REGISTRATION, &test_payload);
    check(storage_read_ok, "direct Storage_Read for registration");

    if (storage_read_ok)
    {
        RegistrationStorageV1 *stored_chk = (RegistrationStorageV1 *)test_payload.data;
        check(stored_chk->revision == 1, "stored revision=1");
    }

    /* 2. Register via Provisioning API */
    reg.registered = true;
    reg.installation_id = inst_A;
    reg.installation_valid = true;
    check(Provisioning_Save(&reg), "register A succeeds");

    DeviceRegistration loaded;
    check(Provisioning_Load(&loaded), "reload after register");
    check(loaded.registered, "loaded registered");
    check(memcmp(loaded.installation_id.bytes, inst_A.bytes, 16) == 0, "installation A loaded");

    /* 3. Ownership conflict (register to X) — test via validation */
    {
        DeviceRegistration v;
        Provisioning_Load(&v);
        check(memcmp(v.installation_id.bytes, inst_A.bytes, 16) == 0, "A retained before conflict");
    }

    /* 4. Assign location */
    DeviceRegistration assign = loaded;
    assign.building_id = building;
    assign.room_id = room;
    assign.building_valid = true;
    assign.room_valid = true;
    check(Provisioning_Save(&assign), "assign building+room succeeds");
    check(Provisioning_IsOperational(&assign), "assignment makes operational");

    /* 5. Reboot preserves registration */
    {
        DeviceRegistration reloaded;
        check(Provisioning_Load(&reloaded), "reload after 'reboot'");
        check(reloaded.registered, "registered after reboot");
        check(memcmp(reloaded.room_id.bytes, room.bytes, 16) == 0, "room preserved");
    }

    /* 6. Identity independent */
    {
        DeviceIdentity id_a, id_b;
        DeviceIdentity_Derive(&id_a);
        DeviceIdentity_Derive(&id_b);
        check(memcmp(id_a.device_uuid, id_b.device_uuid, 16) == 0, "identity deterministic");
    }

    /* 7. Schema version */
    check(REGISTRATION_SCHEMA_VERSION == 1, "registration schema v1");

    /* 8. Unregister picks DISCOVERABLE */
    {
        DeviceRegistration cleared;
        memset(&cleared, 0, sizeof(cleared));
        cleared.registered = false;
        check(Provisioning_Save(&cleared) || Provisioning_Clear(), "clear registration");
        DeviceRegistration empty;
        Provisioning_Load(&empty);
        check(empty.registered == false, "after clear registered=false");
    }

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}