#ifndef SGP41_H
#define SGP41_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "room_sensor_types.h"
#include "i2c_bus.h"

/* SGP41 (Sensirion) VOC / NOx sensor.

   Portable transport driver. It depends ONLY on the I2cBus * abstraction —
   never on the STM32 HAL, App, RoomState, Telemetry, Display or Command layers.
   Timing and lifecycle (non-blocking measurement state machine in the runtime /
   App) are NOT in these operations, so no call here blocks the watchdog.

   Protocol constants come from the official Sensirion SGP41 datasheet and the
   Sensirion embedded-i2c-sgp41 reference driver
   (https://github.com/Sensirion/embedded-i2c-sgp41). The I2C address is a named
   constant inside the driver; App never sees the raw address.

   The gas-index (raw VOC/NOx -> VOC/NOx Index) computation is a SEPARATE
   concern and lives in Drives/gas_index (see gas_index.h). This driver returns
   only raw ticks; it never fabricates an air-quality index. */

/* SGP41 7-bit I2C address (official Sensirion datasheet / driver). The I2cBus
   abstraction expects the address left-shifted into 8-bit form (bit0=R/W),
   same as SCD41/SHT45/Display. The driver left-shifts at Init so the named
   constant stays the official datasheet value while matching the bus's
   byte-address convention on the wire. */
#define SGP41_I2C_ADDR (0x59U)

/* Response words (2 bytes each + 1 CRC byte each). */
#define SGP41_RESPONSE_WORD_BYTES 3U
#define SGP41_CONDITIONING_RESPONSE_BYTES (1U * SGP41_RESPONSE_WORD_BYTES)
#define SGP41_MEASURE_RESPONSE_BYTES (2U * SGP41_RESPONSE_WORD_BYTES)
#define SGP41_SELF_TEST_RESPONSE_BYTES (1U * SGP41_RESPONSE_WORD_BYTES)
#define SGP41_SERIAL_RESPONSE_BYTES (3U * SGP41_RESPONSE_WORD_BYTES)

/* Command identifiers (SGP41 I2C, 16-bit big-endian MSB-first). From the
   Sensirion SGP41 datasheet / embedded-i2c-sgp41 driver. */
#define SGP41_CMD_MEASURE_RAW_SIGNALS 0x2619U
#define SGP41_CMD_EXECUTE_CONDITIONING 0x2612U
#define SGP41_CMD_EXECUTE_SELF_TEST 0x280EU
#define SGP41_CMD_TURN_HEATER_OFF 0x3615U
#define SGP41_CMD_GET_SERIAL_NUMBER 0x3682U

/* Official command execution times (Sensirion SGP41 datasheet / reference
   driver). The response to a command may only be read after the specified
   command execution time has elapsed. The runtime enforces this cooperatively
   (non-blocking) using a deadline captured after the Begin_* command write;
   the Finish_* read is performed on a later scheduler tick. The portable
   driver never blocks. */
#define SGP41_MEASURE_EXECUTION_MS 50U      /* measure_raw_signals */
#define SGP41_CONDITIONING_EXECUTION_MS 50U /* execute_conditioning */
#define SGP41_TURN_HEATER_OFF_EXECUTION_MS 1U
#define SGP41_SERIAL_EXECUTION_MS 1U

/* Humidity compensation default tick values (SGP41 datasheet /
   embedded-i2c-sgp41): sending these disables humidity compensation.
   RH ticks = %RH * 65535 / 100  (50%RH -> 0x8000)
   T  ticks = (degC + 45) * 65535 / 175 (25 degC -> 0x6666) */
#define SGP41_DEFAULT_RH_TICKS 0x8000U
#define SGP41_DEFAULT_T_TICKS 0x6666U

/* Conditioning duration safety: to avoid damage to the sensing material the
   conditioning must not exceed 10 s cumulative per power-up (SGP41 datasheet /
   reference example runs 10 x 1 s). */
#define SGP41_CONDITIONING_MAX_MS 10000U

