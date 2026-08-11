#include <stdio.h>
#include <string.h>

#include "provisioning.h"
#include "provisioning_storage.h"
#include "storage.h"
#include "device_identity.h"
#include "config.h"
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

static EntityId inst_A, inst_X, building, room, ff_all;

static void InitA(void)
{
    FakeFlash_Init();
    FakeUniqueId_Set((const uint8_t[]){0xAA,0xBB,0xCC,0xDD,0x01,0x02,0x03,0x04,0xFE,0xED,0xBE,0xEF});
    Storage_Init();
    Provisioning_Init();
    EntityId_Parse(&inst_A, "11111111111111111111111111111111", 32);
    EntityId_Parse(&inst_X, "99999999999999999999999999999999", 32);
    EntityId_Parse(&building, "22222222222222222222222222222222", 32);
    EntityId_Parse(&room, "33333333333333333333333333333333", 32);
    memset(&ff_all, 0xFF, sizeof(ff_all));
}

/* Write a structurally-valid (CRC-correct) RegistrationStorageV1 to Flash.
   Storage validates structure only, so semantic corruption survives the write
   and is only rejected by Provisioning_Init's persisted-semantic validator. */
static void WriteRawRegistration(bool registered, const EntityId *inst,
                                 const EntityId *bld, const EntityId *room,
                                 uint32_t rev)
{
    RegistrationStorageV1 stored;
    memset(&stored, 0, sizeof(stored));
    stored.schema_version = REGISTRATION_SCHEMA_VERSION;
    stored.revision = rev;
    stored.registered = registered;
    if (inst) stored.installation_id = *inst;
    if (bld)  stored.building_id  = *bld;
    if (room) stored.room_id      = *room;
    Storage_Write(RECORD_TYPE_REGISTRATION, (const uint8_t *)&stored, sizeof(stored));
}

static void TestSemanticCorruption(void)
{
    printf("\n=== Persisted semantic validation ===\n");

    /* registered=false + non-zero installation -> CORRUPT */
    InitA();
    WriteRawRegistration(false, &inst_A, NULL, NULL, 1);
    Storage_Init();
    check(!Provisioning_Init(), "unregistered + non-zero install rejected");
    check(Provisioning_GetRuntime()->storage_status == STORAGE_READ_CORRUPT,
          "  -> storage_status CORRUPT");

    /* registered=true + invalid (all-FF) building -> CORRUPT */
    InitA();
    WriteRawRegistration(true, &inst_A, &ff_all, NULL, 1);
    Storage_Init();
    check(!Provisioning_Init(), "registered + all-FF building rejected");
    check(Provisioning_GetRuntime()->storage_status == STORAGE_READ_CORRUPT,
          "  -> storage_status CORRUPT");

    /* building zero + non-zero room -> CORRUPT (room without building) */
    InitA();
    WriteRawRegistration(true, &inst_A, NULL, &room, 1);
    Storage_Init();
    check(!Provisioning_Init(), "room without building rejected");
    check(Provisioning_GetRuntime()->storage_status == STORAGE_READ_CORRUPT,
          "  -> storage_status CORRUPT");

    /* registered=true + valid install + all-zero optional -> CONFIGURATION_PENDING */
    InitA();
    WriteRawRegistration(true, &inst_A, NULL, NULL, 1);
    Storage_Init();
    check(Provisioning_Init(), "valid partial (install only) loads");
    check(Provisioning_GetRuntime()->status.state == PROVISIONING_CONFIGURATION_PENDING,
          "  -> CONFIGURATION_PENDING");

    /* registered=true + valid install + building + room -> OPERATIONAL */
    InitA();
    WriteRawRegistration(true, &inst_A, &building, &room, 1);
    Storage_Init();
    check(Provisioning_Init(), "valid full registration loads");
    check(Provisioning_GetRuntime()->status.state == PROVISIONING_OPERATIONAL,
          "  -> OPERATIONAL");
}

static void TestRecovery(void)
{
    printf("\n=== Corrupt registration recovery (record-scoped) ===\n");
    InitA();

    /* establish a valid registration + identity that must be preserved */
    WriteRawRegistration(true, &inst_A, &building, &room, 1);
    Storage_Init();
    Provisioning_Init();

    DeviceIdentity did;
    DeviceIdentity_Derive(&did);
    check(DeviceIdentity_Save(&did), "identity persisted before corrupt");

    /* physically corrupt both registration slots */
    FakeFlash_Corrupt(8192, 40);
    FakeFlash_Corrupt(10240, 40);
    Storage_Init();
    check(!Provisioning_Init(), "init fails on both-corrupt");
    check(Provisioning_GetRuntime()->storage_status == STORAGE_READ_CORRUPT,
          "  -> storage_status CORRUPT");

    /* normal ownership mutation must still be refused */
    {
        DeviceRegistration takeover = {0};
        takeover.registered = true;
        takeover.installation_valid = true;
        takeover.installation_id = inst_X;
        check(!Provisioning_Save(&takeover), "takeover still denied on corrupt");
    }

    /* explicit recovery unregisters and repairs ONLY registration */
    check(Provisioning_Clear(), "recovery clear succeeds");
    check(!Provisioning_GetRuntime()->current.registered, "unregistered after clear");
    check(Provisioning_GetRuntime()->storage_status == STORAGE_READ_OK,
          "registration storage healthy after clear");

    /* config & identity are untouched by registration recovery */
    DeviceIdentity reloaded;
    check(DeviceIdentity_Load(&reloaded), "identity preserved after recovery");
    check(memcmp(reloaded.device_uuid, did.device_uuid, 16) == 0,
          "identity bytes preserved after recovery");
}

