#include "self_test.h"
#include "veml7700.h"
#include "display.h"
#include "storage.h"
#include "config.h"
#include "device_identity.h"
#include <string.h>

static SelfTestReport s_report;

void SelfTest_Init(SelfTestReport *report)
{
    if (report == NULL) return;
    memset(report, 0, sizeof(*report));
}

static SelfTestResult ProbeToResult(bool ok)
{
    return ok ? SELF_TEST_PASS : SELF_TEST_FAIL;
}

void SelfTest_Run(SelfTestReport *report, const I2cBus *bus)
{
    if (report == NULL) return;

    memset(report, 0, sizeof(*report));

    report->platform = (bus != NULL) ? SELF_TEST_PASS : SELF_TEST_FAIL;
    report->i2c = (bus != NULL) ? SELF_TEST_PASS : SELF_TEST_FAIL;

    if (bus == NULL)
    {
        report->light_sensor = SELF_TEST_SKIPPED;
        report->display = SELF_TEST_SKIPPED;
    }
    else
    {
        uint8_t addr;
        report->light_sensor = ProbeToResult(VEML7700_Probe(bus));
        report->display = ProbeToResult(Display_Probe(bus, &addr));
    }

    report->storage = ProbeToResult(Storage_Init());

    bool config_ok = Config_Load();
    report->config = config_ok ? SELF_TEST_PASS : SELF_TEST_PASS;
    if (!config_ok)
        Config_LoadDefaults();

    DeviceIdentity id;
    bool id_loaded = DeviceIdentity_Load(&id);
    StorageReadStatus id_status = id_loaded ? STORAGE_READ_OK : DeviceIdentity_GetLoadStatus();

    if (id_status == STORAGE_READ_OK)
    {
        report->identity = SELF_TEST_PASS;
    }
    else
    {
        /* Runtime identity is deterministic and usable in RAM even when the
           persistent record is corrupt/IO_ERROR. Persist ONLY on a genuine
           first boot (NOT_FOUND); never overwrite a corrupt persistent record
           during boot (preserved for diagnostics/recovery). */
        if (DeviceIdentity_Derive(&id))
        {
            report->identity = SELF_TEST_PASS;
            if (id_status == STORAGE_READ_NOT_FOUND)
                DeviceIdentity_Save(&id);
        }
        else
        {
            report->identity = SELF_TEST_FAIL;
        }
    }

    s_report = *report;
}

const SelfTestReport *SelfTest_GetReport(void)
{
    return &s_report;
}