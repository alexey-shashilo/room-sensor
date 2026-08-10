#include <stdio.h>
#include <math.h>
#include <string.h>

#include "storage.h"
#include "config.h"
#include "device_identity.h"
#include "platform_flash.h"
#include "platform_unique_id.h"
#include "fake_flash.h"
#include "fake_unique_id.h"
#include "fake_platform_time.h"

static int s_pass = 0;
static int s_fail = 0;
static int s_case = 0;

static void test(const char *name, int cond)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

int main(void)
{
    printf("Storage / Config / Identity Host Tests\n");
    fflush(stdout);

    uint8_t default_uid[12] = {0xAA, 0xBB, 0xCC, 0xDD, 0x01, 0x02, 0x03, 0x04, 0xFE, 0xED, 0xBE, 0xEF};

    /* ================================================================
       UUID byte positions (UUIDv8)
       ================================================================ */
    printf("\n=== UUID byte positions (UUIDv8) ===\n");
    {
        FakeUniqueId_Set(default_uid);
        DeviceIdentity id;
        DeviceIdentity_Derive(&id);

        test("version byte[6] top nibble = 0x80",
             (id.device_uuid[6] & 0xF0U) == 0x80U);
        test("variant byte[8] top bits = 0x80",
             (id.device_uuid[8] & 0xC0U) == 0x80U);
        test("uuid not all zero",
             memcmp(id.device_uuid, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) != 0);
        test("uuid not all FF",
             memcmp(id.device_uuid, "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 16) != 0);
    }

    /* ================================================================
       UID read failure
       ================================================================ */
    printf("\n=== UID read failure ===\n");
    {
        FakeUniqueId_SetFail(true);
        DeviceIdentity id;
        bool ok = DeviceIdentity_Derive(&id);
        test("derive fails when UID unavailable", !ok);
        test("uuid is 16 zero bytes on failure",
             memcmp(id.device_uuid, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0);
        test("validation rejects failed derive", !DeviceIdentity_Validate(&id));
        FakeUniqueId_SetFail(false);
    }

    /* ================================================================
       Determinism
       ================================================================ */
    printf("\n=== Determinism ===\n");
    {
        FakeUniqueId_Set(default_uid);
        DeviceIdentity id1, id2;
        DeviceIdentity_Derive(&id1);
        DeviceIdentity_Derive(&id2);
        test("same uuid twice", memcmp(id1.device_uuid, id2.device_uuid, 16) == 0);
        test("same hardware_rev", id1.hardware_revision == id2.hardware_revision);
    }

    /* ================================================================
       Different UID
       ================================================================ */
    printf("\n=== Different UID ===\n");
    {
        uint8_t uid_a[12] = {0,1,2,3,4,5,6,7,8,9,10,11};
        uint8_t uid_b[12] = {0xFF,0xFE,0xFD,0xFC,0xFB,0xFA,0xF9,0xF8,0xF7,0xF6,0xF5,0xF4};
        DeviceIdentity id_a, id_b;
        FakeUniqueId_Set(uid_a); DeviceIdentity_Derive(&id_a);
        FakeUniqueId_Set(uid_b); DeviceIdentity_Derive(&id_b);
        test("different uid -> different uuid",
             memcmp(id_a.device_uuid, id_b.device_uuid, 16) != 0);
    }

    /* ================================================================
       Time independence
       ================================================================ */
    printf("\n=== Time independence ===\n");
    {
        DeviceIdentity id_t1, id_t2;
        FakeUniqueId_Set(default_uid);
        FakePlatform_SetTick(0);
        DeviceIdentity_Derive(&id_t1);
        FakePlatform_SetTick(999999);
        DeviceIdentity_Derive(&id_t2);
        test("tick does not affect uuid",
             memcmp(id_t1.device_uuid, id_t2.device_uuid, 16) == 0);
    }

    /* ================================================================
       Identity validation
       ================================================================ */
    printf("\n=== Identity validation ===\n");
    {
        DeviceIdentity valid, bad;
        FakeUniqueId_Set(default_uid);
        DeviceIdentity_Derive(&valid);
        test("valid identity passes validation", DeviceIdentity_Validate(&valid));

        bad = valid;
        memset(bad.device_uuid, 0, 16);
        test("all-zero uuid rejected", !DeviceIdentity_Validate(&bad));

        bad = valid;
        memset(bad.device_uuid, 0xFF, 16);
        test("all-FF uuid rejected", !DeviceIdentity_Validate(&bad));

        bad = valid;
        bad.device_uuid[6] &= 0x0F;
        test("wrong version bits rejected", !DeviceIdentity_Validate(&bad));

        bad = valid;
        bad.device_uuid[8] &= 0x3F;
        test("wrong variant bits rejected", !DeviceIdentity_Validate(&bad));

        bad = valid;
        bad.hardware_revision = 0;
        test("zero hardware_rev rejected", !DeviceIdentity_Validate(&bad));
    }

    /* ================================================================
       UID call count: identity loaded from Flash
       ================================================================ */
    printf("\n=== UID call count: loaded identity ===\n");
    {
        FakeFlash_Init();
        FakeUniqueId_Set(default_uid);
        Storage_Init();

        DeviceIdentity id;
        DeviceIdentity_Derive(&id);
        DeviceIdentity_Save(&id);

        int before = FakeUniqueId_GetCallCount();
        DeviceIdentity loaded;
        test("load from flash succeeds", DeviceIdentity_Load(&loaded));
        int after = FakeUniqueId_GetCallCount();
        test("UID calls == 0 when loading from Flash", (after - before) == 0);
    }

    /* ================================================================
       UID call count: fresh derivation
       ================================================================ */
    printf("\n=== UID call count: fresh derive ===\n");
    {
        int before = FakeUniqueId_GetCallCount();
        DeviceIdentity id;
        DeviceIdentity_Derive(&id);
        int after = FakeUniqueId_GetCallCount();
        test("UID calls == 1 during derive", (after - before) == 1);
    }

    /* ================================================================
       Save semantics: const input, no re-derive
       ================================================================ */
    printf("\n=== Save semantics ===\n");
    {
        FakeFlash_Init();
        FakeUniqueId_Set(default_uid);
        Storage_Init();

        DeviceIdentity id;
        DeviceIdentity_Derive(&id);
        uint8_t before[sizeof(id)];
        memcpy(before, &id, sizeof(id));

        int uid_before = FakeUniqueId_GetCallCount();
        test("save succeeds", DeviceIdentity_Save(&id));
        int uid_after = FakeUniqueId_GetCallCount();
        test("save does NOT call Platform_GetUniqueId", (uid_after - uid_before) == 0);

        test("identity byte-identical after save",
             memcmp(before, &id, sizeof(id)) == 0);
        test("identity still validates after save", DeviceIdentity_Validate(&id));
    }

    /* ================================================================
       Save invalid identity -> rejected
       ================================================================ */
    printf("\n=== Save invalid identity rejected ===\n");
    {
        DeviceIdentity bad;
        memset(&bad, 0, sizeof(bad));
        test("save NULL rejected", !DeviceIdentity_Save(NULL));
        test("save all-zero rejected", !DeviceIdentity_Save(&bad));
    }

    /* ================================================================
       Derive + save fail -> runtime identity still valid
       ================================================================ */
    printf("\n=== Derive + save fail ===\n");
    {
        FakeFlash_Init();
        FakeUniqueId_Set(default_uid);
        Storage_Init();

        DeviceIdentity derived;
        test("derive succeeds", DeviceIdentity_Derive(&derived));

        FakeFlash_SetWriteFail(true);
        uint8_t before[sizeof(derived)];
        memcpy(before, &derived, sizeof(derived));

        test("save returns false on write fail", !DeviceIdentity_Save(&derived));
        test("identity byte-identical after failed save",
             memcmp(before, &derived, sizeof(derived)) == 0);
        test("identity still valid after failed save", DeviceIdentity_Validate(&derived));
    }

    /* ================================================================
       Persistence failure still yields valid derive
       ================================================================ */
    printf("\n=== App boot simulation: derive + save fail ===\n");
    {
        FakeFlash_Init();
        FakeUniqueId_Set(default_uid);
        Storage_Init();
        FakeFlash_SetWriteFail(true);

        DeviceIdentity derived;
        bool derived_ok = DeviceIdentity_Derive(&derived);
        bool saved_ok = DeviceIdentity_Save(&derived);

        test("derive succeeds", derived_ok);
        test("save returns false on flash failure", !saved_ok);
        test("derived id is still valid", DeviceIdentity_Validate(&derived));
        test("derived uuid non-zero",
             memcmp(derived.device_uuid, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) != 0);
    }

    /* ================================================================
       Schema version exact match
       ================================================================ */
    printf("\n=== Schema version validation ===\n");
    {
        FakeFlash_Init();
        FakeUniqueId_Set(default_uid);
        Storage_Init();

        DeviceIdentity id;
        DeviceIdentity_Derive(&id);

        uint8_t raw[24];
        memset(raw, 0, sizeof(raw));
        memcpy(raw + 4, id.device_uuid, 16);
        raw[20] = 1;

        raw[0] = 0; Storage_Write(RECORD_TYPE_IDENTITY, raw, sizeof(raw));
        test("schema version 0 -> load fails", !DeviceIdentity_Load(&id));

        raw[0] = 99; Storage_Write(RECORD_TYPE_IDENTITY, raw, sizeof(raw));
        test("future schema version rejected", !DeviceIdentity_Load(&id));
    }

    /* ================================================================
       Wrong payload size
       ================================================================ */
    printf("\n=== Wrong payload size ===\n");
    {
        FakeFlash_Init();
        Storage_Init();
        uint8_t bad[10];
        memset(bad, 0xAA, sizeof(bad));
        Storage_Write(RECORD_TYPE_IDENTITY, bad, sizeof(bad));
        DeviceIdentity dummy;
        test("wrong payload size rejected", !DeviceIdentity_Load(&dummy));
    }

    /* ================================================================
       Corrupted identity
       ================================================================ */
    printf("\n=== Corrupted identity ===\n");
    {
        FakeFlash_Init();
        FakeUniqueId_Set(default_uid);
        Storage_Init();

        DeviceIdentity id;
        DeviceIdentity_Derive(&id);
        DeviceIdentity_Save(&id);

        FakeFlash_Corrupt(8192, 24);
        test("corrupted identity rejected", !DeviceIdentity_Load(&id));
    }

    /* ================================================================
       Config defaults
       ================================================================ */
    printf("\n=== Config defaults ===\n");
    {
        FakeFlash_Init();
        Storage_Init();
        Config_LoadDefaults();
        test("defaults set", Config_Get()->storage.light_period_ms == 500);
        test("calibration factor FP", fabsf(Config_Get()->runtime.light_calibration_factor - 1.0f) < 0.001f);
    }

    /* ================================================================
       Config save/load round-trip
       ================================================================ */
    printf("\n=== Config save/load ===\n");
    {
        FakeFlash_Init(); Storage_Init(); Config_LoadDefaults();
        RoomSensorConfig *w = (RoomSensorConfig *)(void *)Config_Get();
        w->storage.light_period_ms = 250;
        w->runtime.light_calibration_factor = 2.5f;
        test("config saved", Config_Save());
        Config_LoadDefaults();
        test("config loaded", Config_Load());
        test("period persisted", Config_Get()->storage.light_period_ms == 250);
        test("calib persisted", fabsf(Config_Get()->runtime.light_calibration_factor - 2.5f) < 0.001f);
    }

    /* ================================================================
       Config version / value validation
       ================================================================ */
    printf("\n=== Config validation ===\n");
    {
        ConfigStorageV1 bad;
        memset(&bad, 0, sizeof(bad)); bad.version = 99;
        test("wrong version rejected", !Config_Validate(&bad));

        memset(&bad, 0, sizeof(bad)); bad.version = 1;
        bad.light_period_ms = 500; bad.display_period_ms = 500;
        bad.diagnostics_period_ms = 10000; bad.retry_period_ms = 5000;
        bad.telemetry_period_ms = 5000;
        bad.light_calibration_q16 = 0;
        test("zero calibration q16 rejected", !Config_Validate(&bad));

        bad.light_calibration_q16 = 1;
        test("tiny calibration rejected", !Config_Validate(&bad));
    }

    /* ================================================================
       Reset config preserves identity
       ================================================================ */
    printf("\n=== Reset config preserves identity ===\n");
    {
        FakeFlash_Init(); FakeUniqueId_Set(default_uid); Storage_Init();

        DeviceIdentity id1, id2;
        DeviceIdentity_Derive(&id1);
        DeviceIdentity_Save(&id1);

        Config_LoadDefaults(); Config_Save(); Config_ResetToDefaults();

        test("identity still valid after config reset", DeviceIdentity_Load(&id2));
        test("identity unchanged", memcmp(id1.device_uuid, id2.device_uuid, 16) == 0);
    }

    /* ================================================================
       Platform_GetUniqueId contract
       ================================================================ */
    printf("\n=== Platform_GetUniqueId contract ===\n");
    {
        uint8_t buf[12];
        memset(buf, 0, sizeof(buf));
        test("NULL out returns false", !Platform_GetUniqueId(NULL, 12));
        test("size < 12 returns false", !Platform_GetUniqueId(buf, 11));
        test("size >= 12 returns true", Platform_GetUniqueId(buf, 12));
        test("writes expected bytes", buf[0] == 0xAA && buf[11] == 0xEF);
    }

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);

    return s_fail > 0 ? 1 : 0;
}