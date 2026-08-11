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

/* Map persistent storage state to a self-test result.
   OK -> PASS; NOT_FOUND (fresh/blank first boot) -> SKIPPED (runtime defaults /
   derived identity are used, persistence simply not yet established);
   CORRUPT / IO_ERROR -> FAIL. Runtime defaults being usable does NOT mean the
   persistence check passed. */
static SelfTestResult PersistenceToResult(StorageReadStatus st)
{
    switch (st)
    {
        case STORAGE_READ_OK:         return SELF_TEST_PASS;
        case STORAGE_READ_NOT_FOUND:  return SELF_TEST_SKIPPED;
        case STORAGE_READ_CORRUPT:    return SELF_TEST_FAIL;
        case STORAGE_READ_IO_ERROR:   return SELF_TEST_FAIL;
        default:                      return SELF_TEST_FAIL;
    }
}

void SelfTest_Run(SelfTestReport *report, const I2cBus *bus)
{
    if (report == NULL) return;

    memset(report, 0, sizeof(*report));

    /* SelfTest is OBSERVATIONAL: it inspects current state and performs NO
       persistence writes, no Storage_Init(), no Config_Load/Defaults mutation,
       and no identity persistence. I2C probe/read traffic is allowed. */

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

    /* Storage subsystem initialization is runtime state, not something a
       diagnostic re-initializes. Queried non-mutating. */
    report->storage = Storage_IsInitialized() ? SELF_TEST_PASS : SELF_TEST_FAIL;

    /* Config: inspect the persisted record without mutating live config. */
    report->config = PersistenceToResult(Config_SelfCheck());

    /* Identity: inspect the persisted record without persisting anything. */
    report->identity = PersistenceToResult(DeviceIdentity_SelfCheck());

    s_report = *report;
}

const SelfTestReport *SelfTest_GetReport(void)
{
    return &s_report;
}