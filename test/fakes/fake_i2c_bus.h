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
   Sensirion SCD4x command/response style. The SCD4x is a legal-stateful device,
   not merely a status mailbox: it tracks whether it is in IDLE or PERIODIC
   measurement mode and only accepts the commands the datasheet allows in that
   mode. This prevents the class of "fake agrees with production bug" that
   previously hid the retained-periodic-mode failure.

   SCD4x mode legality (datasheet §3.6/Table 9):
     IDLE:     start_periodic (0x21B1) allowed.
     PERIODIC: start_periodic 0x21B1 -> NACK (BUS_ERROR);
               stop_periodic   0x3F86 -> allowed (returns to IDLE after settle).
   Tests script probe success/failure, measurement responses, CRC corruption,
   I2C read/write failure, sensor disappearance and recovery by manipulating the
   fields below and the helper builders. */
typedef enum
{
    FAKE_SCD41_MODE_IDLE = 0,
    FAKE_SCD41_MODE_PERIODIC
} FakeScd41Mode;

typedef struct
{
    /* Modeled SCD4x measurement mode (default IDLE). When PERIODIC, the fake
       rejects start_periodic_measurement with an acknowledge-style failure and
       accepts stop_periodic_measurement (legal during measurement). */
    FakeScd41Mode scd41_mode;

    /* Per-operation results (DRIVER_STATUS_OK by default). */
    DriverStatus write_result;
    DriverStatus read_mem_result;
    DriverStatus read_result;
    DriverStatus probe_result;

    int write_call_count;
    int read_mem_call_count;
    int read_call_count;
    int probe_call_count;

    /* Monotonic tick at which the most recent plain read (I2cBus_Read) was
       performed. Used by timing regressions to prove a two-phase response is
       NOT read before its deadline (the fake stamps Platform_GetTickMs()). */
    uint32_t last_read_tick_ms;

    /* Last 8-bit (left-shifted) device address used across all transactions.
       Lets tests assert a driver addresses the correct on-wire byte (e.g. the
       SCD41 must probe/write/read at the left-shifted 0xC4, not 7-bit 0x62). */
    uint16_t last_addr;

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

    /* Ordered log of the last SCD41 2-byte write commands (START_PERIODIC /
       STOP_PERIODIC / GET_DATA_READY / READ_MEASUREMENT), so tests can assert
       the exact protocol sequence across a recovery (e.g. 21B1 3F86 ... 21B1). */
    uint16_t scd41_cmd_log[16];
    int scd41_cmd_log_count;

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

    /* SHT45 (0x88 wire addr) single-byte command + scripted 6-byte response.
       SHT45 is a single-shot sensor: a 1-byte command write (measure 0xFD etc.),
       then a 6-byte read (T-word+CRC, RH-word+CRC). Independent of the SCD41
       state machine so tests exercise the two sensors independently. */
    uint8_t  sht45_last_cmd;
    uint8_t  sht45_read_response[6];
    bool     sht45_respond;   /* when false, a 6-byte SHT45 read NACKs */
    int      sht45_measure_cmd_count;

    /* BMP390: the flat register map cannot host both VEML's ALS_CONF at reg 0x00
       AND the BMP390 CHIP_ID at reg 0x00. When a BMP390 wire address (0x76/0x77
       left-shifted = 0xEC/0xEE) is read at CHIP_ID (reg 0x00) or CALIB_DATA
       (reg 0x31), serve these dedicated fields instead of the shared regs[], so
       app-level tests can present a BMP390 alongside VEML/display/SCD41/SHT45.
       bmp390_chip_id == 0 means "no BMP390 present" (reads return regs[]). */
    uint16_t bmp390_wire_addr;   /* left-shifted wire address to relay, 0 = disabled */
    uint8_t  bmp390_chip_id;
    uint8_t  bmp390_calib[21];
} FakeI2cBus;

void FakeI2cBus_Init(FakeI2cBus *fake);
void FakeI2cBus_GetBus(I2cBus *bus, FakeI2cBus *fake);

void FakeI2cBus_SetAlsRead(FakeI2cBus *fake, uint16_t raw);

/* SCD4x measurement-mode control (models retained periodic state). */
void FakeI2cBus_SetScd41Mode(FakeI2cBus *fake, FakeScd41Mode mode);

/* Sensirion SCD4x scripting helpers. */

/* Schedule the data-ready plain read response. ready=false clears the
   scripted data-ready word so not-ready reads return a zero word. */
void FakeI2cBus_SetScd41DataReady(FakeI2cBus *fake, bool ready);

/* Inject a raw SCD41 data-ready response as explicit wire bytes (MSB-first
   word + CRC). Used by independent fixed-vector tests; the caller supplies the
   exact bytes, so it does not rely on any encode helper. */
void FakeI2cBus_SetScd41RawDataReady(FakeI2cBus *fake, uint8_t msb, uint8_t lsb);

/* Inject a raw SCD41 measurement response (9 bytes exactly) verbatim. The
   caller supplies the full MSB-first word+CRC triplets, so fixed wire vectors
   can be tested independently of any word encode helper. */
void FakeI2cBus_SetScd41RawRead(FakeI2cBus *fake, const uint8_t raw9[9]);

/* Inject a raw 3-byte single-word command response verbatim (word + CRC),
   used by fixed-vector tests that do not rely on an encode helper. */
void FakeI2cBus_SetRawRead(FakeI2cBus *fake, const uint8_t *data, size_t size);

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

/* SHT45 scripting. `raw6` is the exact on-wire response (T word MSB-first +
   CRC, RH word MSB-first + CRC). `respond` controls whether the fake NACKs the
   6-byte read (sensor busy / conversion in progress). */
void FakeI2cBus_SetSht45Response(FakeI2cBus *fake, const uint8_t raw6[6], bool respond);

/* Present / remove a BMP390 at a given wire address (left-shifted 0xEC/0xEE).
   Relays CHIP_ID (reg 0x00) and CALIB_DATA (reg 0x31) from dedicated fields so
   app-level tests can host a BMP390 alongside VEML/display without the flat
   register map colliding (VEML uses reg 0x00 for ALS_CONF). */
void FakeI2cBus_SetBmp390Present(FakeI2cBus *fake, uint16_t wire_addr,
                                 uint8_t chip_id, const uint8_t calib[21]);
void FakeI2cBus_SetBmp390Absent(FakeI2cBus *fake);

#ifdef __cplusplus
}
#endif

#endif