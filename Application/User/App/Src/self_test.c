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

/* Map (read status, redundancy health) to a self-test result. Read status alone
   is insufficient: a VALID+IO record reads OK yet its A/B mirror is unhealthy.
   Semantics:
     read OK + HEALTHY                  -> PASS
     read OK + DEGRADED/DEGRADED_IO     -> DEGRADED (usable, mirror unhealthy)
     read OK + CORRUPT/IO               -> FAIL
     NOT_FOUND (fresh/first boot)       -> SKIPPED
     read CORRUPT / IO_ERROR            -> FAIL
   Runtime defaults being usable does NOT mean the persistence check passed. */
static SelfTestResult PersistenceToResult(StorageReadStatus st, StorageHealth h)
{
    if (st == STORAGE_READ_OK)
    {
        switch (h)
        {
            case STORAGE_HEALTH_HEALTHY:    return SELF_TEST_PASS;
            case STORAGE_HEALTH_DEGRADED:   return SELF_TEST_DEGRADED;
            case STORAGE_HEALTH_DEGRADED_IO:return SELF_TEST_DEGRADED;
            case STORAGE_HEALTH_CORRUPT:    return SELF_TEST_FAIL;
            case STORAGE_HEALTH_IO_ERROR:   return SELF_TEST_FAIL;
            default:                        return SELF_TEST_FAIL;
        }
    }
    if (st == STORAGE_READ_NOT_FOUND) return SELF_TEST_SKIPPED;
    if (st == STORAGE_READ_CORRUPT)   return SELF_TEST_FAIL;
    if (st == STORAGE_READ_IO_ERROR)  return SELF_TEST_FAIL;
    return SELF_TEST_FAIL;
}

void SelfTest_Run(SelfTestReport *report, const I2cBus *bus)
{
    if (report == NULL) return;

    memset(report, 0, sizeof(*report));

    /* SelfTest is OBSERVATIONAL: it inspects current state and performs NO
       persistence writes, no Storage_Init(), no Config_Load/Defaults mutation,
       and no identity persistence. I2C probe/read traffic is allowed. */

    /* PAL semantics: `platform` currently reports whether the minimum execution
   platform is available (a valid I2C bus handle is required for any driver
   traffic). It is therefore kept in step with the separately-checked `i2c`
   field; both report PASS iff a bus is present. `platform` is intentionally NOT
   a deep SoC self-diagnostic and is deliberately not extended here (P2, no
   redesign). */
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

    /* Config: inspect the persisted record without mutating live config.
       Report redundancy health: a usable record with a damaged mirror yields
       SELF_TEST_DEGRADED rather than a misleading PASS. */
    report->config = PersistenceToResult(Config_SelfCheck(), Storage_GetHealth(RECORD_TYPE_CONFIG));

    /* Identity: inspect the persisted record without persisting anything. */
    report->identity = PersistenceToResult(DeviceIdentity_SelfCheck(), Storage_GetHealth(RECORD_TYPE_IDENTITY));

    s_report = *report;
}

const SelfTestReport *SelfTest_GetReport(void)
{
    return &s_report;
}