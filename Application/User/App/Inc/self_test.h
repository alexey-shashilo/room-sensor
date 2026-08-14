#ifndef SELF_TEST_H
#define SELF_TEST_H

#include <stdbool.h>
#include <stdint.h>
#include "i2c_bus.h"

typedef enum
{
    SELF_TEST_NOT_RUN = 0,
    SELF_TEST_PASS,
    SELF_TEST_FAIL,
    SELF_TEST_SKIPPED,
    SELF_TEST_DEGRADED   /* usable, but a redundancy mirror (A/B) is degraded */
} SelfTestResult;

typedef struct
{
    SelfTestResult platform;
    SelfTestResult i2c;
    SelfTestResult storage;
    SelfTestResult config;
    SelfTestResult identity;
    SelfTestResult light_sensor;
    SelfTestResult display;
    SelfTestResult co2_sensor;
    SelfTestResult temp_humidity_sensor;   /* SHT45 */
} SelfTestReport;

void SelfTest_Init(SelfTestReport *report);
void SelfTest_Run(SelfTestReport *report, const I2cBus *bus);
const SelfTestReport *SelfTest_GetReport(void);

/* Map a SelfTestResult to its wire-protocol string WITHOUT collapsing distinct
   states into pass/fail. NOT_RUN->"not_run", PASS->"pass", FAIL->"fail",
   SKIPPED->"skipped", DEGRADED->"degraded" (unknown -> "unknown"). */
const char *SelfTestResult_ToProtocolString(SelfTestResult result);

#endif