static void TestIoRecoveryRefused(void)
{
    printf("\n=== IO_ERROR recovery refused ===\n");
    InitA();
    WriteRawRegistration(true, &inst_A, NULL, NULL, 1);
    Storage_Init();
    Provisioning_Init();

    /* Flash read fails on registration -> IO_ERROR state. */
    FakeFlash_SetReadFail(true, 8192, 12288);
    bool init_ok = Provisioning_Init();
    FakeFlash_SetReadFail(false, 0, 0);
    check(!init_ok, "init fails on IO_ERROR");
    check(Provisioning_GetRuntime()->storage_status == STORAGE_READ_IO_ERROR,
          "  -> storage_status IO_ERROR");

    /* Destructive clear must NOT be attempted under IO uncertainty. */
    check(!Provisioning_Clear(), "clear refused on IO_ERROR (fail closed)");
}

static void TestFactoryPreservesIdentity(void)
{
    printf("\n=== Factory reset preserves identity ===\n");
    InitA();
    DeviceIdentity did;
    DeviceIdentity_Derive(&did);
    check(DeviceIdentity_Save(&did), "identity persisted");

    WriteRawRegistration(true, &inst_A, &building, &room, 1);
    Storage_Init();
    Provisioning_Init();

    /* corrupt registration, then factory-style recovery + config reset */
    FakeFlash_Corrupt(8192, 40);
    FakeFlash_Corrupt(10240, 40);
    Storage_Init();
    Provisioning_Init();
    check(Provisioning_Clear(), "factory registration recovery");
    check(Config_ResetToDefaults(), "factory config reset");

    /* device identity record is never erased by product factory reset */
    DeviceIdentity reloaded;
    check(DeviceIdentity_Load(&reloaded), "identity preserved after factory reset");
    check(memcmp(reloaded.device_uuid, did.device_uuid, 16) == 0,
          "identity bytes preserved after factory reset");
}

