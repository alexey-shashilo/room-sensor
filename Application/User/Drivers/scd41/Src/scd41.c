#include "scd41.h"
#include <string.h>

/* SCD4x command framing: a 2-byte command word (MSB first). Commands that take
   a payload append the word's CRC-8 (none used by the periodic path here). */
static DriverStatus SendCommand(const Scd41 *dev, uint16_t cmd)
{
    if (dev == NULL || dev->bus == NULL)
        return DRIVER_STATUS_INVALID_ARG;

    uint8_t tx[2];
    tx[0] = (uint8_t)(cmd >> 8U);
    tx[1] = (uint8_t)(cmd & 0xFFU);
    return I2cBus_Write(dev->bus, dev->address, tx, sizeof(tx));
}

uint8_t SCD41_Crc8(const uint8_t *data, size_t count)
{
    uint8_t crc = 0xFFU;
    for (size_t i = 0; i < count; i++)
    {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8U; j++)
            crc = (crc & 0x80U) ? (uint8_t)((crc << 1U) ^ 0x31U) : (uint8_t)(crc << 1U);
    }
    return crc;
}

/* Validate one 16-bit word (2 bytes) followed by its CRC byte. Returns the word
   on success, or DRIVER_STATUS_CRC_ERROR via *status if the CRC mismatches.

   SCD4x transmits each 16-bit word MSB-first (big-endian): p[0] is the most
   significant byte, p[1] the least significant. A previous implementation
   decoded word = ((uint16_t)p[1] << 8U) | p[0] i.e. treated the first received
   byte as LSB, which is wrong for the SCD4x wire protocol. */
static uint16_t ReadWordWithCrc(const uint8_t *p, DriverStatus *status)
{
    uint8_t crc = SCD41_Crc8(&p[0], 2U);
    if (crc != p[2])
    {
        *status = DRIVER_STATUS_CRC_ERROR;
        return 0U;
    }
    *status = DRIVER_STATUS_OK;
    return (uint16_t)(((uint16_t)p[0] << 8U) | (uint16_t)p[1]);
}

DriverStatus SCD41_Init(Scd41 *dev, const I2cBus *bus)
{
    if (dev == NULL || bus == NULL)
        return DRIVER_STATUS_INVALID_ARG;

    memset(dev, 0, sizeof(*dev));
    dev->bus = bus;
    /* Left-shift the official 7-bit address into the bus's 8-bit byte-address
       convention (matching VEML7700/Display and the STM32 HAL). */
    dev->address = (uint16_t)(SCD41_I2C_ADDR << 1U);
    dev->initialized = 1U;
    return DRIVER_STATUS_OK;
}

DriverStatus SCD41_Probe(const I2cBus *bus)
{
    if (bus == NULL)
        return DRIVER_STATUS_INVALID_ARG;
    return I2cBus_Probe(bus, (uint16_t)(SCD41_I2C_ADDR << 1U));
}

DriverStatus SCD41_StartPeriodicMeasurement(Scd41 *dev)
{
    return SendCommand(dev, SCD41_CMD_START_PERIODIC);
}

DriverStatus SCD41_StopPeriodicMeasurement(Scd41 *dev)
{
    return SendCommand(dev, SCD41_CMD_STOP_PERIODIC);
}

DriverStatus SCD41_GetDataReady(Scd41 *dev, bool *ready)
{
    if (dev == NULL || ready == NULL)
        return DRIVER_STATUS_INVALID_ARG;
    if (dev->initialized == 0U)
        return DRIVER_STATUS_NOT_READY;

    DriverStatus s;
    s = SendCommand(dev, SCD41_CMD_GET_DATA_READY);
    if (s != DRIVER_STATUS_OK)
        return s;

    uint8_t raw[3];
    s = I2cBus_Read(dev->bus, dev->address, raw, sizeof(raw));
    if (s != DRIVER_STATUS_OK)
        return s;

    DriverStatus cstatus;
    uint16_t word = ReadWordWithCrc(raw, &cstatus);
    if (cstatus != DRIVER_STATUS_OK)
        return cstatus;

    *ready = (word & SCD41_DATA_READY_MASK) != 0U;
    return DRIVER_STATUS_OK;
}

DriverStatus SCD41_ReadMeasurement(Scd41 *dev, Scd41Measurement *measurement)
{
    if (dev == NULL || measurement == NULL)
        return DRIVER_STATUS_INVALID_ARG;
    if (dev->initialized == 0U)
        return DRIVER_STATUS_NOT_READY;

    /* The measurement buffer is only committed when ALL words pass CRC. */
    measurement->valid = false;

    DriverStatus s;
    s = SendCommand(dev, SCD41_CMD_READ_MEASUREMENT);
    if (s != DRIVER_STATUS_OK)
        return s;

    uint8_t raw[9];
    s = I2cBus_Read(dev->bus, dev->address, raw, sizeof(raw));
    if (s != DRIVER_STATUS_OK)
        return s;

    DriverStatus c1, c2, c3;
    uint16_t co2 = ReadWordWithCrc(&raw[0], &c1);
    uint16_t t   = ReadWordWithCrc(&raw[3], &c2);
    uint16_t rh  = ReadWordWithCrc(&raw[6], &c3);

    /* Reject the WHOLE sample if ANY word's CRC fails. */
    if (c1 != DRIVER_STATUS_OK)
        return c1;
    if (c2 != DRIVER_STATUS_OK)
        return c2;
    if (c3 != DRIVER_STATUS_OK)
        return c3;

    /* SCD4x conversions (datasheet):
         CO2[ppm] = co2
         T[degC]  = -45 + 175 * t/65535
         RH[%]    = 100 * rh/65535  */
    measurement->co2_ppm = co2;
    measurement->temperature_c = -45.0f + 175.0f * (float)t / 65535.0f;
    measurement->relative_humidity_pct = 100.0f * (float)rh / 65535.0f;
    measurement->valid = true;
    return DRIVER_STATUS_OK;
}