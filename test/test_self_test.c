#include <stdio.h>
#include <string.h>

#include "self_test.h"
#include "config.h"
#include "device_identity.h"
#include "storage.h"
#include "fake_flash.h"
#include "fake_i2c_bus.h"
#include "fake_unique_id.h"
#include "fake_platform_time.h"

/* Side-effect-free self-test tests.

   Normal SelfTest_Run must be strictly OBSERVATIONAL: it inspects current
   persistent state and performs ZERO Flash writes/erases, does not call
   Storage_Init(), does not mutate runtime Config, and does not persist
   identity. These tests also pin the persistence-status model:
     config/identity healthy       -> SELF_TEST_PASS
     blank/first-boot (NOT_FOUND)  -> SELF_TEST_SKIPPED
     corrupt / IO_ERROR            -> SELF_TEST_FAIL
   and that a successful first-boot save yields current persistence status OK. */

static int s_pass = 0, s_fail = 0, s_case = 0;
static void check(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

static FakeI2cBus s_fake_i2c;
static I2cBus     s_bus;

static void InitI2cHealthy(void)
{
    FakeI2cBus_Init(&s_fake_i2c);
    s_fake_i2c.probe_result = DRIVER_STATUS_OK;
    FakeI2cBus_GetBus(&s_bus, &s_fake_i2c);
}

static void InitI2cMissing(void)
{
    FakeI2cBus_Init(&s_fake_i2c);
    s_fake_i2c.probe_result = DRIVER_STATUS_BUS_ERROR;
    FakeI2cBus_GetBus(&s_bus, &s_fake_i2c);
}

/* Boot setup: storage + config + identity all persisted healthy. */
static void BootHealthy(void)
{
    FakeFlash_Init();
    Storage_Init();
    Config_Load();
    Config_LoadDefaults();
    Config_Save();
    DeviceIdentity seed;
    DeviceIdentity_Derive(&seed);
    DeviceIdentity_Save(&seed);
    FakeFlash_ResetIoCounters();
}

/* Determine probe result from the current fake probe_result. */
static bool ProbeOk(void)
{
    return s_fake_i2c.probe_result == DRIVER_STATUS_OK;
}

int main(void)
{
    printf("SelfTest Side-Effect-Free Host Tests\n");
    fflush(stdout);

    /* ============ 1. Healthy config/identity -> PASS, zero writes ========== */
    printf("\n=== healthy storage -> config/identity PASS, zero Flash ops ===\n");
    {
        InitI2cHealthy();
        BootHealthy();
        bool init_before = Storage_IsInitialized();
        uint32_t w0 = FakeFlash_GetWriteCount();
        uint32_t e0 = FakeFlash_GetEraseCount();

        SelfTestReport r;
        SelfTest_Run(&r, &s_bus);

        check(r.config == SELF_TEST_PASS, "healthy config -> PASS");
        check(r.identity == SELF_TEST_PASS, "healthy identity -> PASS");
        check(r.storage == SELF_TEST_PASS, "storage initialized -> PASS");
        check(ProbeOk() && r.light_sensor == SELF_TEST_PASS, "VEML present -> PASS");
        check(ProbeOk() && r.display == SELF_TEST_PASS, "display present -> PASS");
        check(FakeFlash_GetWriteCount() == w0, "zero Flash writes during self-test");
        check(FakeFlash_GetEraseCount() == e0, "zero Flash erases during self-test");
        check(Storage_IsInitialized() == init_before, "Storage_IsInitialized unchanged");
    }

    /* ============ 2. Blank (first boot) config/identity -> SKIPPED ========= */
    printf("\n=== blank storage -> config/identity SKIPPED, zero Flash ops ===\n");
    {
        InitI2cHealthy();
        FakeFlash_Init();
        Storage_Init();
        FakeFlash_ResetIoCounters();
        bool init_before = Storage_IsInitialized();

        SelfTestReport r;
        SelfTest_Run(&r, &s_bus);

        check(r.config == SELF_TEST_SKIPPED, "blank config -> SKIPPED (first boot)");
        check(r.identity == SELF_TEST_SKIPPED, "blank identity -> SKIPPED (first boot)");
        check(Storage_IsInitialized() == init_before, "Storage_IsInitialized unchanged (blank)");
        check(FakeFlash_GetWriteCount() == 0, "blank: zero Flash writes");
        check(FakeFlash_GetEraseCount() == 0, "blank: zero Flash erases");
    }

    /* ============ 3. Corrupt config -> FAIL, zero writes ================== */
    printf("\n=== corrupt config -> config FAIL, zero Flash ops ===\n");
    {
        InitI2cHealthy();
        BootHealthy();
        FakeFlash_Corrupt(0, 24);   /* config slot A header */
        bool init_before = Storage_IsInitialized();

        SelfTestReport r;
        SelfTest_Run(&r, &s_bus);

        check(r.config == SELF_TEST_FAIL, "corrupt config -> FAIL");
        check(r.identity == SELF_TEST_PASS, "corrupt config does not affect identity");
        check(Storage_IsInitialized() == init_before, "Storage_IsInitialized unchanged (corrupt)");
        check(FakeFlash_GetWriteCount() == 0, "corrupt config: zero Flash writes");
        check(FakeFlash_GetEraseCount() == 0, "corrupt config: zero Flash erases");
    }

    /* ============ 4. Corrupt identity -> FAIL, zero writes ================ */
    printf("\n=== corrupt identity -> identity FAIL, zero Flash ops ===\n");
    {
        InitI2cHealthy();
        BootHealthy();
        FakeFlash_Corrupt(4096, 24);   /* identity slot A header */
        bool init_before = Storage_IsInitialized();

        SelfTestReport r;
        SelfTest_Run(&r, &s_bus);

        check(r.identity == SELF_TEST_FAIL, "corrupt identity -> FAIL");
        check(r.config == SELF_TEST_PASS, "corrupt identity does not affect config");
        check(Storage_IsInitialized() == init_before, "Storage_IsInitialized unchanged (corrupt ident)");
        check(FakeFlash_GetWriteCount() == 0, "corrupt identity: zero Flash writes");
        check(FakeFlash_GetEraseCount() == 0, "corrupt identity: zero Flash erases");
    }

    /* ============ 5. Config IO_ERROR -> FAIL, zero writes ================= */
    printf("\n=== config IO_ERROR -> config FAIL, zero Flash ops ===\n");
    {
        InitI2cHealthy();
        BootHealthy();
        FakeFlash_SetReadFail(true, 0, 2048);   /* config slot A unreadable */
        bool init_before = Storage_IsInitialized();

        SelfTestReport r;
        SelfTest_Run(&r, &s_bus);
        FakeFlash_SetReadFail(false, 0, 0);

        check(r.config == SELF_TEST_FAIL, "config IO_ERROR -> FAIL");
        check(Storage_IsInitialized() == init_before, "Storage_IsInitialized unchanged (io)");
        check(FakeFlash_GetWriteCount() == 0, "config IO_ERROR: zero Flash writes");
        check(FakeFlash_GetEraseCount() == 0, "config IO_ERROR: zero Flash erases");
    }

    /* ============ 6. Identity IO_ERROR -> FAIL, zero writes =============== */
    printf("\n=== identity IO_ERROR -> identity FAIL, zero Flash ops ===\n");
    {
        InitI2cHealthy();
        BootHealthy();
        FakeFlash_SetReadFail(true, 4096, 8192);   /* identity region unreadable */
        bool init_before = Storage_IsInitialized();

        SelfTestReport r;
        SelfTest_Run(&r, &s_bus);
        FakeFlash_SetReadFail(false, 0, 0);

        check(r.identity == SELF_TEST_FAIL, "identity IO_ERROR -> FAIL");
        check(Storage_IsInitialized() == init_before, "Storage_IsInitialized unchanged (ident io)");
        check(FakeFlash_GetWriteCount() == 0, "identity IO_ERROR: zero Flash writes");
        check(FakeFlash_GetEraseCount() == 0, "identity IO_ERROR: zero Flash erases");
    }

    /* ============ 7. Self-test does NOT mutate runtime Config ============= */
    printf("\n=== custom runtime Config unchanged after self-test ===\n");
    {
        InitI2cHealthy();
        BootHealthy();
        /* Apply a custom config and persist it. */
        RoomSensorConfig custom = *Config_Get();
        custom.storage.light_period_ms = 7777U;
        custom.storage.retry_period_ms = 9999U;
        custom.runtime.light_calibration_factor = 2.5f;
        check(Config_ApplyPersistent(&custom) == CONFIG_APPLY_OK, "custom config applied");
        const RoomSensorConfig *before = Config_Get();
        RoomSensorConfig before_copy = *before;
        FakeFlash_ResetIoCounters();

        SelfTestReport r;
        SelfTest_Run(&r, &s_bus);

        const RoomSensorConfig *after = Config_Get();
        check(memcmp(&before_copy, after, sizeof(RoomSensorConfig)) == 0,
              "runtime Config byte-identical after self-test");
        check(after->storage.light_period_ms == 7777U, "runtime stays custom (light_period)");
        check(after->runtime.light_calibration_factor == 2.5f, "runtime stays custom (calib)");
        check(FakeFlash_GetWriteCount() == 0, "no Config writes during self-test");
        check(FakeFlash_GetEraseCount() == 0, "no Config erases during self-test");
    }

    /* ============ 8. Self-test does NOT mutate identity persistence ======= */
    printf("\n=== identity persistence status unchanged after self-test ===\n");
    {
        InitI2cHealthy();
        BootHealthy();
        StorageReadStatus ps_before = DeviceIdentity_GetPersistenceStatus();
        FakeFlash_ResetIoCounters();

        SelfTestReport r;
        SelfTest_Run(&r, &s_bus);

        check(DeviceIdentity_GetPersistenceStatus() == ps_before,
              "identity persistence status unchanged");
        check(FakeFlash_GetWriteCount() == 0, "no identity writes during self-test");
        check(FakeFlash_GetEraseCount() == 0, "no identity erases during self-test");
    }

    /* ============ 9. Sensor/display missing correctly reported ============ */
    printf("\n=== VEML/display missing -> both FAIL ===\n");
    {
        InitI2cMissing();
        BootHealthy();

        SelfTestReport r;
        SelfTest_Run(&r, &s_bus);

        check(r.light_sensor == SELF_TEST_FAIL, "VEML missing -> FAIL");
        check(r.display == SELF_TEST_FAIL, "display missing -> FAIL");
    }

    /* ============ 10. NULL bus -> skipped sensors ------------------------- */
    printf("\n=== NULL bus -> sensors SKIPPED ===\n");
    {
        BootHealthy();
        SelfTestReport r;
        SelfTest_Run(&r, NULL);
        check(r.light_sensor == SELF_TEST_SKIPPED, "NULL bus: light_sensor SKIPPED");
        check(r.display == SELF_TEST_SKIPPED, "NULL bus: display SKIPPED");
    }

    /* ============ Persistence status model ================================ */
    printf("\n=== config current-persistence status model ===\n");
    {
        /* first boot NOT_FOUND -> save success -> current status OK */
        FakeFlash_Init();
        Storage_Init();
        check(Config_Load() == false, "first boot Config_Load false");
        check(Config_GetStorageStatus() == STORAGE_READ_NOT_FOUND, "first boot status NOT_FOUND");
        Config_LoadDefaults();
        bool saved = Config_Save();
        check(saved, "first boot config save succeeds");
        check(Config_GetStorageStatus() == STORAGE_READ_OK, "after successful save status OK");

        /* load OK -> OK */
        Config_Load();
        check(Config_GetStorageStatus() == STORAGE_READ_OK, "load persisted -> status OK");

        /* corrupt -> CORRUPT */
        FakeFlash_Init();
        Storage_Init();
        Config_Load();
        Config_LoadDefaults();
        Config_Save();
        FakeFlash_Corrupt(0, 24);
        check(Config_Load() == false, "corrupt config load false");
        check(Config_GetStorageStatus() == STORAGE_READ_CORRUPT, "corrupt -> status CORRUPT");

        /* successful explicit recovery (Storage_RecoverCorruptRecord) -> a
           subsequent load is OK and current status is OK */
        ConfigStorageV1 rec;
        memset(&rec, 0, sizeof(rec));
        rec.version = CONFIG_SCHEMA_VERSION;
        rec.light_period_ms = 500U;
        rec.display_period_ms = 500U;
        rec.diagnostics_period_ms = 10000U;
        rec.retry_period_ms = 5000U;
        rec.telemetry_period_ms = 5000U;
        rec.light_calibration_q16 = (uint32_t)(1.0f * 65536.0f);
        check(Storage_RecoverCorruptRecord(RECORD_TYPE_CONFIG,
                                           (const uint8_t *)&rec, sizeof(rec)) ==
              STORAGE_RECOVERY_OK, "explicit config recovery succeeds");
        check(Config_Load(), "load after recovery -> OK");
        check(Config_GetStorageStatus() == STORAGE_READ_OK,
              "after recovery load status OK");

        /* failed save does not falsely report OK */
        FakeFlash_Init();
        Storage_Init();
        Config_Load();
        Config_LoadDefaults();
        FakeFlash_SetWriteFail(true);
        check(Config_Save() == false, "failed save returns false");
        FakeFlash_SetWriteFail(false);
        check(Config_GetStorageStatus() != STORAGE_READ_OK, "failed save does not report OK");
    }

    /* ============ Identity current-persistence status model =============== */
    printf("\n=== identity current-persistence status model ===\n");
    {
        /* first boot NOT_FOUND -> derive+save -> status OK */
        FakeFlash_Init();
        Storage_Init();
        DeviceIdentity id;
        check(DeviceIdentity_Load(&id) == false, "first boot identity load false");
        check(DeviceIdentity_GetPersistenceStatus() == STORAGE_READ_NOT_FOUND,
              "first boot identity status NOT_FOUND");
        DeviceIdentity_Derive(&id);
        bool saved = DeviceIdentity_Save(&id);
        check(saved, "identity save succeeds");
        check(DeviceIdentity_GetPersistenceStatus() == STORAGE_READ_OK,
              "after successful identity save status OK");

        /* load OK -> OK */
        check(DeviceIdentity_Load(&id), "load persisted identity");
        check(DeviceIdentity_GetPersistenceStatus() == STORAGE_READ_OK,
              "load persisted -> status OK");

        /* corrupt -> CORRUPT */
        FakeFlash_Init();
        Storage_Init();
        DeviceIdentity_Derive(&id);
        DeviceIdentity_Save(&id);
        FakeFlash_Corrupt(4096, 24);
        check(DeviceIdentity_Load(&id) == false, "corrupt identity load false");
        check(DeviceIdentity_GetPersistenceStatus() == STORAGE_READ_CORRUPT,
              "corrupt -> status CORRUPT");

        /* failed save does not falsely report OK */
        FakeFlash_Init();
        Storage_Init();
        DeviceIdentity_Derive(&id);
        FakeFlash_SetWriteFail(true);
        check(DeviceIdentity_Save(&id) == false, "failed identity save returns false");
        FakeFlash_SetWriteFail(false);
        check(DeviceIdentity_GetPersistenceStatus() != STORAGE_READ_OK,
              "failed identity save does not report OK");
    }

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}