#ifndef SCD41_H
#define SCD41_H

#include <stdbool.h>
#include <stdint.h>
#include "room_sensor_types.h"
#include "i2c_bus.h"

/* SCD41 (Sensirion SCD4x family) CO2 / temperature / humidity sensor.

   Portable driver. It depends ONLY on the I2cBus * abstraction — never on the
   STM32 HAL, App, RoomState, Telemetry, Display or Command layers. Timing and
   lifecycle (periodic measurement state machine) live in the runtime / App,
   NOT inside these operations, so no call here blocks the watchdog.

   All protocol constants come from the official Sensirion SCD4x datasheet and
   the Sensirion SCD4x I2C driver (embedded-scd4x). The I2C address is a named
   constant inside the driver; App never sees the raw address. */

/* SCD4x 7-bit I2C address (official Sensirion datasheet). The I2cBus
   abstraction and its STM32 HAL implementation expect the address already
   left-shifted into 8-bit form (bit0 = R/W), same as VEML7700/Display
   (e.g. 0x10<<1). The driver left-shifts at Init so the named constant stays
   the official datasheet value while matching the bus's byte-address
   convention on the wire. */
#define SCD41_I2C_ADDR          (0x62U)
#define SCD41_SERIAL_NUM_WORDS  (3U)

/* Command identifiers (SCD4x I2C, big-endian incl. CRC when payload present).
   From Sensirion SCD4x driver / datasheet. */
#define SCD41_CMD_START_PERIODIC  0x21B1U
#define SCD41_CMD_STOP_PERIODIC   0x3F86U
#define SCD41_CMD_GET_DATA_READY  0xE4B8U
#define SCD41_CMD_READ_MEASUREMENT 0xEC05U

/* Periodic measurement cadence (SCD4x datasheet). Data becomes ready ~5 s after
   start_periodic_measurement and thereafter on the same interval. */
#define SCD41_PERIODIC_INTERVAL_MS 5000U

/* Data-ready word: data is ready when any of the 11 least-significant bits is
   set (SCD4x datasheet). */
#define SCD41_DATA_READY_MASK     0x07FFU

/* Official SCD4x command execution times (Sensirion SCD4x datasheet / reference
   driver "embedded-scd4x"). The response to a command may only be read after
   the specified command execution time has elapsed:
     start_periodic_measurement : no read-back, no mandated response delay.
     stop_periodic_measurement  : first new data ~1 s; the command completes in
                                  500 ms before the sensor is idle-safe.
     get_data_ready_status      : ~1 ms command execution time before the 2-byte
                                  response may be read.
     read_measurement           : ~1 ms command execution time before the 9-byte
                                  response may be read.
   Timing is enforced by the runtime (cooperative, non-blocking) using a
   deadline captured after the Begin_* command write; the Finish_* read is
   performed on a later scheduler tick once the deadline has elapsed. The
   portable driver never blocks: there is no HAL/polling delay inside it. */
#define SCD41_COMMAND_RESPONSE_DELAY_MS 1U

typedef struct
{
    uint16_t co2_ppm;
    float temperature_c;
    float relative_humidity_pct;

    /* False if the SCD41 has no valid sample (still warming up / no data).
       A freshly-read, fully-CRC-valid measurement sets valid=true. */
    bool valid;
} Scd41Measurement;

typedef struct
{
    const I2cBus *bus;
    uint16_t address;
    uint8_t  initialized;
} Scd41;

DriverStatus SCD41_Init(Scd41 *dev, const I2cBus *bus);

/* Probe: verify the SCD41 answers on its I2C address. Non-blocking. */
DriverStatus SCD41_Probe(const I2cBus *bus);

DriverStatus SCD41_StartPeriodicMeasurement(Scd41 *dev);

/* stop_periodic_measurement: issues the command. NOTE: the official protocol
   requires ~500 ms of command execution time before the sensor is safe for
   commands that need idle mode. This function only transmits the command — it
   does NOT block and does NOT imply the sensor is immediately idle. */
DriverStatus SCD41_StopPeriodicMeasurement(Scd41 *dev);

/* Two-phase SCD4x response model. The SCD4x requires ~1 ms command execution
   time between sending a command and reading its response. Each Begin_* sends
   the command and returns immediately; the caller must wait
   SCD41_COMMAND_RESPONSE_DELAY_MS (enforced cooperatively by the runtime
   deadline) before calling the matching Finish_* to read the response. No call
   here blocks, and there is no duplicate protocol logic between phases. */

/* GET_DATA_READY (0xE4B8). Begin: send command. Finish: read 3 bytes (word +
   CRC), validate CRC, output ready flag. `ready=false` (no new data) is a VALID
   result, NOT an error. */
DriverStatus SCD41_BeginGetDataReady(Scd41 *dev);
DriverStatus SCD41_FinishGetDataReady(Scd41 *dev, bool *ready);

/* READ_MEASUREMENT (0xEC05). Begin: send command. Finish: read 9 bytes (3
   words + CRCs), CRC-validate EVERY word; if any CRC fails the WHOLE sample is
   rejected (no partial commit) and measurement->valid stays false. Callers must
   gate this on data-ready first. */
DriverStatus SCD41_BeginReadMeasurement(Scd41 *dev);
DriverStatus SCD41_FinishReadMeasurement(Scd41 *dev, Scd41Measurement *measurement);

/* Sensirion SCD4x CRC-8 (polynomial 0x31, init 0xFF). */
uint8_t SCD41_Crc8(const uint8_t *data, size_t count);

#endif