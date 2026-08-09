#ifndef VEML7700_TEST_H
#define VEML7700_TEST_H

#include <stdint.h>

uint8_t VEML7700_UT_GetRangeCount(void);
uint8_t VEML7700_UT_GetRangeGain(uint8_t idx);
uint8_t VEML7700_UT_GetRangeIt(uint8_t idx);
float   VEML7700_UT_GetRangeSens(uint8_t idx);
float   VEML7700_UT_GetRangeRes(uint8_t idx);

#endif