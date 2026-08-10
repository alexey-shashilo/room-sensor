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

    /* UUID byte positions */
    printf("\n=== UUID byte positions ===\n");
    {
        DeviceIdentity id;
        FakeUniqueId_Set(default_uid);
        DeviceIdentity_Generate(&id);

        test("UUID version byte[6] top nibble = 0x40",
             (id.device_uuid[6] & 0xF0U) == 0x40U);
        test("UUID variant byte[8] top bits = 0x80",
             (id.device_uuid[8] & 0xC0U) == 0x80U);
        test("uuid not all zero",
             memcmp(id.device_uuid, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) != 0);
        test("uuid not all FF",
             memcmp(id.device_uuid, "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 16) != 0);

        char short_id[16];
        DeviceIdentity_GetShortId(&id, short_id, sizeof(short_id));
        test("short id non-empty", strlen(short_id) > 0);
    }

    /* Determinism */
    printf("\n=== Determinism ===\n");
    {
        FakeUniqueId_Set(default_uid);
        DeviceIdentity id1, id2;
        DeviceIdentity_Generate(&id1);
        DeviceIdentity_Generate(&id2);
        test("deterministic: same uuid twice", memcmp(id1.device_uuid, id2.device_uuid, 16) == 0);
        test("deterministic: same hardware_rev", id1.hardware_revision == id2.hardware_revision);
        test("deterministic: same schema version", id1.identity_schema_version == id2.identity_schema_version);
    }

    /* Different UID produces different UUID */
    printf("\n=== Different UID ===\n");
    {
        uint8_t uid_a[12] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B};
        uint8_t uid_b[12] = {0xFF,0xFE,0xFD,0xFC,0xFB,0xFA,0xF9,0xF8,0xF7,0xF6,0xF5,0xF4};

        DeviceIdentity id_a, id_b;
        FakeUniqueId_Set(uid_a); DeviceIdentity_Generate(&id_a);
        FakeUniqueId_Set(uid_b); DeviceIdentity_Generate(&id_b);
        test("different uid -> different uuid",
             memcmp(id_a.device_uuid, id_b.device_uuid, 16) != 0);
    }

    /* Time independence */
    printf("\n=== Time independence ===\n");
    {
        DeviceIdentity id_t1, id_t2;
        FakeUniqueId_Set(default_uid);
        FakePlatform_SetTick(0);
        DeviceIdentity_Generate(&id_t1);
        FakePlatform_SetTick(999999);
        DeviceIdentity_Generate(&id_t2);
        test("tick does not affect uuid",
             memcmp(id_t1.device_uuid, id_t2.device_uuid, 16) == 0);
    }

    /* Identity validation */
    printf("\n=== Identity validation ===\n");
    {
        DeviceIdentity valid, bad;
        FakeUniqueId_Set(default_uid);
        DeviceIdentity_Generate(&valid);
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

        bad = valid;
        bad.identity_schema_version = 0;
        test("zero schema version rejected", !DeviceIdentity_Validate(&bad));
    }

    /* Identity persistence and load */
    printf("\n=== Identity persist/load ===\n");
    {
        FakeFlash_Init();
        FakeUniqueId_Set(default_uid);
        Storage_Init();

        DeviceIdentity id1, id2;
        DeviceIdentity_Generate(&id1);
        printf("    id1.identity_schema_version=%u hw_rev=%u uuid[6]=0x%02X\n",
               (unsigned)id1.identity_schema_version,
               (unsigned)id1.hardware_revision,
               (unsigned)id1.device_uuid[6]);

        bool loaded = DeviceIdentity_Load(&id2);
        printf("    load result=%d\n", (int)loaded);
        test("identity saved", loaded);
        test("same uuid after load",
            loaded && memcmp(id1.device_uuid, id2.device_uuid, 16) == 0);
        test("loaded id passes validation",
            loaded && DeviceIdentity_Validate(&id2));
    }

    /* Corrupted identity rejected */
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

    /* Config: blank -> defaults */
    printf("\n=== Config defaults ===\n");
    {
        FakeFlash_Init();
        Storage_Init();
        Config_LoadDefaults();
        test("defaults set", Config_Get()->storage.light_period_ms == 500);
        test("calibration factor FP", fabsf(Config_Get()->runtime.light_calibration_factor - 1.0f) < 0.001f);
    }

    /* Config: save/load round-trip */
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

    /* Config: version mismatch rejected */
    printf("\n=== Config version mismatch ===\n");
    {
        ConfigStorageV1 bad;
        memcpy(&bad, &Config_Get()->storage, sizeof(bad));
        bad.version = 99;
        test("wrong version rejected", Config_Validate(&bad) == false);
    }

    /* Config: invalid values rejected */
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

    /* Reset config does not change identity */
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

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);

    return s_fail > 0 ? 1 : 0;
}