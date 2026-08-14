#ifndef BMP390_TEST_H
#define BMP390_TEST_H

#include <stdint.h>
#include "bmp390.h"

/* Host unit-test export of the static BMP390 parse/compensate internals. These
   symbols are compiled into bmp390.c ONLY under BMP390_UNIT_TEST (set by the
   host test build), so the production firmware never carries them. The regression
   suite uses them to compare the production driver against Bosch BMP3_SensorAPI
   v2.0.6 frozen golden values WITHOUT linking any Bosch code into the driver. */

void              BMP390_UT_ParseCalib(const uint8_t *raw, Bmp390QuantizedCalib *q);
uint32_t          BMP390_UT_Raw24(const uint8_t *p);
void              BMP390_UT_Compensate(Bmp390QuantizedCalib *q, uint64_t raw_press,
                                       int64_t raw_temp, double *temp_out, double *press_out);

#endif