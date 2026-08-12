#include <stdio.h>
#include <string.h>
#include <math.h>

#include "config.h"
#include "device_identity.h"
#include "storage.h"
#include "fake_flash.h"
#include "fake_unique_id.h"
#include "fake_platform_time.h"

/* Config / Identity mirror-ownership and failed-write regressions.

   The core regression: Config and DeviceIdentity must refresh BOTH their
   current readable-persistence status and their A/B redundancy health through
   the OWNER module API (Config_EnsureRedundancy / DeviceIdentity_EnsureRedundancy)
   after the Storage mirror maintenance that App does at boot. The module
   cached health must NEVER disagree with the physical mirrors.

   Also pins the transactional failed-write semantics: a verify / IO / unsafe
   write failure leaves the runtime OLD, preserves the old readable record, and
   reports an exact last_write_status — never "IO_ERROR" for an unsafe pair and
   never a forced CORRUPT because a write failed. */

static int s_pass = 0, s_fail = 0, s_case = 0;
static void check(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

static uint8_t s_uid[12] = {0xAA,0xBB,0xCC,0xDD,0x01,0x02,0x03,0x04,0xFE,0xED,0xBE,0xEF};

/* Persist a config with the given light_period, returning the committed copy. */
static RoomSensorConfig SeedConfig(uint32_t light_period)
{
    Config_Load();            /* blank -> NOT_FOUND, RAM defaults */
    Config_LoadDefaults();
    RoomSensorConfig c = *Config_Get();
    c.storage.light_period_ms = light_period;
    c.runtime.light_calibration_factor = 2.5f;
    c.storage.light_calibration_q16 = (uint32_t)(2.5f * 65536.0f);
    if (Config_ApplyPersistent(&c) != CONFIG_APPLY_OK)
        printf("  NOTE: seed config apply failed\n");
    return c;
}

static void SeedIdentity(void)
{
    FakeUniqueId_Set(s_uid);
    DeviceIdentity id;
    if (DeviceIdentity_Derive(&id))
        DeviceIdentity_Save(&id);
}

static bool SlotErased(uint8_t record_type, uint8_t slot)
{
    uint8_t *mem = (uint8_t *)FakeFlash_GetData();
    uint32_t page = (uint32_t)(record_type - RECORD_TYPE_CONFIG) * 2U + slot;
    for (uint32_t i = page * 2048U; i < (page + 1U) * 2048U; i++)
        if (mem[i] != 0xFF) return false;
    return true;
}

int main(void)
{
    printf("Config/Identity Mirror Ownership & Failed-Write Host Tests\n");
    fflush(stdout);

    /* ================= CONFIG mirror ownership ================= */
    printf("\n=== Config_EnsureRedundancy: VALID+ERASED -> DONE, health refreshed ===\n");
    {
        FakeFlash_Init(); Storage_Init();
        SeedConfig(900U);                 /* single copy: slot A valid, B erased */
        check(SlotErased(RECORD_TYPE_CONFIG, 1), "pre: slot B erased (single copy)");
        check(Config_GetStorageHealth() == STORAGE_HEALTH_DEGRADED,
              "pre: cached health DEGRADED (single copy)");
        /* Stale-cache regression: after the module-owned repair the cached
           health MUST be HEALTHY — exactly what App_UpdateHealth evaluates. */
        StorageRepairStatus repair = Config_EnsureRedundancy();
        check(repair == STORAGE_REPAIR_DONE, "repair DONE");
        check(Config_GetStorageHealth() == STORAGE_HEALTH_HEALTHY,
              "cached health HEALTHY (no stale DEGRADED)");
        check(Config_GetStorageStatus() == STORAGE_READ_OK, "read status OK");
        check(Storage_GetHealth(RECORD_TYPE_CONFIG) == STORAGE_HEALTH_HEALTHY,
              "physical mirror HEALTHY (A+B valid)");
        check(!SlotErased(RECORD_TYPE_CONFIG, 1), "slot B now valid");
    }

    printf("\n=== Config_EnsureRedundancy: VALID+VALID -> NOT_NEEDED, HEALTHY ===\n");
    {
        FakeFlash_Init(); Storage_Init();
        SeedConfig(901U);
        Config_EnsureRedundancy();        /* make it VALID+VALID */
        check(Config_GetStorageHealth() == STORAGE_HEALTH_HEALTHY, "pre: HEALTHY");
        StorageRepairStatus repair = Config_EnsureRedundancy();
        check(repair == STORAGE_REPAIR_NOT_NEEDED, "repair NOT_NEEDED");
        check(Config_GetStorageHealth() == STORAGE_HEALTH_HEALTHY, "health stays HEALTHY");
        check(Config_GetStorageStatus() == STORAGE_READ_OK, "read status OK");
    }

    printf("\n=== Config_EnsureRedundancy: VALID+IO -> REFUSED, DEGRADED_IO, no erase ===\n");
    {
        FakeFlash_Init(); Storage_Init();
        SeedConfig(902U);
        Config_EnsureRedundancy();        /* VALID+VALID */
        check(Config_GetStorageHealth() == STORAGE_HEALTH_HEALTHY, "pre: HEALTHY");
        FakeFlash_SetReadFail(true, 2048, 4096);   /* slot B unreadable */
        FakeFlash_ResetIoCounters();
        StorageRepairStatus repair = Config_EnsureRedundancy();
        check(repair == STORAGE_REPAIR_REFUSED, "repair REFUSED");
        check(Config_GetStorageHealth() == STORAGE_HEALTH_DEGRADED_IO,
              "cached health DEGRADED_IO (not HEALTHY)");
        check(Config_GetStorageStatus() == STORAGE_READ_OK,
              "read status OK (valid peer readable)");
        check(FakeFlash_GetEraseCount() == 0, "REFUSED: zero erases (cannot trust peer)");
        FakeFlash_SetReadFail(false, 0, 0);
    }

    printf("\n=== Config_EnsureRedundancy: ERASED+ERASED -> NOT_FOUND, HEALTHY ===\n");
    {
        FakeFlash_Init(); Storage_Init();
        Config_Load();   /* refresh module state from blank Flash fresh */
        check(Config_GetStorageStatus() == STORAGE_READ_NOT_FOUND, "pre: NOT_FOUND");
        StorageRepairStatus repair = Config_EnsureRedundancy();
        check(repair == STORAGE_REPAIR_NOT_FOUND, "repair NOT_FOUND (blank)");
        check(Config_GetStorageHealth() == STORAGE_HEALTH_HEALTHY,
              "blank mirrors are HEALTHY (both erased)");
        check(Config_GetStorageStatus() == STORAGE_READ_NOT_FOUND, "read status NOT_FOUND");
    }

    /* ================= IDENTITY mirror ownership ================= */
    printf("\n=== DeviceIdentity_EnsureRedundancy: VALID+ERASED -> DONE, health refreshed ===\n");
    {
        FakeFlash_Init(); Storage_Init();
        SeedIdentity();                   /* single copy: slot A valid, B erased */
        check(SlotErased(RECORD_TYPE_IDENTITY, 1), "pre: slot B erased (single copy)");
        check(DeviceIdentity_GetStorageHealth() == STORAGE_HEALTH_DEGRADED,
              "pre: cached health DEGRADED (single copy)");
        StorageRepairStatus repair = DeviceIdentity_EnsureRedundancy();
        check(repair == STORAGE_REPAIR_DONE, "repair DONE");
        check(DeviceIdentity_GetStorageHealth() == STORAGE_HEALTH_HEALTHY,
              "cached health HEALTHY (no stale DEGRADED)");
        check(DeviceIdentity_GetPersistenceStatus() == STORAGE_READ_OK, "persistence OK");
        check(Storage_GetHealth(RECORD_TYPE_IDENTITY) == STORAGE_HEALTH_HEALTHY,
              "physical identity mirror HEALTHY");
        check(!SlotErased(RECORD_TYPE_IDENTITY, 1), "identity slot B now valid");
    }

    printf("\n=== DeviceIdentity_EnsureRedundancy: VALID+VALID -> NOT_NEEDED ===\n");
    {
        FakeFlash_Init(); Storage_Init();
        SeedIdentity();
        DeviceIdentity_EnsureRedundancy();
        check(DeviceIdentity_GetStorageHealth() == STORAGE_HEALTH_HEALTHY, "pre: HEALTHY");
        StorageRepairStatus repair = DeviceIdentity_EnsureRedundancy();
        check(repair == STORAGE_REPAIR_NOT_NEEDED, "repair NOT_NEEDED");
        check(DeviceIdentity_GetStorageHealth() == STORAGE_HEALTH_HEALTHY, "health stays HEALTHY");
    }

    printf("\n=== DeviceIdentity_EnsureRedundancy: VALID+IO -> REFUSED, DEGRADED_IO ===\n");
    {
        FakeFlash_Init(); Storage_Init();
        SeedIdentity();
        DeviceIdentity_EnsureRedundancy();
        FakeFlash_SetReadFail(true, 6144, 8192);   /* identity slot B unreadable */
        StorageRepairStatus repair = DeviceIdentity_EnsureRedundancy();
        check(repair == STORAGE_REPAIR_REFUSED, "repair REFUSED");
        check(DeviceIdentity_GetStorageHealth() == STORAGE_HEALTH_DEGRADED_IO,
              "cached identity health DEGRADED_IO");
        check(DeviceIdentity_GetPersistenceStatus() == STORAGE_READ_OK,
              "persistence OK (valid peer readable)");
        FakeFlash_SetReadFail(false, 0, 0);
    }

    /* ================= Config VERIFY_FAILED transaction ================= */
    printf("\n=== Config failed write (VERIFY_FAILED) preserves OLD ===\n");
    {
        FakeFlash_Init(); Storage_Init();
        RoomSensorConfig old = SeedConfig(1000U);
        Config_EnsureRedundancy();        /* redundant OLD before injection */
        check(Config_GetLastWriteStatus() == STORAGE_WRITE_OK, "pre: last write OK");

        RoomSensorConfig new_cfg = *Config_Get();
        new_cfg.storage.light_period_ms = 4321U;
        new_cfg.runtime.light_calibration_factor = 9.0f;

        /* New differs from OLD. */
        check(new_cfg.storage.light_period_ms != old.storage.light_period_ms,
              "candidate NEW differs from OLD");

        FakeFlash_SetVerifyFail(true);    /* fail readback on the NEW write */
        ConfigApplyStatus app = Config_ApplyPersistent(&new_cfg);
        FakeFlash_SetVerifyFail(false);

        check(app == CONFIG_APPLY_PERSIST_FAILED, "apply returns PERSIST_FAILED");
        check(Config_GetLastWriteStatus() == STORAGE_WRITE_VERIFY_FAILED,
              "last_write_status == VERIFY_FAILED");
        check(memcmp(Config_Get(), &old, sizeof(RoomSensorConfig)) == 0,
              "runtime Config == OLD (unchanged)");
        StoragePayload p;
        StorageReadStatus rs = Storage_Read(RECORD_TYPE_CONFIG, &p);
        check(rs == STORAGE_READ_OK, "Storage_Read(CONFIG) == OK");
        check(p.size == sizeof(ConfigStorageV1) &&
              memcmp(p.data, &old.storage, sizeof(ConfigStorageV1)) == 0,
              "persisted payload == OLD");
        check(Config_GetStorageStatus() == STORAGE_READ_OK, "current read status OK");
        check(Config_GetStorageHealth() == STORAGE_HEALTH_DEGRADED,
              "health DEGRADED (new leg partially written, old valid)");

        /* simulate reboot: Config_Load restores OLD */
        Config_LoadDefaults();
        check(Config_Load(), "reboot Config_Load() true");
        check(Config_Get()->storage.light_period_ms == old.storage.light_period_ms,
              "reboot loads OLD (light_period)");
        check(fabsf(Config_Get()->runtime.light_calibration_factor - 2.5f) < 0.001f,
              "reboot loads OLD (calibration)");
    }

    /* ================= Config IO transaction ================= */
    printf("\n=== Config failed write (IO_ERROR) preserves OLD ===\n");
    {
        FakeFlash_Init(); Storage_Init();
        RoomSensorConfig old = SeedConfig(1001U);
        Config_EnsureRedundancy();
        RoomSensorConfig new_cfg = *Config_Get();
        new_cfg.storage.light_period_ms = 5555U;

        FakeFlash_SetWriteFail(true);     /* erase/program HAL failure */
        ConfigApplyStatus app = Config_ApplyPersistent(&new_cfg);
        FakeFlash_SetWriteFail(false);

        check(app == CONFIG_APPLY_PERSIST_FAILED, "apply returns PERSIST_FAILED");
        check(Config_GetLastWriteStatus() == STORAGE_WRITE_IO_ERROR,
              "last_write_status == IO_ERROR");
        check(memcmp(Config_Get(), &old, sizeof(RoomSensorConfig)) == 0,
              "runtime Config == OLD");
        StoragePayload p;
        check(Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_OK,
              "old persisted record still readable (OK)");
        check(Config_GetStorageStatus() == STORAGE_READ_OK,
              "current read status reflects actual Flash (OK, NOT forced CORRUPT)");

        Config_LoadDefaults();
        check(Config_Load(), "reboot Config_Load() true");
        check(Config_Get()->storage.light_period_ms == old.storage.light_period_ms,
              "reboot loads OLD");
    }

    /* ================= Config UNSAFE_STATE transaction ================= */
    printf("\n=== Config unsafe state -> UNSAFE_STATE, no erase, not misreported as IO ===\n");
    {
        FakeFlash_Init(); Storage_Init();
        SeedConfig(1002U);
        /* Corrupt slot A header and erase/keep-empty slot B -> no VALID record. */
        FakeFlash_Corrupt(0, 24);
        StoragePayload p;
        check(Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_CORRUPT,
              "pre: read status CORRUPT (not IO)");
        RoomSensorConfig old = *Config_Get();

        FakeFlash_ResetIoCounters();
        check(Config_Save() == false, "Config_Save returns false");
        check(Config_GetLastWriteStatus() == STORAGE_WRITE_UNSAFE_STATE,
              "last_write_status == UNSAFE_STATE (NOT IO_ERROR)");
        check(FakeFlash_GetEraseCount() == 0, "no erase on unsafe pair");
        check(memcmp(Config_Get(), &old, sizeof(RoomSensorConfig)) == 0,
              "runtime Config unchanged");
        check(Storage_Read(RECORD_TYPE_CONFIG, &p) == STORAGE_READ_CORRUPT,
              "record remains CORRUPT (no VALID)");
        check(Config_GetStorageStatus() == STORAGE_READ_CORRUPT,
              "read status CORRUPT, not misreported as physical IO");
    }

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}