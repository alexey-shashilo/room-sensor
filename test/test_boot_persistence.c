#include <stdio.h>
#include <string.h>

#include "config.h"
#include "device_identity.h"
#include "storage.h"
#include "fake_flash.h"
#include "fake_unique_id.h"
#include "fake_platform_time.h"

/* Boot-persistence policy tests.

   The Boot persistence decision (persist only on NOT_FOUND) is enforced in
   app.c's lifecycle, gated on Config_GetStorageStatus() and
   DeviceIdentity_GetLoadStatus(). These tests verify those status functions
   distinguish each state correctly and that applying the documented policy
   (mirrored below via the same status APIs) yields the required write counts:
     NOT_FOUND -> persiast once; CORRUPT/IO -> zero writes (evidence preserved). */

static int s_pass = 0, s_fail = 0, s_case = 0;
static void check(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

/* Write a valid config record to Flash so a subsequent Config_Load sees OK. */
static void PreStoreValidConfig(void)
{
    Config_Load();            /* blank -> NOT_FOUND, RAM defaults */
    Config_LoadDefaults();
    Config_Save();            /* persist defaults -> valid stored record */
    Config_Load();            /* now OK */
}

int main(void)
{
    printf("Boot Persistence Policy Host Tests\n");
    fflush(stdout);

    /* ================= CONFIG ================= */
    printf("\n=== Config boot policy ===\n");

    /* NOT_FOUND: persist defaults once (1 storage write). */
    FakeFlash_Init();
    Storage_Init();
    {
        int loaded_then_gs = Config_Load();
        StorageReadStatus st = Config_GetStorageStatus();
        check(!loaded_then_gs && st == STORAGE_READ_NOT_FOUND,
              "NOT_FOUND: Config_Load false, status NOT_FOUND");
        FakeFlash_ResetIoCounters();
        Config_LoadDefaults();
        bool saved = Config_Save();
        check(saved && FakeFlash_GetWriteCount() == 1,
              "NOT_FOUND: persist defaults exactly once (1 write)");
        check(Config_Load(), "NOT_FOUND: subsequent load uses persisted");
    }

    /* OK: load persisted, zero boot writes. */
    FakeFlash_Init();
    Storage_Init();
    PreStoreValidConfig();
    {
        FakeFlash_ResetIoCounters();
        check(Config_Load(), "OK: load persisted config");
        check(Config_GetStorageStatus() == STORAGE_READ_OK, "OK: status OK");
        check(FakeFlash_GetWriteCount() == 0, "OK: zero boot writes");
    }

    /* CORRUPT: defaults runtime, zero writes (evidence preserved). */
    FakeFlash_Init();
    Storage_Init();
    PreStoreValidConfig();
    FakeFlash_Corrupt(0, 24);   /* corrupt config slot A header */
    {
        bool ok = Config_Load();
        StorageReadStatus st = Config_GetStorageStatus();
        check(!ok && st == STORAGE_READ_CORRUPT, "CORRUPT: status CORRUPT");
        FakeFlash_ResetIoCounters();
        Config_LoadDefaults();   /* safe RAM defaults only */
        check(FakeFlash_GetWriteCount() == 0, "CORRUPT: zero boot writes");
    }

    /* IO_ERROR: defaults runtime, zero writes. */
    FakeFlash_Init();
    Storage_Init();
    PreStoreValidConfig();
    {
        FakeFlash_SetReadFail(true, 0, 2048);   /* config slot A unreadable */
        bool ok = Config_Load();
        StorageReadStatus st = Config_GetStorageStatus();
        FakeFlash_SetReadFail(false, 0, 0);
        check(!ok && st == STORAGE_READ_IO_ERROR, "IO_ERROR: status IO_ERROR");
        FakeFlash_ResetIoCounters();
        Config_LoadDefaults();
        check(FakeFlash_GetWriteCount() == 0, "IO_ERROR: zero boot writes");
    }

    /* ================= IDENTITY ================= */
    printf("\n=== Identity boot policy ===\n");

    /* NOT_FOUND: derive + exactly one save. */
    FakeFlash_Init();
    Storage_Init();
    {
        DeviceIdentity id;
        bool ok = DeviceIdentity_Load(&id);
        StorageReadStatus st = DeviceIdentity_GetLoadStatus();
        check(!ok && st == STORAGE_READ_NOT_FOUND, "NOT_FOUND: status NOT_FOUND");
        FakeFlash_ResetIoCounters();
        bool derived = DeviceIdentity_Derive(&id) && DeviceIdentity_Save(&id);
        check(derived && FakeFlash_GetWriteCount() == 1,
              "NOT_FOUND: derive + one save (1 write)");
        DeviceIdentity rt;
        check(DeviceIdentity_Load(&rt), "NOT_FOUND: subsequent load OK");
    }

    /* CORRUPT: derive runtime usable, zero saves. */
    FakeFlash_Init();
    Storage_Init();
    {
        DeviceIdentity seed;
        DeviceIdentity_Derive(&seed);
        DeviceIdentity_Save(&seed);
        FakeFlash_Corrupt(4096, 24);   /* corrupt identity slot A */
        DeviceIdentity id;
        bool ok = DeviceIdentity_Load(&id);
        StorageReadStatus st = DeviceIdentity_GetLoadStatus();
        check(!ok && st == STORAGE_READ_CORRUPT, "CORRUPT: status CORRUPT");
        FakeFlash_ResetIoCounters();
        bool derived = DeviceIdentity_Derive(&id);
        check(derived, "CORRUPT: runtime identity derived (usable in RAM)");
        check(FakeFlash_GetWriteCount() == 0, "CORRUPT: zero saves (evidence preserved)");
    }

    /* IO_ERROR: derive runtime usable, zero saves. */
    FakeFlash_Init();
    Storage_Init();
    {
        DeviceIdentity seed;
        DeviceIdentity_Derive(&seed);
        DeviceIdentity_Save(&seed);
        FakeFlash_SetReadFail(true, 4096, 8192);   /* identity region unreadable */
        DeviceIdentity id;
        bool ok = DeviceIdentity_Load(&id);
        StorageReadStatus st = DeviceIdentity_GetLoadStatus();
        FakeFlash_SetReadFail(false, 0, 0);
        check(!ok && st == STORAGE_READ_IO_ERROR, "IO_ERROR: status IO_ERROR");
        FakeFlash_ResetIoCounters();
        bool derived = DeviceIdentity_Derive(&id);
        check(derived, "IO_ERROR: runtime identity derived");
        check(FakeFlash_GetWriteCount() == 0, "IO_ERROR: zero saves");
    }

    /* Same hardware UID -> deterministic identity across simulated boots. */
    printf("\n=== Identity determinism across boots ===\n");
    {
        uint8_t uid[16];
        memset(uid, 0x42, sizeof(uid));
        DeviceIdentity a, b;

        FakeFlash_Init();
        Storage_Init();
        FakeUniqueId_Set(uid);
        /* first boot: derived (no persisted) */
        DeviceIdentity_Load(&a);
        DeviceIdentity_Derive(&a);

        FakeFlash_Init();      /* simulated reboot: storage wiped */
        Storage_Init();
        FakeUniqueId_Set(uid); /* same hardware UID */
        DeviceIdentity_Load(&b);
        DeviceIdentity_Derive(&b);

        check(memcmp(a.device_uuid, b.device_uuid, 16) == 0,
              "same UID -> same derived identity across boots");
    }

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}