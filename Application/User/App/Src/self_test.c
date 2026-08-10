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
    if (DeviceIdentity_Load(&id))
    {
        report->identity = SELF_TEST_PASS;
    }
    else
    {
        if (DeviceIdentity_Derive(&id) && DeviceIdentity_Save(&id))
            report->identity = SELF_TEST_PASS;
        else
            report->identity = SELF_TEST_FAIL;
    }

    s_report = *report;
}

const SelfTestReport *SelfTest_GetReport(void)
{
    return &s_report;
}