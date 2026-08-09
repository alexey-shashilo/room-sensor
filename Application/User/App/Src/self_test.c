#include "self_test.h"
#include "veml7700.h"
#include "display.h"
#include <string.h>

static SelfTestReport s_report;

void SelfTest_Init(SelfTestReport *report)
{
    if (report == NULL) return;
    memset(report, 0, sizeof(*report));
}

static SelfTestResult ProbeToResult(bool probed)
{
    return probed ? SELF_TEST_PASS : SELF_TEST_FAIL;
}

void SelfTest_Run(SelfTestReport *report, const I2cBus *bus)
{
    if ((report == NULL) || (bus == NULL))
    {
        if (report)
        {
            report->platform = SELF_TEST_FAIL;
            report->i2c = SELF_TEST_SKIPPED;
            report->light_sensor = SELF_TEST_SKIPPED;
            report->display = SELF_TEST_SKIPPED;
        }
        return;
    }

    memset(report, 0, sizeof(*report));

    report->platform = SELF_TEST_PASS;
    report->i2c = SELF_TEST_PASS;

    uint8_t addr;
    report->light_sensor = ProbeToResult(VEML7700_Probe(bus));
    report->display = ProbeToResult(Display_Probe(bus, &addr));

    s_report = *report;
}

const SelfTestReport *SelfTest_GetReport(void)
{
    return &s_report;
}