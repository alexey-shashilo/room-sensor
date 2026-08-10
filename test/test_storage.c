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

    /* --- UUID byte positions (UUIDv8) --- */
    printf("\n=== UUID byte positions (UUIDv8) ===\n");
    {
        DeviceIdentity id;
        FakeUniqueId_Set(default_uid);
        DeviceIdentity_Derive(&id);

        test("UUID version byte[6] top nibble = 0x80 (UUIDv8)",
             (id.device_uuid[6] & 0xF0U) == 0x80U);
        test("UUID variant byte[8] top bits = 0x80 (RFC 4122)",
             (id.device_uuid[8] & 0xC0U) == 0x80U);
        test("uuid not all zero",
             memcmp(id.device_uuid, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) != 0);
        test("uuid not all FF",
             memcmp(id.device_uuid, "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 16) != 0);
    }

    /* --- UID read failure --- */
    printf("\n=== UID read failure ===\n");
    {
        FakeUniqueId_SetFail(true);
        DeviceIdentity id;
        bool ok = DeviceIdentity_Derive(&id);
        test("derive fails when UID unavailable", !ok);
        test("uuid is 16 zero bytes on failure",
             memcmp(id.device_uuid, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0);
        test("validation rejects failed derive", !DeviceIdentity_Validate(&id));
    }

    /* --- Determinism --- */
    printf("\n=== Determinism ===\n");
    {
        FakeUniqueId_SetFail(false);
        FakeUniqueId_Set(default_uid);
        DeviceIdentity id1, id2;
        DeviceIdentity_Derive(&id1);
        DeviceIdentity_Derive(&id2);
        test("deterministic: same uuid twice", memcmp(id1.device_uuid, id2.device_uuid, 16) == 0);
        test("deterministic: same hardware_rev", id1.hardware_revision == id2.hardware_revision);
    }

    /* --- Different UID produces different UUID --- */
    printf("\n=== Different UID ===\n");
    {
        uint8_t uid_a[12] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B};
        uint8_t uid_b[12] = {0xFF,0xFE,0xFD,0xFC,0xFB,0xFA,0xF9,0xF8,0xF7,0xF6,0xF5,0xF4};

        DeviceIdentity id_a, id_b;
        FakeUniqueId_Set(uid_a); DeviceIdentity_Derive(&id_a);
        FakeUniqueId_Set(uid_b); DeviceIdentity_Derive(&id_b);
        printf("    id_a[0..7]= %02x%02x%02x%02x%02x%02x%02x%02x  id_b[0..7]= %02x%02x%02x%02x%02x%02x%02x%02x\n",
               id_a.device_uuid[0],id_a.device_uuid[1],id_a.device_uuid[2],id_a.device_uuid[3],
               id_a.device_uuid[4],id_a.device_uuid[5],id_a.device_uuid[6],id_a.device_uuid[7],
               id_b.device_uuid[0],id_b.device_uuid[1],id_b.device_uuid[2],id_b.device_uuid[3],
               id_b.device_uuid[4],id_b.device_uuid[5],id_b.device_uuid[6],id_b.device_uuid[7]);
        test("different uid -> different uuid",
             memcmp(id_a.device_uuid, id_b.device_uuid, 16) != 0);
    }

    /* --- Time independence --- */
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

    /* --- Identity validation --- */
    printf("\n=== Identity validation ===\n");
    {
        DeviceIdentity valid, bad;
        FakeUniqueId_SetFail(false);
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

    /* --- Identity persistence and load round-trip --- */
    printf("\n=== Identity persist/load ===\n");
    {
        FakeFlash_Init();
        FakeUniqueId_Set(default_uid);
        Storage_Init();

        DeviceIdentity id1, id2;
        DeviceIdentity_Generate(&id1);
        test("identity loaded after generate", DeviceIdentity_Load(&id2));
        test("same uuid after load", memcmp(id1.device_uuid, id2.device_uuid, 16) == 0);
        test("loaded id passes validation", DeviceIdentity_Validate(&id2));
    }

    /* --- Persistence failure still yields valid derive --- */
    printf("\n=== Persistence failure ===\n");
    {
        FakeFlash_Init();
        FakeUniqueId_Set(default_uid);
        Storage_Init();
        FakeFlash_SetWriteFail(true);

        DeviceIdentity id;
        bool derived = DeviceIdentity_Derive(&id);
        bool generated = DeviceIdentity_Generate(&id);

        test("derive succeeds when flash fails", derived);
        test("generate returns false on flash failure", !generated);
        test("derived id is still valid", DeviceIdentity_Validate(&id));
    }

    /* Schema version exact match */
    printf("\n=== Schema version validation ===\n");
    {
        DeviceIdentity id;
        FakeUniqueId_Set(default_uid);
        DeviceIdentity_Derive(&id);

        /* Storage reject: schema version 0 */
        uint8_t raw0[24];
        memset(raw0, 0, sizeof(raw0));
        /* raw0[0..3] = schema_version = 0 (already zero) */
        memcpy(raw0 + 4, id.device_uuid, 16);
        /* raw0[20..23] = hardware_revision = 1 */
        raw0[20] = 1;

        Storage_Write(RECORD_TYPE_IDENTITY, raw0, sizeof(raw0));
        DeviceIdentity loaded;
        test("schema version 0 -> load fails", !DeviceIdentity_Load(&loaded));

        /* schema version 99 (future) */
        uint8_t raw99[24];
        memset(raw99, 0, sizeof(raw99));
        raw99[0] = 99;
        memcpy(raw99 + 4, id.device_uuid, 16);
        raw99[20] = 1;
        Storage_Write(RECORD_TYPE_IDENTITY, raw99, sizeof(raw99));
        test("future schema version rejected", !DeviceIdentity_Load(&loaded));
    }

    /* Wrong payload size */
    printf("\n=== Wrong payload size ===\n");
    {
        FakeFlash_Init();
        Storage_Init();
        uint8_t bad[10];
        memset(bad, 0xAA, sizeof(bad));
        Storage_Write(RECORD_TYPE_IDENTITY, bad, sizeof(bad));
        DeviceIdentity id;
        test("wrong payload size rejected", !DeviceIdentity_Load(&id));
    }

    /* --- Corrupted identity rejected --- */
    printf("\n=== Corrupted identity ===\n");
    {
        FakeFlash_Init();
        FakeUniqueId_Set(default_uid);
        Storage_Init();

        DeviceIdentity id;
        DeviceIdentity_Generate(&id);
        FakeFlash_Corrupt(4096, 24);
        test("corrupted identity rejected", !DeviceIdentity_Load(&id));
    }

    /* --- Config defaults --- */
    printf("\n=== Config defaults ===\n");
    {
        FakeFlash_Init();
        Storage_Init();
        Config_LoadDefaults();
        test("defaults set", Config_Get()->storage.light_period_ms == 500);
        test("calibration factor FP", fabsf(Config_Get()->runtime.light_calibration_factor - 1.0f) < 0.001f);
    }

    /* --- Config save/load round-trip --- */
    printf("\n=== Config save/load ===\n");
    {
        FakeFlash_Init();
        Storage_Init();
        Config_LoadDefaults();
        RoomSensorConfig *w = (RoomSensorConfig *)(void *)Config_Get();
        w->storage.light_period_ms = 250;
        w->runtime.light_calibration_factor = 2.5f;
        test("config saved", Config_Save());

        Config_LoadDefaults();
        test("config loaded", Config_Load());
        test("period persisted", Config_Get()->storage.light_period_ms == 250);
        test("calib persisted", fabsf(Config_Get()->runtime.light_calibration_factor - 2.5f) < 0.001f);
    }

    /* --- Config version mismatch rejected --- */
    printf("\n=== Config version mismatch ===\n");
    {
        ConfigStorageV1 bad;
        memcpy(&bad, &Config_Get()->storage, sizeof(bad));
        bad.version = 99;
        test("wrong version rejected", Config_Validate(&bad) == false);
    }

    /* --- Config invalid values rejected --- */
    printf("\n=== Config invalid values ===\n");
    {
        ConfigStorageV1 bad;
        memset(&bad, 0, sizeof(bad));
        bad.version = 1;
        bad.light_period_ms = 500;
        bad.display_period_ms = 500;
        bad.diagnostics_period_ms = 10000;
        bad.retry_period_ms = 5000;
        bad.telemetry_period_ms = 5000;
        bad.light_calibration_q16 = 0;
        test("zero calibration q16 rejected", Config_Validate(&bad) == false);

        bad.light_calibration_q16 = 1;
        test("tiny calibration rejected", Config_Validate(&bad) == false);
    }

    /* --- Reset config does not change identity --- */
    printf("\n=== Reset config preserves identity ===\n");
    {
        FakeFlash_Init();
        FakeUniqueId_Set(default_uid);
        Storage_Init();

        DeviceIdentity id1, id2;
        DeviceIdentity_Generate(&id1);

        Config_LoadDefaults();
        Config_Save();
        Config_ResetToDefaults();

        test("identity still valid after config reset", DeviceIdentity_Load(&id2));
        test("identity unchanged", memcmp(id1.device_uuid, id2.device_uuid, 16) == 0);
    }

    /* Platform_GetUniqueId contract */
    printf("\n=== Platform_GetUniqueId contract ===\n");
    {
        uint8_t buf[12];
        test("NULL out returns false", !Platform_GetUniqueId(NULL, 12));
        test("size < 12 returns false", !Platform_GetUniqueId(buf, 11));
        test("size >= 12 returns true", Platform_GetUniqueId(buf, 12));
        test("size > 12 still works", Platform_GetUniqueId(buf, 100));
        test("writes expected bytes", buf[0] == 0xAA && buf[11] == 0xEF);

        FakeUniqueId_SetFail(false);
    }

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);

    return s_fail > 0 ? 1 : 0;
}