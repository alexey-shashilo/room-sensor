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
    SELF_TEST_SKIPPED
} SelfTestResult;

typedef struct
{
    SelfTestResult platform;
    SelfTestResult i2c;
    SelfTestResult light_sensor;
    SelfTestResult display;
} SelfTestReport;

void SelfTest_Init(SelfTestReport *report);
void SelfTest_Run(SelfTestReport *report, const I2cBus *bus);
const SelfTestReport *SelfTest_GetReport(void);

#endif