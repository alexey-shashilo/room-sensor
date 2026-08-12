#include <stdio.h>
#include <string.h>

#include "app.h"
#include "config.h"
#include "device_identity.h"
#include "storage.h"
#include "provisioning.h"
#include "device_lifecycle.h"
#include "room_sensor_types.h"
#include "platform_unique_id.h"
#include "fake_flash.h"
#include "fake_i2c_bus.h"
#include "fake_unique_id.h"
#include "fake_platform_time.h"

/* App-level (boot-orchestration) first-boot regression.

   This drives the REAL boot policy through App_Init + a full App_Run lifecycle
   (BOOT -> LOAD_CONFIGURATION -> LOAD_IDENTITY -> CREATE_BOOT_SESSION ->
   SELF_TEST -> PROBE_PERIPHERALS -> INITIALIZE_DRIVERS -> READY -> OPERATIONAL)
   starting from blank persistence with I2C available and both peripherals
   present. It asserts the same health inputs App_UpdateHealth uses, so it
   catches any stale Config/Identity cached health after the boot mirror
   establishment — which low-level Storage_EnsureRedundancy unit tests cannot. */

static int s_pass = 0, s_fail = 0, s_case = 0;
static void check(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

static FakeI2cBus s_fake_i2c;
static I2cBus     s_bus;

static void SetupHealthyPeripherals(void)
{
    FakeI2cBus_Init(&s_fake_i2c);
    s_fake_i2c.probe_result = DRIVER_STATUS_OK;   /* VEML + display both present */
    FakeI2cBus_GetBus(&s_bus, &s_fake_i2c);
}

/* App_Run advances exactly ONE lifecycle transition per call (like the firmware
   main loop). Drive the full boot: BOOT -> ... -> READY/OPERATIONAL. */
static void RunBootToOperational(void)
{
    int guard = 0;
    while (!DeviceLifecycle_IsOperational() && guard < 20)
    {
        App_Run();
        guard++;
    }
}

int main(void)
{
    printf("App First-Boot Health Host Tests\n");
    fflush(stdout);

    /* Dirty the module caches before boot to make the regression meaningful:
       force Config/DeviceIdentity to remember a single-copy (DEGRADED) state
       that App must repair through the OWNER API during boot. */
    printf("\n=== clean first boot -> SYSTEM_HEALTH_OK, HEALTHY mirrors ===\n");
    {
        FakeFlash_Init();
        FakeUniqueId_Set((const uint8_t[]){0xAA,0xBB,0xCC,0xDD,0x01,0x02,0x03,0x04,0xFE,0xED,0xBE,0xEF});
        FakePlatform_SetTick(0);
        Storage_Init();

        /* Pretend a prior single-copy persistence happened so the module caches
           start DEGRADED (the staleness the boot must fix). */
        Config_Load();
        Config_LoadDefaults();
        Config_Save();                       /* single valid copy: slot A */
        check(Config_GetStorageHealth() == STORAGE_HEALTH_DEGRADED,
              "pre: cached config health DEGRADED (single copy)");
        DeviceIdentity id;
        DeviceIdentity_Derive(&id);
        DeviceIdentity_Save(&id);            /* single valid copy: slot A */
        check(DeviceIdentity_GetStorageHealth() == STORAGE_HEALTH_DEGRADED,
              "pre: cached identity health DEGRADED (single copy)");

        /* True blank first-boot with peripherals present. */
        FakeFlash_Init();                    /* wipe -> blank factory-new */
        FakeFlash_ResetIoCounters();

        SetupHealthyPeripherals();
        App_SetI2C(&s_bus);
        check(App_Init() == ROOM_SENSOR_OK, "App_Init OK");

        /* Drive the whole boot lifecycle to OPERATIONAL. */
        FakePlatform_SetTick(1);
        RunBootToOperational();

        check(DeviceLifecycle_GetState() == LIFECYCLE_OPERATIONAL,
              "lifecycle reached OPERATIONAL");

        /* Config covers: already persisted (slot A). Expect boot repaired B. */
        StorageInfo ci, ii, ri;
        Storage_GetPageInfo(RECORD_TYPE_CONFIG, &ci);
        check(ci.slot_a_valid && ci.slot_b_valid,
              "Config storage VALID + VALID (mirror established)");
        Storage_GetPageInfo(RECORD_TYPE_IDENTITY, &ii);
        check(ii.slot_a_valid && ii.slot_b_valid,
              "Identity storage VALID + VALID (mirror established)");

        check(Config_GetStorageStatus() == STORAGE_READ_OK,
              "Config_GetStorageStatus() == OK");
        check(Config_GetStorageHealth() == STORAGE_HEALTH_HEALTHY,
              "Config_GetStorageHealth() == HEALTHY (stale degraded fixed)");
        check(DeviceIdentity_GetPersistenceStatus() == STORAGE_READ_OK,
              "DeviceIdentity_GetPersistenceStatus() == OK");
        check(DeviceIdentity_GetStorageHealth() == STORAGE_HEALTH_HEALTHY,
              "DeviceIdentity_GetStorageHealth() == HEALTHY (stale degraded fixed)");
        check(Storage_GetHealth(RECORD_TYPE_CONFIG) == STORAGE_HEALTH_HEALTHY,
              "physical Config mirror HEALTHY");
        check(Storage_GetHealth(RECORD_TYPE_IDENTITY) == STORAGE_HEALTH_HEALTHY,
              "physical Identity mirror HEALTHY");

        /* Provisioning: blank registration is acceptable and healthy. */
        Storage_GetPageInfo(RECORD_TYPE_REGISTRATION, &ri);
        check(!ri.slot_a_valid && !ri.slot_b_valid,
              "Registration storage NOT_FOUND (both blank)");
        const ProvisioningRuntime *pr = Provisioning_GetRuntime();
        check(pr != NULL && pr->status.state == PROVISIONING_DISCOVERABLE,
              "Provisioning DISCOVERABLE (unregistered factory-new)");
        check(pr != NULL && pr->storage_status == STORAGE_READ_NOT_FOUND,
              "Provisioning storage NOT_FOUND (acceptable)");

        /* System health via the authoritative AppStatus + internal health. */
        AppStatus st;
        App_GetStatus(&st);
        check(st.health == SYSTEM_HEALTH_OK, "SystemHealth == SYSTEM_HEALTH_OK");
        check(st.light_sensor.state == DEVICE_STATE_READY, "VEML READY");
        check(st.display.state == DEVICE_STATE_READY, "display READY");
        check(st.storage_initialized, "storage initialized");
        check(st.provisioning_initialized, "provisioning healthy");
        check(st.config_storage_health == STORAGE_HEALTH_HEALTHY,
              "AppStatus config health HEALTHY");
        check(st.identity_storage_health == STORAGE_HEALTH_HEALTHY,
              "AppStatus identity health HEALTHY");
        check(st.provisioning_storage_health == STORAGE_HEALTH_HEALTHY,
              "AppStatus provisioning health HEALTHY");

        /* SelfTest must reflect the repaired healthy mirrors, not stale state. */
        const SelfTestReport *sr = SelfTest_GetReport();
        check(sr->config == SELF_TEST_PASS, "SelfTest Config PASS");
        check(sr->identity == SELF_TEST_PASS, "SelfTest Identity PASS");
    }

    /* ============ POWER-LOSS BOOT REPAIR: single-copy -> HEALTHY ========== */
    printf("\n=== reboot from single-copy Config & Identity repairs mirrors ===\n");
    {
        /* previous boot left Config A valid/B erased; Identity A valid/B erased */
        FakeFlash_Init();
        FakeUniqueId_Set((const uint8_t[]){0x11,0x22,0x33,0x44,0x01,0x02,0x03,0x04,0xFE,0xED,0xBE,0xEF});
        Storage_Init();
        Config_Load(); Config_LoadDefaults();
        Config_Save();                 /* slot A */
        DeviceIdentity did;
        DeviceIdentity_Derive(&did);
        DeviceIdentity_Save(&did);     /* slot A */
        check(Config_GetStorageHealth() == STORAGE_HEALTH_DEGRADED, "pre cfg DEGRADED");
        check(DeviceIdentity_GetStorageHealth() == STORAGE_HEALTH_DEGRADED, "pre id DEGRADED");

        /* "reboot": re-init runtime stack, keep Flash as-is (single copy). */
        SetupHealthyPeripherals();
        App_SetI2C(&s_bus);
        check(App_Init() == ROOM_SENSOR_OK, "App_Init OK (reboot)");
        FakePlatform_SetTick(2);
        RunBootToOperational();

        check(DeviceLifecycle_GetState() == LIFECYCLE_OPERATIONAL, "OPERATIONAL");
        check(Config_GetStorageHealth() == STORAGE_HEALTH_HEALTHY,
              "Config cached health HEALTHY after reboot repair (no extra reboot)");
        check(DeviceIdentity_GetStorageHealth() == STORAGE_HEALTH_HEALTHY,
              "Identity cached health HEALTHY after reboot repair");
        check(Storage_GetHealth(RECORD_TYPE_CONFIG) == STORAGE_HEALTH_HEALTHY,
              "Config physical mirror HEALTHY");
        check(Storage_GetHealth(RECORD_TYPE_IDENTITY) == STORAGE_HEALTH_HEALTHY,
              "Identity physical mirror HEALTHY");
        AppStatus st; App_GetStatus(&st);
        check(st.health == SYSTEM_HEALTH_OK, "SystemHealth OK after reboot repair");
    }

    /* ====== RUNTIME DIAGNOSTICS use CURRENT state, not boot snapshot ====== */
    printf("\n=== runtime diagnostics reflect current persistence, not boot snapshot ===\n");
    {
        /* This block needs a completed first-boot under App control so the
           module current status is refreshed by the boot (defaults persisted +
           mirrors established). Re-run a clean first boot. */
        FakeFlash_Init();
        FakeUniqueId_Set((const uint8_t[]){0x9A,0x8B,0x7C,0x6D,0x01,0x02,0x03,0x04,0xFE,0xED,0xBE,0xEF});
        FakePlatform_SetTick(0);
        Storage_Init();
        SetupHealthyPeripherals();
        App_SetI2C(&s_bus);
        check(App_Init() == ROOM_SENSOR_OK, "App_Init OK (diagnostics first boot)");
        FakePlatform_SetTick(3);
        RunBootToOperational();

        /* Runtime diagnostics call Config_GetStorageStatus()/.GetStorageHealth(),
           NOT the boot-load snapshot. The device booted BLANK (boot snapshot
           NOT_FOUND), but after defaults were persisted the CURRENT status is OK.
           A diagnostic that used the boot snapshot would print "defaults"; the
           fixed code prints the current "read=OK health=HEALTHY". */
        check(Config_GetStorageStatus() == STORAGE_READ_OK,
              "diagnostics current config read status == OK (NOT stale defaults)");
        check(Config_GetStorageHealth() == STORAGE_HEALTH_HEALTHY,
              "diagnostics current config health == HEALTHY");
        check(DeviceIdentity_GetPersistenceStatus() == STORAGE_READ_OK,
              "diagnostics current identity persistence == OK");
        check(DeviceIdentity_GetStorageHealth() == STORAGE_HEALTH_HEALTHY,
              "diagnostics current identity health == HEALTHY");

        AppStatus diag;
        App_GetStatus(&diag);
        check(diag.config_storage_status == STORAGE_READ_OK,
              "AppStatus config read status == OK (current, not NOT_FOUND)");
        check(diag.identity_storage_status == STORAGE_READ_OK,
              "AppStatus identity persistence == OK (current)");
    }

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}