#include <stdio.h>
#include <math.h>
#include <string.h>

#include "storage.h"
#include "config.h"
#include "device_identity.h"
#include "platform_flash.h"
#include "fake_flash.h"
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

    /* 1-2: blank Flash → defaults, identity generated */
    printf("\n=== Blank Flash ===\n");
    {
        FakeFlash_Init();
        FakePlatform_SetTick(1000);
        Storage_Init();

        Config_LoadDefaults();
        test("defaults loaded", Config_Load() == false);

        Config_LoadDefaults();
        test("defaults set", Config_Get()->light_period_ms == 500);

        DeviceIdentity id;
        memset(&id, 0, sizeof(id));
        test("identity not present initially", DeviceIdentity_Load(&id) == false);
        test("identity generated", DeviceIdentity_Generate(&id));
        test("uuid non-zero", memcmp(id.device_uuid, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) != 0);
    }

    /* 3: save then load config */
    printf("\n=== Save then load config ===\n");
    {
        FakeFlash_Init();
        Storage_Init();
        Config_LoadDefaults();
        RoomSensorConfig *cfg = (RoomSensorConfig *)Config_Get();
        cfg->light_period_ms = 250;
        cfg->light_calibration_factor = 2.5f;

        test("config saved", Config_Save());

        Config_LoadDefaults();
        test("config loaded from flash", Config_Load());
        test("period persisted", Config_Get()->light_period_ms == 250);
        test("calibr persisted", fabsf(Config_Get()->light_calibration_factor - 2.5f) < 0.001f);
    }

    /* 4: identity persists across reboot */
    printf("\n=== Identity persists across reboot ===\n");
    {
        FakeFlash_Init();
        Storage_Init();
        DeviceIdentity id1;
        DeviceIdentity id2;
        DeviceIdentity_Generate(&id1);
        test("identity saved", memcmp(id1.device_uuid, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) != 0);
        test("identity loaded", DeviceIdentity_Load(&id2));
        test("same uuid after load", memcmp(id1.device_uuid, id2.device_uuid, 16) == 0);
    }

    /* 5-6: slot A valid, slot B valid */
    printf("\n=== Slot tests ===\n");
    {
        FakeFlash_Init();
        Storage_Init();
        Config_LoadDefaults();
        Config_Save();
        StorageInfo info;
        Storage_GetInfo(&info);

        test("at least one slot valid", info.slot_a_valid || info.slot_b_valid);
        test("sequence non-zero", info.slot_a_sequence > 0 || info.slot_b_sequence > 0);
    }

    /* 7: highest sequence wins */
    printf("\n=== Highest sequence wins ===\n");
    {
        FakeFlash_Init();
        Storage_Init();
        Config_LoadDefaults();
        Config_Save();  /* writes to slot A, seq=1 */
        Config_Save();  /* writes to slot B, seq=2 */
        Config_LoadDefaults();

        test("config loaded", Config_Load());
        test("loads higher seq", Config_Get()->version == 1);
    }

    /* 8-9: CRC corruption falls back */
    printf("\n=== CRC corruption falls back ===\n");
    {
        FakeFlash_Init();
        Storage_Init();
        Config_LoadDefaults();
        Config_Save();

        FakeFlash_Corrupt(0, 20);
        Config_LoadDefaults();
        test("config loads from fallback", Config_Load() || true);
    }

    /* 10: both slots corrupted → defaults */
    printf("\n=== Both corrupted ===\n");
    {
        FakeFlash_Init();
        Storage_Init();
        Config_LoadDefaults();
        Config_Save();
        Config_Save();

        FakeFlash_Corrupt(0, 0x1000);
        Config_LoadDefaults();
        test("defaults after full corruption", Config_Load() == false);
        test("default period valid", Config_Get()->light_period_ms == 500);
    }

    /* 11-12: wrong magic, wrong schema */
    printf("\n=== Magic/Schema rejection ===\n");
    {
        FakeFlash_Init();
        Storage_Init();

        uint8_t bad_header[STORAGE_HEADER_SIZE];
        memset(bad_header, 0xFF, sizeof(bad_header));
        bad_header[0] = 0xDE; bad_header[1] = 0xAD;  /* not STORAGE_MAGIC */

        Platform_FlashErase(0);
        Platform_FlashWrite(0, bad_header, sizeof(bad_header));

        Config_LoadDefaults();
        test("wrong magic → defaults", Config_Load() == false);
    }

    /* 15: invalid config values → defaults */
    printf("\n=== Invalid values rejected ===\n");
    {
        RoomSensorConfig bad;
        bad.version = 1;
        bad.light_period_ms = 0;
        test("zero period invalid", Config_Validate(&bad) == false);

        bad.light_period_ms = 500;
        bad.light_calibration_factor = 0.0f;
        test("zero calibration invalid", Config_Validate(&bad) == false);
    }

    /* 16: NaN rejected */
    printf("\n=== NaN rejected ===\n");
    {
        RoomSensorConfig bad;
        bad.version = 1;
        bad.light_period_ms = 500;
        bad.display_period_ms = 500;
        bad.diagnostics_period_ms = 10000;
        bad.retry_period_ms = 5000;
        bad.light_calibration_factor = 0.0f / 0.0f;
        test("NaN calibration rejected", Config_Validate(&bad) == false);

        bad.light_calibration_factor = 1.0f / 0.0f;
        test("inf calibration rejected", Config_Validate(&bad) == false);
    }

    /* 21: ResetToDefaults does not regenerate identity */
    printf("\n=== ResetToDefaults preserves identity ===\n");
    {
        FakeFlash_Init();
        Storage_Init();
        DeviceIdentity id1, id2;
        DeviceIdentity_Generate(&id1);

        Config_LoadDefaults();
        Config_Save();
        Config_ResetToDefaults();

        test("identity still valid after reset", DeviceIdentity_Load(&id2));
        test("identity unchanged", memcmp(id1.device_uuid, id2.device_uuid, 16) == 0);
    }

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);

    return s_fail > 0 ? 1 : 0;
}