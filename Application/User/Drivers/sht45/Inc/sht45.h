#ifndef SHT45_H
#define SHT45_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "room_sensor_types.h"
#include "i2c_bus.h"

/* SHT45 (Sensirion SHT4x family) temperature / humidity sensor.

   Portable driver. It depends ONLY on the I2cBus * abstraction — never on the
   STM32 HAL, App, RoomState, Telemetry, Display or Command layers. Timing and
   lifecycle (single-shot measurement state machine) live in the runtime / App,
   NOT inside these operations, so no call here blocks the watchdog.

   All protocol constants come from the official Sensirion SHT4x datasheet
   (V7.3, 2025) and the Sensirion SHT4x I2C driver (embedded-sht). The I2C
   address is a named constant inside the driver; App never sees the raw
   address. */

/* SHT4x 7-bit I2C address (official datasheet: SHT45-AD1B -> 0x44). The I2cBus
   abstraction and its STM32 HAL implementation expect the address already
   left-shifted into 8-bit form (bit0 = R/W), same as VEML7700/Display/SCD41.
   The driver left-shifts at Init so the named constant stays the official
   datasheet value while matching the bus's byte-address convention. */
#define SHT45_I2C_ADDR   (0x44U)

/* Single-byte measurement commands (SHT4x datasheet Table 8). Preferred v1 mode
   is HIGH repeatability (0xFD) — the best precision, no heater. */
#define SHT45_CMD_MEASURE_HPM  0xFDU   /* measure T & RH, high precision */
#define SHT45_CMD_MEASURE_MPM  0xF6U   /* measure T & RH, medium precision */
#define SHT45_CMD_MEASURE_LPM  0xE0U   /* measure T & RH, low precision */
#define SHT45_CMD_READ_SERIAL  0x89U
#define SHT45_CMD_SOFT_RESET   0x94U

/* Response payload length for a T&RH measurement: T-word(2) + CRC(1) + RH-word(2)
   + CRC(1) = 6 bytes, transmitted in that order (datasheet §4.3/Table 9). */
#define SHT45_MEASUREMENT_RESPONSE_LEN 6U

/* High-precision measurement duration (datasheet tMEAS,H; reference driver:
   SHT4X_MEASUREMENT_DURATION_USEC = 10 ms). Non-blocking: the runtime waits this
   deadline cooperatively before reading (a read-before-ready NACKs). */
#define SHT45_MEASUREMENT_DURATION_MS 10U

/* Soft-reset command execution time (datasheet §3.1: tSR <= 1 ms). */
#define SHT45_RESET_DURATION_MS 1U

/* Sensirion CRC-8 (polynomial 0x31, init 0xFF, byte-reflected), identical to
   the SCD4x CRC used elsewhere in this codebase. One checksum per 16-bit data
   word, transmitted after the word (SHT4x datasheet §4.4). */
uint8_t SHT45_Crc8(const uint8_t *data, size_t count);

typedef struct
{
    float temperature_c;
    float relative_humidity_pct;

    /* False if no valid, fully-CRC-checked sample was accepted. */
    bool valid;
} Sht45Measurement;

typedef struct
{
    const I2cBus *bus;
    uint16_t address;
    uint8_t  initialized;
} Sht45;

/* Init the driver handle against a bus. Left-shifts the official 7-bit address
   into the bus's 8-bit (R/W bit) wire convention. */
DriverStatus SHT45_Init(Sht45 *dev, const I2cBus *bus);

/* Probe: verify the SHT45 answers on its I2C address. Non-blocking. */
DriverStatus SHT45_Probe(const I2cBus *bus);

/* Begin a single-shot high-precision measurement: write the measure command
   byte. The response may only be read once at least
   SHT45_MEASUREMENT_DURATION_MS has elapsed (enforced cooperatively by the
   runtime); reading earlier NACKs. */
DriverStatus SHT45_BeginMeasurement(Sht45 *dev);

/* Read and decode the 6-byte measurement response. Both CRCs are validated;
   if either fails the WHOLE sample is rejected (measurement->valid=false) and
   DRIVER_STATUS_CRC_ERROR is returned. Temperature is decoded as
   T = -45 + 175 * ST / 65535; RH = -6 + 125 * SRH / 65535 (clamped to 0..100). */
DriverStatus SHT45_FinishMeasurement(Sht45 *dev, Sht45Measurement *measurement);

/* Soft reset (0x94). Useful to recover an unresponsive sensor; no heater active.
   The runtime waits SHT45_RESET_DURATION_MS before re-probing. */
DriverStatus SHT45_SoftReset(Sht45 *dev);

#endif