#include "sht45.h"
#include <string.h>

/* SHT4x command framing: a single command byte. There is no boot-cycle / address
   ambiguity: each command is exactly one byte written MSB-first (a lone byte). */
static DriverStatus SendCommand(const Sht45 *dev, uint8_t cmd)
{
    if (dev == NULL || dev->bus == NULL)
        return DRIVER_STATUS_INVALID_ARG;
    return I2cBus_Write(dev->bus, dev->address, &cmd, sizeof(cmd));
}

uint8_t SHT45_Crc8(const uint8_t *data, size_t count)
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

/* Validate one 16-bit word (2 bytes) followed by its CRC byte, MSB-first
   (SHT4x transmits T-word then CRC then RH-word then CRC, each word big-endian).
   Returns the word on success, or DRIVER_STATUS_CRC_ERROR via *status. */
static uint16_t ReadWordWithCrc(const uint8_t *p, DriverStatus *status)
{
    uint8_t crc = SHT45_Crc8(&p[0], 2U);
    if (crc != p[2])
    {
        *status = DRIVER_STATUS_CRC_ERROR;
        return 0U;
    }
    *status = DRIVER_STATUS_OK;
    return (uint16_t)(((uint16_t)p[0] << 8U) | (uint16_t)p[1]);
}

DriverStatus SHT45_Init(Sht45 *dev, const I2cBus *bus)
{
    if (dev == NULL || bus == NULL)
        return DRIVER_STATUS_INVALID_ARG;

    memset(dev, 0, sizeof(*dev));
    dev->bus = bus;
    /* Left-shift the official 7-bit address into the bus's 8-bit byte-address
       convention (matching VEML7700/Display/SCD41 and the STM32 HAL). */
    dev->address = (uint16_t)(SHT45_I2C_ADDR << 1U);
    dev->initialized = 1U;
    return DRIVER_STATUS_OK;
}

DriverStatus SHT45_Probe(const I2cBus *bus)
{
    if (bus == NULL)
        return DRIVER_STATUS_INVALID_ARG;
    return I2cBus_Probe(bus, (uint16_t)(SHT45_I2C_ADDR << 1U));
}

DriverStatus SHT45_BeginMeasurement(Sht45 *dev)
{
    if (dev == NULL)
        return DRIVER_STATUS_INVALID_ARG;
    if (dev->initialized == 0U)
        return DRIVER_STATUS_NOT_READY;
    return SendCommand(dev, SHT45_CMD_MEASURE_HPM);
}

DriverStatus SHT45_FinishMeasurement(Sht45 *dev, Sht45Measurement *measurement)
{
    if (dev == NULL || measurement == NULL)
        return DRIVER_STATUS_INVALID_ARG;
    if (dev->initialized == 0U)
        return DRIVER_STATUS_NOT_READY;

    /* The measurement buffer is only committed when BOTH words pass CRC. */
    measurement->valid = false;

    uint8_t raw[SHT45_MEASUREMENT_RESPONSE_LEN];
    DriverStatus s = I2cBus_Read(dev->bus, dev->address, raw, sizeof(raw));
    if (s != DRIVER_STATUS_OK)
        return s;

    DriverStatus c1, c2;
    uint16_t t  = ReadWordWithCrc(&raw[0], &c1);
    uint16_t rh = ReadWordWithCrc(&raw[3], &c2);

    /* Reject the WHOLE sample if EITHER word's CRC fails: never publish a
       half-new / half-old T-RH pair. */
    if (c1 != DRIVER_STATUS_OK)
        return c1;
    if (c2 != DRIVER_STATUS_OK)
        return c2;

    /* SHT4x conversions (datasheet §4.6):
         T[degC]  = -45 + 175 * ST  / 65535
         RH[%]    =  -6 + 125 * SRH / 65535, clamped to [0,100] */
    float temp = -45.0f + 175.0f * (float)t / 65535.0f;
    float hum  = -6.0f + 125.0f * (float)rh / 65535.0f;
    if (hum < 0.0f) hum = 0.0f;
    if (hum > 100.0f) hum = 100.0f;

    measurement->temperature_c = temp;
    measurement->relative_humidity_pct = hum;
    measurement->valid = true;
    return DRIVER_STATUS_OK;
}

DriverStatus SHT45_SoftReset(Sht45 *dev)
{
    if (dev == NULL)
        return DRIVER_STATUS_INVALID_ARG;
    if (dev->initialized == 0U)
        return DRIVER_STATUS_NOT_READY;
    return SendCommand(dev, SHT45_CMD_SOFT_RESET);
}