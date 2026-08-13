#ifndef FAKE_I2C_BUS_H
#define FAKE_I2C_BUS_H

#include <stdint.h>
#include <stddef.h>
#include "i2c_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Host fake I2C bus with scripted transaction support.

   Besides the register-mapped VEML/display behavior it ALSO models the
   Sensirion SCD4x command/response style: a plain read (I2cBus_Read) returns
   the current `read_response` buffer. Tests script probe success/failure,
   command TX, data-ready/measurement responses, CRC corruption, I2C read/write
   failure, sensor disappearance and recovery by manipulating the fields below
   and the helper builders. */
typedef struct
{
    /* Per-operation results (DRIVER_STATUS_OK by default). */
    DriverStatus write_result;
    DriverStatus read_mem_result;
    DriverStatus read_result;
    DriverStatus probe_result;

    int write_call_count;
    int read_mem_call_count;
    int read_call_count;
    int probe_call_count;

    /* Register-mapped memory (VEML/legacy). */
    uint8_t  regs[256];
    uint16_t last_write_reg;
    uint16_t last_write_value;

    /* Full last-written command/data bytes + length (SCD41 command framing). */
    uint8_t  last_write_data[16];
    size_t   last_write_size;

    /* Last SCD41 command (decoded from last_write_data) for stateful responses:
       GET_DATA_READY (0xE4B8) returns data_ready_word+CRC on a 3-byte read;
       READ_MEASUREMENT (0xEC05) returns read_response. */
    uint16_t last_scd41_cmd;

    /* Scripted data-ready (SCD41 get_data_ready_status returns one word).
       When `data_ready_scripted` is set, a plain read returns this word
       followed by its CRC. */
    uint8_t data_ready_scripted;
    uint16_t data_ready_word;

    /* Scripted plain-read response payload (SCD41 read_measurement: 9 bytes,
       or a 3-byte single-word command response). Reproduced verbatim (no CRC
       injection) so tests can pre-select valid or corrupted bytes. */
    uint8_t  read_response[16];
    size_t   read_response_size;
} FakeI2cBus;

void FakeI2cBus_Init(FakeI2cBus *fake);
void FakeI2cBus_GetBus(I2cBus *bus, FakeI2cBus *fake);

void FakeI2cBus_SetAlsRead(FakeI2cBus *fake, uint16_t raw);

/* Sensirion SCD4x scripting helpers. */

/* Schedule the data-ready plain read response. ready=false clears the
   scripted data-ready word so not-ready reads return a zero word. */
void FakeI2cBus_SetScd41DataReady(FakeI2cBus *fake, bool ready);

/* Schedule a 9-byte SCD41 measurement response from raw 16-bit words. Each
   word is followed by its CRC-8. `corrupt_co2/temp/rh` optionally corrupt the
   corresponding CRC byte so a specific word fails validation. */
void FakeI2cBus_SetScd41Measurement(FakeI2cBus *fake,
                                    uint16_t co2,
                                    uint16_t temp_raw,
                                    uint16_t rh_raw,
                                    bool corrupt_co2,
                                    bool corrupt_temp,
                                    bool corrupt_rh);

/* Compute the Sensirion SCD4x CRC-8 for `count` bytes (used to script frames).
   Duplicated from the driver as a test helper; an integration test reads a
   fake-built frame through the real driver to cross-check both copies agree. */
uint8_t FakeI2cBus_Scd41Crc(uint8_t *data, size_t count);

/* Convenience: raw temperature / RH word encodings per SCD4x datasheet.
     temp_raw = (T_c + 45) * 65535 / 175
     rh_raw   = RH_pct       * 65535 / 100 */
uint16_t FakeI2cBus_TempRaw(float temp_c);
uint16_t FakeI2cBus_RhRaw(float rh_pct);

#ifdef __cplusplus
}
#endif

#endif