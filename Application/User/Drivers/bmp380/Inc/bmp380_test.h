#ifndef BMP380_TEST_H
#define BMP380_TEST_H

#include <stdint.h>
#include "bmp380.h"

/* Host unit-test export of the static BMP380 parse/compensate internals. These
   symbols are compiled into bmp380.c ONLY under BMP380_UNIT_TEST (set by the
   host test build), so the production firmware never carries them. The
   regression suite uses them to compare the production driver against Bosch
   BMP3_SensorAPI v2.0.6 frozen golden values WITHOUT linking any Bosch code. */

void              BMP380_UT_ParseCalib(const uint8_t *raw, Bmp380QuantizedCalib *q);
uint32_t          BMP380_UT_Raw24(const uint8_t *p);
void              BMP380_UT_Compensate(Bmp380QuantizedCalib *q, uint64_t raw_press,
                                       int64_t raw_temp, double *temp_out, double *press_out);

#endif