/* One decoded measurement: the two raw ticks the SGP41 returns for a single
   measure_raw_signals transaction. `valid` is true only when BOTH words passed
   CRC-8 (no partial commitment on a failure). Values are raw ticks proportional
   to log(resistance); they are NOT air-quality indices. */
typedef struct
{
    uint16_t raw_voc;
    uint16_t raw_nox;
    bool valid;
} Sgp41RawMeasurement;

typedef struct
{
    const I2cBus *bus;
    uint16_t address;
    uint8_t initialized;
} Sgp41;

DriverStatus SGP41_Init(Sgp41 *dev, const I2cBus *bus);

/* Probe: verify the SGP41 answers on its I2C address. Non-blocking. */
DriverStatus SGP41_Probe(const I2cBus *bus);

/* Two-phase SGP41 response model. The SGP41 requires SGP41_MEASURE_EXECUTION_MS
   of command execution time between sending a command and reading its response
   (50 ms for measure/conditioning, ~1 ms for serial). Each Begin_* sends the
   command and returns immediately; the caller must wait the corresponding
   execution time (enforced cooperatively by the runtime deadline) before
   calling the matching Finish_* to read the response. No call here blocks. */

/* MEASURE_RAW_SIGNALS (0x2619): write RH + temperature compensation ticks.
   Begin: send command. Finish: read 6 bytes (VOC word + CRC, NOx word + CRC),
   CRC-validate EVERY word; if either CRC fails the WHOLE sample is rejected
   (no partial commit) and measurement->valid stays false. */
DriverStatus SGP41_BeginMeasure(Sgp41 *dev, uint16_t rh_ticks, uint16_t t_ticks);
DriverStatus SGP41_FinishMeasure(Sgp41 *dev, Sgp41RawMeasurement *measurement);

/* EXECUTE_CONDITIONING (0x2612): operate the VOC pixel at measurement temp and
   the NOx pixel at a conditioning temp, returning only the VOC raw tick. Used
   during the <=10 s NOx conditioning warm-up. Begin: send command. Finish:
   read 3 bytes (VOC word + CRC). */
DriverStatus SGP41_BeginConditioning(Sgp41 *dev, uint16_t rh_ticks, uint16_t t_ticks);
DriverStatus SGP41_FinishConditioning(Sgp41 *dev, uint16_t *raw_voc);

/* EXECUTE_SELF_TEST (0x280E): trigger the built-in self-test; result low 4 bits
   are 0 when passed. NOTE: the official command execution time is 320 ms (see
   SGP41_SELF_TEST_EXECUTION_MS). The runtime / SelfTest must enforce this
   deadline cooperatively. */
DriverStatus SGP41_BeginSelfTest(Sgp41 *dev);
DriverStatus SGP41_FinishSelfTest(Sgp41 *dev, uint8_t *test_result);

/* TURN_HEATER_OFF (0x3615): stop the hotplate and enter idle mode. Single write,
   no response. The sensor must fully settle before a subsequent measurement
   (reference driver waits 1 ms; a longer settle is appropriate after idle
   before re-measuring). */
DriverStatus SGP41_TurnHeaterOff(Sgp41 *dev);

/* GET_SERIAL_NUMBER (0x3682): Begin: send command. Finish: read 9 bytes
   (3 words + CRCs). Each word CRC-validated. `serial[3]` is a 48-bit unique
   serial (serial[0] most-significant word). */
DriverStatus SGP41_BeginGetSerial(Sgp41 *dev);
DriverStatus SGP41_FinishGetSerial(Sgp41 *dev, uint16_t serial[3]);

/* Sensirion SGP41 CRC-8 (polynomial 0x31, init 0xFF), identical to the
   SCD41/SHT45 Sensirion CRC used elsewhere in this codebase. One checksum per
   16-bit word (SGP41 datasheet; Sensirion common). */
uint8_t SGP41_Crc8(const uint8_t *data, size_t count);

#endif