int main(void)
{
    printf("Provisioning Host Tests\n");
    fflush(stdout);

    InitA();

    /* ---- canonical invariant rules ---- */
    printf("\n=== Canonical invariants ===\n");
    {
        DeviceRegistration valid_empty = {0};
        check(Provisioning_ValidateRegistration(&valid_empty),
              "canonical unregistered accepted");

        DeviceRegistration bad_unreg = {0};
        bad_unreg.registered = false;
        bad_unreg.installation_valid = true;  /* contradictory: flag w/o id */
        check(!Provisioning_ValidateRegistration(&bad_unreg),
              "unregistered with valid flag rejected");

        DeviceRegistration bad_inst = {0};
        bad_inst.registered = true;
        /* no valid installation id */
        check(!Provisioning_ValidateRegistration(&bad_inst),
              "registered without valid installation rejected");

        DeviceRegistration reg_inst = {0};
        reg_inst.registered = true;
        reg_inst.installation_valid = true;
        reg_inst.installation_id = inst_A;
        check(Provisioning_ValidateRegistration(&reg_inst),
              "registered with valid installation accepted");

        DeviceRegistration reg_bad_building = reg_inst;
        reg_bad_building.building_id = building;
        reg_bad_building.building_valid = true;
        /* building_valid with installation_valid implicitly true; ok here */
        check(Provisioning_ValidateRegistration(&reg_bad_building),
              "building+installation accepted");
    }
    {
        DeviceRegistration reg = {0};
        reg.registered = true;
        reg.installation_valid = true;
        reg.installation_id = inst_A;
        reg.building_id = building;
        reg.room_id = room;
        reg.building_valid = true;
        reg.room_valid = true;
        check(Provisioning_ValidateRegistration(&reg),
              "fully-registered canonical accepted");
    }
    {
        DeviceRegistration reg = {0};
        reg.registered = true;
        reg.installation_id = inst_A;
        reg.building_valid = true;  /* building_valid true but building_id zero */
        reg.room_valid = false;
        check(!Provisioning_ValidateRegistration(&reg),
              "building_valid with zero building_id rejected");
    }
    {
        DeviceRegistration reg = {0};
        reg.registered = true;
        reg.installation_id = inst_A;
        reg.building_id = building;
        reg.building_valid = true;
        reg.room_valid = true;       /* room_valid true but room_id zero */
        check(!Provisioning_ValidateRegistration(&reg),
              "room_valid with zero room_id rejected");
    }
    {
        DeviceRegistration reg = {0};
        reg.registered = true;
        reg.installation_id = inst_A;
        reg.building_valid = false;
        reg.room_valid = true;       /* invalid entity (building/room) */
        check(!Provisioning_ValidateRegistration(&reg),
              "building_valid false but room_valid true rejected");
    }

    /* ---- blank -> DISCOVERABLE ---- */
    printf("\n=== Blank storage ===\n");
    {
        const ProvisioningRuntime *rt = Provisioning_GetRuntime();
        check(rt->storage_status == STORAGE_READ_NOT_FOUND, "blank storage NOT_FOUND");
        check(rt->status.state == PROVISIONING_DISCOVERABLE, "state = DISCOVERABLE");
        check(rt->current.registered == false, "not registered");
    }

    /* ---- register + runtime owner ---- */
    printf("\n=== Register / runtime owner ===\n");
    {
        DeviceRegistration updated = Provisioning_GetRuntime()->current;
        updated.registered = true;
        updated.installation_valid = true;
        updated.installation_id = inst_A;
        check(Provisioning_Save(&updated), "register A succeeds");

        const ProvisioningRuntime *rt = Provisioning_GetRuntime();
        check(rt->current.registered, "runtime registered");
        check(memcmp(rt->current.installation_id.bytes, inst_A.bytes, 16) == 0,
              "runtime installation = A");
        check(rt->storage_status == STORAGE_READ_OK, "storage status OK after save");

        /* GET performs zero Flash reads: corrupt flash after init; runtime is
           cached in RAM and must NOT change. */
        FakeFlash_Corrupt(8192, 24);  /* REGISTRATION region (pages 4-5) */
        rt = Provisioning_GetRuntime();
        check(rt->current.registered, "runtime cached, no Flash reload on GET");
        check(rt->storage_status == STORAGE_READ_OK, "runtime status cached");
        FakeFlash_Init(); Storage_Init(); Provisioning_Init();
        check(Provisioning_GetRuntime()->storage_status == STORAGE_READ_NOT_FOUND,
              "re-init after reset");
    }

    /* ---- takeover after corrupt registration denied ---- */
    printf("\n=== Corrupt registration fail-closed ===\n");
    {
        InitA();
        DeviceRegistration updated = Provisioning_GetRuntime()->current;
        updated.registered = true;
        updated.installation_valid = true;
        updated.installation_id = inst_A;
        Provisioning_Save(&updated);

        /* Corrupt both registration slots -> CORRUPT. */
        FakeFlash_Corrupt(8192, 40);
        FakeFlash_Corrupt(10240, 40);
        bool init_ok = Provisioning_Init();
        const ProvisioningRuntime *rt = Provisioning_GetRuntime();
        check(!init_ok, "init fails on corrupt storage");
        check(rt->storage_status == STORAGE_READ_CORRUPT, "storage status CORRUPT");
        check(rt->status.state == PROVISIONING_ERROR, "state ERROR on corrupt");

        /* Ownership takeover (register to X) must be refused. */
        DeviceRegistration takeover = {0};
        takeover.registered = true;
        takeover.installation_id = inst_X;
        check(!Provisioning_Save(&takeover),
              "ownership takeover after corrupt denied (fail closed)");
    }

    /* ---- IO failure -> ERROR ---- */
    printf("\n=== IO failure ===\n");
    {
        InitA();
        DeviceRegistration updated = Provisioning_GetRuntime()->current;
        updated.registered = true;
        updated.installation_valid = true;
        updated.installation_id = inst_A;
        Provisioning_Save(&updated);

        FakeFlash_SetReadFail(true, 8192, 12288);  /* registration unreadable */
        bool init_ok = Provisioning_Init();
        FakeFlash_SetReadFail(false, 0, 0);
        const ProvisioningRuntime *rt = Provisioning_GetRuntime();
        check(!init_ok, "init fails on IO error");
        check(rt->storage_status == STORAGE_READ_IO_ERROR, "storage status IO_ERROR");
        check(rt->status.state == PROVISIONING_ERROR, "state ERROR on IO failure");

        DeviceRegistration takeover = {0};
        takeover.registered = true;
        takeover.installation_id = inst_X;
        check(!Provisioning_Save(&takeover), "mutation denied on IO failure");
    }

    /* ---- invalid building / room ID via Save ---- */
    printf("\n=== Invalid entity IDs ===\n");
    {
        InitA();
        DeviceRegistration updated = Provisioning_GetRuntime()->current;
        updated.registered = true;
        updated.installation_valid = true;
        updated.installation_id = inst_A;
        updated.building_id = building;
        updated.building_valid = true;
        updated.room_valid = false;
        check(Provisioning_Save(&updated), "register + building (no room) accepted");

        DeviceRegistration with_bad_room = Provisioning_GetRuntime()->current;
        with_bad_room.room_valid = true;   /* room id still zero */
        check(!Provisioning_Save(&with_bad_room), "zero room_id cannot be saved");

        DeviceRegistration with_bad_building = Provisioning_GetRuntime()->current;
        with_bad_building.building_valid = true;
        with_bad_building.building_id = (EntityId){0};
        check(!Provisioning_Save(&with_bad_building), "zero building_id cannot be saved");
    }

    TestSemanticCorruption();
    TestRecovery();
    TestIoRecoveryRefused();
    TestFactoryPreservesIdentity();

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}