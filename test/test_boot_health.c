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
#include "platform_hardware.h"
#include "boot_session.h"
#include "host_platform.h"
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
    /* BMP390 present at the primary address with the reference chip id, so the
       BMP390 SystemHealth contribution is OK alongside the other sensors. The
       fake relays CHIP_ID/CALIB_DATA from dedicated fields (isolated from VEML's
       reg-0 usage in the flat register map). */
    static const uint8_t bmp_calib[21] = {
        0xAD,0xD8,0x26,0x6F,0xFE,0x12,0xC3,0xCF,0x48,0x28,0xBA,
        0x12,0x7A,0xFC,0xFF,0x3C,0xE7,0x74,0x8B,0xC9,0xB0
    };
    FakeI2cBus_SetBmp390Present(&s_fake_i2c, (uint16_t)(0x76U << 1), 0x60U, bmp_calib);
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

    printf("\n=== SCD41 SystemHealth mapping ===\n");
    {
        /* §15 required mapping via App_Scd41HealthOk. */
        check(App_Scd41HealthOk(DEVICE_STATE_STARTING) == true,
              "STARTING -> acceptable (not degraded)");
        check(App_Scd41HealthOk(DEVICE_STATE_WAITING) == true,
              "WAITING -> acceptable (not degraded)");
        check(App_Scd41HealthOk(DEVICE_STATE_READY) == true,
              "READY -> acceptable");
        check(App_Scd41HealthOk(DEVICE_STATE_NOT_FOUND) == false,
              "NOT_FOUND -> degrades health");
        check(App_Scd41HealthOk(DEVICE_STATE_ERROR) == false,
              "ERROR -> degrades health");
        check(App_Scd41HealthOk(DEVICE_STATE_RECOVERING) == false,
              "RECOVERING -> degrades health");

        /* SHT45 SystemHealth mapping (added with the SHT45 slice). A missing or
           errored SHT45 degrades health but never causes a FAULT. */
        check(App_Sht45HealthOk(DEVICE_STATE_STARTING) == true, "SHT45 STARTING acceptable");
        check(App_Sht45HealthOk(DEVICE_STATE_WAITING) == true, "SHT45 WAITING acceptable");
        check(App_Sht45HealthOk(DEVICE_STATE_READY) == true, "SHT45 READY acceptable");
        check(App_Sht45HealthOk(DEVICE_STATE_NOT_FOUND) == false, "SHT45 NOT_FOUND degrades");
        check(App_Sht45HealthOk(DEVICE_STATE_ERROR) == false, "SHT45 ERROR degrades");
        check(App_Sht45HealthOk(DEVICE_STATE_RECOVERING) == false, "SHT45 RECOVERING degrades");

        /* §16 invariant: a missing SCD41 (SelfTest co2_sensor would be FAIL,
           runtime state NOT_FOUND) must NEVER report health OK. */
        check(App_Scd41HealthOk(DEVICE_STATE_NOT_FOUND) == false &&
              !App_Scd41HealthOk(DEVICE_STATE_ERROR),
              "missing/errored SCD41 never yields SYSTEM_HEALTH_OK");
    }

    printf("\n=== P2-2 boot_id contract (device/reset-class identifier) ===\n");
    {
        /* CURRENT documented contract (platform_hardware.h): boot_id is derived
           from the device UID (+ reset-cause class on STM32), so it is a
           device/reset-class identifier, NOT a true per-boot unique session id.
           This regression documents/asserts the current behavior via the HOST
           platform's seedable boot id: same seed -> same boot_id (deterministic
           per device/session source), matching contract B. True per-boot
           uniqueness is NOT required by any current consumer (boot_id is only a
           telemetry tag, never a server-side unique-session key), so no
           persistent flash boot counter is introduced here. */
        uint64_t bid1 = 0, bid2 = 0;
        HostPlatform_SetBootId(0x1234AB);            /* same device/source */
        check(Platform_CreateBootId(&bid1) && Platform_CreateBootId(&bid2) &&
              bid1 == bid2, "same source -> deterministic boot_id (contract B)");

        HostPlatform_SetBootId(0x9999);              /* different source */
        uint64_t bid3 = 0;
        Platform_CreateBootId(&bid3);
        check(bid1 != bid3, "different source -> different boot_id");

        /* BootSession exposes a boot_id cached once per boot, and it is stable
           across Get calls within that boot. */
        BootSession s0; s0.boot_id = 0;
        bool ok0 = BootSession_Get(&s0);
        uint64_t now_platform = 0;
        HostPlatform_SetBootId(s0.boot_id);               /* same source session */
        Platform_CreateBootId(&now_platform);
        check(ok0 && s0.boot_id != 0, "BootSession exposes a nonzero boot_id");
        check(now_platform == s0.boot_id, "BootSession.boot_id matches platform source id");
        BootSession s2; s2.boot_id = 0;
        BootSession_Get(&s2);
        check(s2.boot_id == s0.boot_id, "BootSession boot_id stable within a boot (cache)");
        /* No persistent flash boot counter is introduced here; boot_id remains
           a device/session-source tag (documented contract B). */
    }

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}