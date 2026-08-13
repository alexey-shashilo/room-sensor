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
DriverStatus SCD41_StopPeriodicMeasurement(Scd41 *dev);

/* Query data-ready. `ready=false` is a VALID result (the SCD41 cadence has not
   elapsed) — it is NOT an error and must not be counted as one. */
DriverStatus SCD41_GetDataReady(Scd41 *dev, bool *ready);

/* Read a full measurement (CO2, T, RH). Every 16-bit word is CRC-8 validated;
   if ANY word's CRC fails the ENTIRE sample is rejected (no partial
   measurement is committed and *measurement->valid stays false). The caller
   must gate this on GetDataReady first. */
DriverStatus SCD41_ReadMeasurement(Scd41 *dev, Scd41Measurement *measurement);

/* Sensirion SCD4x CRC-8 (polynomial 0x31, init 0xFF). */
uint8_t SCD41_Crc8(const uint8_t *data, size_t count);

#endif