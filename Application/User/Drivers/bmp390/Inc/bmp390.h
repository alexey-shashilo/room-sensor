#ifndef BMP390_H
#define BMP390_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "room_sensor_types.h"
#include "i2c_bus.h"

/* BMP390 (Bosch Sensortec BMP3 family) barometric pressure sensor.

   Portable driver. Depends ONLY on the I2cBus * abstraction — never on the
   STM32 HAL, App, RoomState, Telemetry, Display or Command layers. Timing and
   lifecycle (forced-measurement state machine) live in the runtime / App, so no
   operation here blocks the watchdog.

   All protocol constants and the compensation math follow the PRIMARY Bosch
   sources: the BMP390 datasheet (v1.3) and the official Bosch BMP3_SensorAPI
   (v2.0.6, boschsensortec/BMP3_SensorAPI), BMP3_FLOAT_COMPENSATION path.

   The I2cBus abstraction expects the address already left-shifted into 8-bit
   form (bit0 = R/W), same as VEML7700/Display/SCD41/SHT45. The driver stores
   the DETECTED wire address in the handle. */

/* 7-bit I2C addresses (BMP390 datasheet / BMP3_SensorAPI):
   primary 0x76, secondary 0x77. Shifted -> 0xEC / 0xEE. */
#define BMP390_I2C_ADDR_PRIM   (0x76U)   /* 7-bit; wire byte 0xEC */
#define BMP390_I2C_ADDR_SEC    (0x77U)   /* 7-bit; wire byte 0xEE */

/* Chip ID for BMP390 (BMP3_SensorAPI BMP390_CHIP_ID). */
#define BMP390_CHIP_ID         (0x60U)

/* Register map (BMP3_SensorAPI bmp3_defs.h). */
#define BMP390_REG_CHIP_ID     0x00U
#define BMP390_REG_ERR         0x02U
#define BMP390_REG_STATUS      0x03U
#define BMP390_REG_DATA        0x04U
#define BMP390_REG_PWR_CTRL    0x1BU
#define BMP390_REG_OSR         0x1CU
#define BMP390_REG_ODR         0x1DU
#define BMP390_REG_CONFIG      0x1FU
#define BMP390_REG_CALIB_DATA  0x31U
#define BMP390_REG_CMD         0x7EU

/* Error bits (BMP3_ERR_*). */
#define BMP390_ERR_FATAL       0x01U
#define BMP390_ERR_CMD         0x02U
#define BMP390_ERR_CONF        0x04U

/* Status bits (BMP3_STATUS_*). */
#define BMP390_STATUS_CMD_RDY   0x10U
#define BMP390_STATUS_DRDY_PRESS 0x20U
#define BMP390_STATUS_DRDY_TEMP 0x40U

/* Power modes (BMP3_MODE_*). */
#define BMP390_MODE_SLEEP       0x00U
#define BMP390_MODE_FORCED      0x01U
#define BMP390_MODE_NORMAL      0x03U

/* Soft reset command (BMP3_SOFT_RESET). */
#define BMP390_SOFT_RESET_CMD   0xB6U

/* ODR code for 25 Hz (BMP3_ODR_25_HZ). Only meaningful in NORMAL mode; written
   once for completeness but FORCED mode is used for the v1 room profile. */
#define BMP390_ODR_25_HZ        0x01U

/* Calibration block length (BMP3_LEN_CALIB_DATA = 21 bytes). */
#define BMP390_LEN_CALIB_DATA   21U

/* Raw P+T data length: pressure 24-bit (3) + temperature 24-bit (3) = 6. */
#define BMP390_LEN_P_T_DATA     6U

/* Defensive operating range (Bosch BMP3 datasheet): 300..1250 hPa (Pa). Used
   ONLY for sample validation — an out-of-range compensated value marks the
   sample invalid; it is never clamped onto a boundary. */
#define BMP390_MIN_PRES_PA      30000.0
#define BMP390_MAX_PRES_PA      125000.0

/* Calibration scale factors (exact quotients used by Bosch BMP3_SensorAPI). */
typedef struct
{
    double par_t1;
    double par_t2;
    double par_t3;
    double par_p1;
    double par_p2;
    double par_p3;
    double par_p4;
    double par_p5;
    double par_p6;
    double par_p7;
    double par_p8;
    double par_p9;
    double par_p10;
    double par_p11;
    double t_lin;   /* last compensated temperature (needed for pressure) */
} Bmp390QuantizedCalib;

typedef struct
{
    /* Parsed (quantized) coefficients, ready for compensation. */
    Bmp390QuantizedCalib quantized;

    /* Flag: calibration loaded at least once after identity verification. */
    uint8_t calibrated;
} Bmp390Calib;

typedef struct
{
    const I2cBus *bus;
    uint16_t address;     /* detected wire byte address (0xEC or 0xEE) */
    uint8_t  initialized;
    Bmp390Calib calib;
} Bmp390;

/* One compensated sample. Pressure in Pa, temperature in deg C. */
typedef struct
{
    float pressure_pa;
    float temperature_c;
    /* True only when a fully data-ready, in-range sample was accepted. */
    bool valid;
} Bmp390Sample;

/* Init the driver handle against a bus (address is detected at Probe/Init). */
DriverStatus BMP390_Init(Bmp390 *dev, const I2cBus *bus);

/* Detect the BMP390: probe 0x76 then 0x77; require an ACK AND CHIP_ID == 0x60.
   Stores the detected wire address in dev->address. Only accepts the sensor when
   both the address responds and the chip ID matches. */
DriverStatus BMP390_Detect(Bmp390 *dev);

/* Read the current CHIP_ID register. */
DriverStatus BMP390_ReadChipId(Bmp390 *dev, uint8_t *chip_id);

/* Verify identity (CHIP_ID == 0x60) and load the 21-byte calibration block.
   Calibration is parsed once after identity verification. */
DriverStatus BMP390_InitCalibration(Bmp390 *dev);

/* Read the error register; maps device-level faults (fatal/cmd/conf) to
   DRIVER_STATUS_DEVICE_ERROR without exposing raw bits to App. */
DriverStatus BMP390_ReadError(Bmp390 *dev, uint8_t *err);

/* Configure the v1 room-sensor operating profile (see bmp390_runtime.h):
   forced mode, pressure + temperature enabled, conservative oversampling /
   ODR / IIR. */
DriverStatus BMP390_ConfigureRoomProfile(Bmp390 *dev);

/* Trigger a single forced measurement (write FORCED mode; sensor returns to
   sleep on completion). Non-blocking (single register write). */
DriverStatus BMP390_TriggerMeasurement(Bmp390 *dev);

/* Read the status register data-ready bits. */
DriverStatus BMP390_ReadStatus(Bmp390 *dev, uint8_t *status);

/* Read the raw P and T 24-bit values and compensate them using the official
   Bosch float algorithm into *sample. Both raw channels come from one atomic
   6-byte read; the sample is only committed when the caller has verified both
   data-ready bits. */
DriverStatus BMP390_ReadSample(Bmp390 *dev, Bmp390Sample *sample);

/* Soft reset (0xB6 to CMD), driver-level; runtime decides whether to use it. */
DriverStatus BMP390_SoftReset(Bmp390 *dev);

#endif