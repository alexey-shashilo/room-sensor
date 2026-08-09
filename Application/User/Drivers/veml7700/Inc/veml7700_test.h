#ifndef VEML7700_TEST_H
#define VEML7700_TEST_H

#include <stdint.h>
#include "veml7700.h"

typedef struct
{
    VEML7700_Gain gain;
    VEML7700_IntegrationTime it;
    float sensitivity;
    float resolution;
} VEML7700_RangeEntry;

uint8_t VEML7700_GetRangeCount(void);
const VEML7700_RangeEntry *VEML7700_GetRangeEntry(uint8_t idx);
float VEML7700_ItMs(VEML7700_IntegrationTime it);

#endif