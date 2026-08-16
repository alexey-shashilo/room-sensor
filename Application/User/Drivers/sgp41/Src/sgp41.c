#include "sgp41.h"
#include <string.h>

/* SGP41 command framing: a 2-byte command word (MSB first), optionally followed
   by 16-bit argument words (MSB first) each with their own CRC byte appended by
   the caller (measure/conditioning carry an RH word + its CRC, then a T word +
   its CRC). */
static DriverStatus SendCommandArgs(const Sgp41 *dev, uint16_t cmd,
                                    const uint8_t *args, size_t arg_len)
{
    if (dev == NULL || dev->bus == NULL)
        return DRIVER_STATUS_INVALID_ARG;
    if (dev->initialized == 0U)
        return DRIVER_STATUS_NOT_READY;

    uint8_t tx[1 + 2 + 2 + 2 + 1 + 1]; /* cmd(2) + up to 2 words + 2 CRCs */
    size_t pos = 0U;
    tx[pos++] = (uint8_t)(cmd >> 8U);
    tx[pos++] = (uint8_t)(cmd & 0xFFU);
    if (args != NULL && arg_len > 0U && (pos + arg_len) <= sizeof(tx))
    {
        memcpy(&tx[pos], args, arg_len);
        pos += arg_len;
    }
    return I2cBus_Write(dev->bus, dev->address, tx, pos);
}

/* Validate one 16-bit word (2 bytes, MSB-first) followed by its CRC byte.
   Returns the word on success, or DRIVER_STATUS_CRC_ERROR via *status if the
   CRC mismatches. Mirrors SCD41/SHT45 wire decoding: p[0] is MSB, p[1] LSB. */
static uint16_t ReadWordWithCrc(const uint8_t *p, DriverStatus *status)
{
    uint8_t crc = SGP41_Crc8(&p[0], 2U);
    if (crc != p[2])
    {
        *status = DRIVER_STATUS_CRC_ERROR;
        return 0U;
    }
    *status = DRIVER_STATUS_OK;
    return (uint16_t)(((uint16_t)p[0] << 8U) | (uint16_t)p[1]);
}

uint8_t SGP41_Crc8(const uint8_t *data, size_t count)
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

DriverStatus SGP41_Init(Sgp41 *dev, const I2cBus *bus)
{
    if (dev == NULL || bus == NULL)
        return DRIVER_STATUS_INVALID_ARG;

    memset(dev, 0, sizeof(*dev));
    dev->bus = bus;
    /* Left-shift the official 7-bit address into the bus's 8-bit byte-address
       convention (matching SCD41/SHT45/Display and the STM32 HAL). */
    dev->address = (uint16_t)(SGP41_I2C_ADDR << 1U);
    dev->initialized = 1U;
    return DRIVER_STATUS_OK;
}

DriverStatus SGP41_Probe(const I2cBus *bus)
{
    if (bus == NULL)
        return DRIVER_STATUS_INVALID_ARG;
    return I2cBus_Probe(bus, (uint16_t)(SGP41_I2C_ADDR << 1U));
}

/* Compose the argument bytes for measure/conditioning: RH word + CRC, then T
   word + CRC (MSB-first words). */
static void BuildCompArg(uint16_t rh_ticks, uint16_t t_ticks, uint8_t out[6])
{
    out[0] = (uint8_t)(rh_ticks >> 8U);
    out[1] = (uint8_t)(rh_ticks & 0xFFU);
    out[2] = SGP41_Crc8(&out[0], 2U);
    out[3] = (uint8_t)(t_ticks >> 8U);
    out[4] = (uint8_t)(t_ticks & 0xFFU);
    out[5] = SGP41_Crc8(&out[3], 2U);
}

DriverStatus SGP41_BeginMeasure(Sgp41 *dev, uint16_t rh_ticks, uint16_t t_ticks)
{
    uint8_t args[6];
    BuildCompArg(rh_ticks, t_ticks, args);
    return SendCommandArgs(dev, SGP41_CMD_MEASURE_RAW_SIGNALS, args, sizeof(args));
}

