#ifndef BMP380_H
#define BMP380_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "room_sensor_types.h"
#include "i2c_bus.h"

/* BMP380 (Bosch Sensortec BMP3 family) barometric pressure sensor.

   Separate production driver for the BMP380 part (CHIP_ID 0x50). It does NOT
   weaken or share the BMP390 identity contract: this driver accepts ONLY the
   BMP380 CHIP_ID, and the existing BMP390 driver continues to accept ONLY the
   BMP390 CHIP_ID (0x60). The two parts are register/compensation-identical in
   the official Bosch BMP3_SensorAPI v2.0.6, differing only in CHIP_ID; this
   driver is nonetheless kept as an independent module so neither identity may
   be silently widened.

   Portable driver. Depends ONLY on the I2cBus * abstraction — never on the
   STM32 HAL, App, RoomState, Telemetry, Display or Command layers. Timing and
   lifecycle (forced-measurement state machine) live in the runtime / App, so no
   operation here blocks the watchdog.

   All protocol constants and the compensation math follow the PRIMARY Bosch
   sources: the BMP380 datasheet and the official Bosch BMP3_SensorAPI
   (v2.0.6, boschsensortec/BMP3_SensorAPI), BMP3_FLOAT_COMPENSATION path. Bosch
   v2.0.6 serves BMP380 and BMP390 from ONE shared register map / calibration /
   compensation; the only functional difference is CHIP_ID (BMP380 = 0x50,
   BMP390 = 0x60). This was verified against the official repository.

   The I2cBus abstraction expects the address already left-shifted into 8-bit
   form (bit0 = R/W), same as VEML7700/Display/SCD41/SHT45/BMP390. The driver
   stores the DETECTED wire address in the handle. */

/* 7-bit I2C addresses (BMP3_SensorAPI): primary 0x76, secondary 0x77.
   Shifted -> 0xEC / 0xEE. */
#define BMP380_I2C_ADDR_PRIM   (0x76U)   /* 7-bit; wire byte 0xEC */
#define BMP380_I2C_ADDR_SEC    (0x77U)   /* 7-bit; wire byte 0xEE */

/* Chip ID for BMP380 (BMP3_SensorAPI BMP3_CHIP_ID). */
#define BMP380_CHIP_ID         (0x50U)

/* Register map (BMP3_SensorAPI bmp3_defs.h). */
#define BMP380_REG_CHIP_ID     0x00U
#define BMP380_REG_ERR         0x02U
#define BMP380_REG_STATUS      0x03U
#define BMP380_REG_DATA        0x04U
#define BMP380_REG_PWR_CTRL    0x1BU
#define BMP380_REG_OSR         0x1CU
#define BMP380_REG_ODR         0x1DU
#define BMP380_REG_CONFIG      0x1FU
#define BMP380_REG_CALIB_DATA  0x31U
#define BMP380_REG_CMD         0x7EU

/* Error bits (BMP3_ERR_*). */
#define BMP380_ERR_FATAL       0x01U
#define BMP380_ERR_CMD         0x02U
#define BMP380_ERR_CONF        0x04U

/* Status bits (BMP3_STATUS_*). */
#define BMP380_STATUS_CMD_RDY   0x10U
#define BMP380_STATUS_DRDY_PRESS 0x20U
#define BMP380_STATUS_DRDY_TEMP 0x40U

/* Power modes (BMP3_MODE_*). */
#define BMP380_MODE_SLEEP       0x00U
#define BMP380_MODE_FORCED      0x01U
#define BMP380_MODE_NORMAL      0x03U

/* Soft reset command (BMP3_SOFT_RESET). */
#define BMP380_SOFT_RESET_CMD   0xB6U

/* ODR code for 25 Hz (BMP3_ODR_25_HZ). Only meaningful in NORMAL mode; written
   once for completeness but FORCED mode is used for the v1 room profile. */
#define BMP380_ODR_25_HZ        0x01U

/* Calibration block length (BMP3_LEN_CALIB_DATA = 21 bytes). */
#define BMP380_LEN_CALIB_DATA   21U

/* Raw P+T data length: pressure 24-bit (3) + temperature 24-bit (3) = 6. */
#define BMP380_LEN_P_T_DATA     6U

/* Defensive operating range (Bosch BMP3 datasheet): 300..1250 hPa (Pa). Used
   ONLY for sample validation — an out-of-range compensated value marks the
   sample invalid; it is never clamped onto a boundary. */
#define BMP380_MIN_PRES_PA      30000.0
#define BMP380_MAX_PRES_PA      125000.0

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
} Bmp380QuantizedCalib;

typedef struct
{
    Bmp380QuantizedCalib quantized;
    uint8_t calibrated;
} Bmp380Calib;

typedef struct
{
    const I2cBus *bus;
    uint16_t address;     /* detected wire byte address (0xEC or 0xEE) */
    uint8_t  initialized;
    Bmp380Calib calib;
} Bmp380;

typedef struct
{
    float pressure_pa;
    float temperature_c;
    bool valid;
} Bmp380Sample;

DriverStatus BMP380_Init(Bmp380 *dev, const I2cBus *bus);
DriverStatus BMP380_Detect(Bmp380 *dev);
DriverStatus BMP380_ReadChipId(Bmp380 *dev, uint8_t *chip_id);
DriverStatus BMP380_InitCalibration(Bmp380 *dev);
DriverStatus BMP380_ReadError(Bmp380 *dev, uint8_t *err);
DriverStatus BMP380_ConfigureRoomProfile(Bmp380 *dev);
DriverStatus BMP380_TriggerMeasurement(Bmp380 *dev);
DriverStatus BMP380_ReadStatus(Bmp380 *dev, uint8_t *status);
DriverStatus BMP380_ReadSample(Bmp380 *dev, Bmp380Sample *sample);
DriverStatus BMP380_SoftReset(Bmp380 *dev);

#endif