DriverStatus SGP41_FinishMeasure(Sgp41 *dev, Sgp41RawMeasurement *measurement)
{
    if (dev == NULL || measurement == NULL)
        return DRIVER_STATUS_INVALID_ARG;
    if (dev->initialized == 0U)
        return DRIVER_STATUS_NOT_READY;

    /* The sample is only committed when ALL words pass CRC. */
    measurement->valid = false;

    uint8_t raw[SGP41_MEASURE_RESPONSE_BYTES];
    DriverStatus s = I2cBus_Read(dev->bus, dev->address, raw, sizeof(raw));
    if (s != DRIVER_STATUS_OK)
    {
        if (s == DRIVER_STATUS_INVALID_ARG || s == DRIVER_STATUS_NOT_SUPPORTED)
            return s;
        /* A read-before-ready / transport failure is reported as-is. */
        return s;
    }

    DriverStatus c1, c2;
    uint16_t voc = ReadWordWithCrc(&raw[0], &c1);
    uint16_t nox = ReadWordWithCrc(&raw[3], &c2);

    /* Reject the WHOLE sample if ANY word's CRC fails. */
    if (c1 != DRIVER_STATUS_OK)
        return c1;
    if (c2 != DRIVER_STATUS_OK)
        return c2;

    measurement->raw_voc = voc;
    measurement->raw_nox = nox;
    measurement->valid = true;
    return DRIVER_STATUS_OK;
}

DriverStatus SGP41_BeginConditioning(Sgp41 *dev, uint16_t rh_ticks, uint16_t t_ticks)
{
    uint8_t args[6];
    BuildCompArg(rh_ticks, t_ticks, args);
    return SendCommandArgs(dev, SGP41_CMD_EXECUTE_CONDITIONING, args, sizeof(args));
}

DriverStatus SGP41_FinishConditioning(Sgp41 *dev, uint16_t *raw_voc)
{
    if (dev == NULL || raw_voc == NULL)
        return DRIVER_STATUS_INVALID_ARG;
    if (dev->initialized == 0U)
        return DRIVER_STATUS_NOT_READY;

    uint8_t raw[SGP41_CONDITIONING_RESPONSE_BYTES];
    DriverStatus s = I2cBus_Read(dev->bus, dev->address, raw, sizeof(raw));
    if (s != DRIVER_STATUS_OK)
        return s;

    DriverStatus c1;
    uint16_t voc = ReadWordWithCrc(&raw[0], &c1);
    if (c1 != DRIVER_STATUS_OK)
        return c1;
    *raw_voc = voc;
    return DRIVER_STATUS_OK;
}

DriverStatus SGP41_BeginSelfTest(Sgp41 *dev)
{
    return SendCommandArgs(dev, SGP41_CMD_EXECUTE_SELF_TEST, NULL, 0U);
}

DriverStatus SGP41_FinishSelfTest(Sgp41 *dev, uint8_t *test_result)
{
    if (dev == NULL || test_result == NULL)
        return DRIVER_STATUS_INVALID_ARG;
    if (dev->initialized == 0U)
        return DRIVER_STATUS_NOT_READY;

    uint8_t raw[SGP41_SELF_TEST_RESPONSE_BYTES];
    DriverStatus s = I2cBus_Read(dev->bus, dev->address, raw, sizeof(raw));
    if (s != DRIVER_STATUS_OK)
        return s;

    DriverStatus c1;
    uint16_t word = ReadWordWithCrc(&raw[0], &c1);
    if (c1 != DRIVER_STATUS_OK)
        return c1;

    /* Low 4 bits of LSB indicate per-pixel pass; all zero = passed. */
    *test_result = (uint8_t)(word & 0x0FU);
    return DRIVER_STATUS_OK;
}

DriverStatus SGP41_TurnHeaterOff(Sgp41 *dev)
{
    return SendCommandArgs(dev, SGP41_CMD_TURN_HEATER_OFF, NULL, 0U);
}

DriverStatus SGP41_BeginGetSerial(Sgp41 *dev)
{
    return SendCommandArgs(dev, SGP41_CMD_GET_SERIAL_NUMBER, NULL, 0U);
}

DriverStatus SGP41_FinishGetSerial(Sgp41 *dev, uint16_t serial[3])
{
    if (dev == NULL || serial == NULL)
        return DRIVER_STATUS_INVALID_ARG;
    if (dev->initialized == 0U)
        return DRIVER_STATUS_NOT_READY;

    uint8_t raw[SGP41_SERIAL_RESPONSE_BYTES];
    DriverStatus s = I2cBus_Read(dev->bus, dev->address, raw, sizeof(raw));
    if (s != DRIVER_STATUS_OK)
        return s;

    DriverStatus c1, c2, c3;
    uint16_t w0 = ReadWordWithCrc(&raw[0], &c1);
    uint16_t w1 = ReadWordWithCrc(&raw[3], &c2);
    uint16_t w2 = ReadWordWithCrc(&raw[6], &c3);
    if (c1 != DRIVER_STATUS_OK) return c1;
    if (c2 != DRIVER_STATUS_OK) return c2;
    if (c3 != DRIVER_STATUS_OK) return c3;

    serial[0] = w0;
    serial[1] = w1;
    serial[2] = w2;
    return DRIVER_STATUS_OK;
}