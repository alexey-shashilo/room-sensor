#include "fake_i2c_bus.h"
#include <string.h>

/* SCD4x command IDs (independent copies so the fake has no driver dependency;
   test_scd41 cross-checks the two CRCs agree). */
#define SCD41_FAKE_CMD_GET_DATA_READY  0xE4B8U
#define SCD41_FAKE_CMD_READ_MEASUREMENT 0xEC05U

static DriverStatus fake_write(void *ctx, uint16_t addr, const uint8_t *data, size_t size)
{
    (void)addr;
    FakeI2cBus *f = (FakeI2cBus *)ctx;
    f->last_addr = addr;
    f->write_call_count++;
    if (size >= 1)
    {
        uint8_t reg = data[0];
        f->last_write_reg = reg;
        if (size >= 3)
        {
            /* Legacy register-mapped write: LSB at data[1], MSB at data[2]. */
            uint16_t val = (uint16_t)data[2] << 8U | (uint16_t)data[1];
            f->last_write_value = val;
            f->regs[reg] = data[1];
            f->regs[reg + 1] = data[2];
        }
    }
    /* Retain full command bytes (SCD41 sends a 2-byte command). */
    f->last_write_size = (size < sizeof(f->last_write_data)) ? size : sizeof(f->last_write_data);
    if (f->last_write_size > 0 && data != NULL)
        memcpy(f->last_write_data, data, f->last_write_size);

    /* Decode the SCD41 command (big-endian 2-byte word). */
    if (f->last_write_size == 2U)
        f->last_scd41_cmd = (uint16_t)(((uint16_t)f->last_write_data[0] << 8U) |
                                       (uint16_t)f->last_write_data[1]);
    return f->write_result;
}

static DriverStatus fake_read_mem(void *ctx, uint16_t addr, uint8_t reg, uint8_t *data, size_t size)
{
    (void)addr;
    FakeI2cBus *f = (FakeI2cBus *)ctx;
    f->read_mem_call_count++;
    if (size > 256) size = 256;
    memcpy(data, &f->regs[reg], size);
    return f->read_mem_result;
}

static DriverStatus fake_read(void *ctx, uint16_t addr, uint8_t *data, size_t size)
{
    (void)addr;
    FakeI2cBus *f = (FakeI2cBus *)ctx;
    f->last_addr = addr;
    f->read_call_count++;

    /* After GET_DATA_READY (0xE4B8), a 3-byte read returns the data-ready word
       followed by its CRC. The 16-bit word is transmitted MSB-first (the
       official SCD4x wire order). */
    if (f->last_scd41_cmd == SCD41_FAKE_CMD_GET_DATA_READY && size == 3U)
    {
        data[0] = (uint8_t)(f->data_ready_word >> 8U);   /* MSB */
        data[1] = (uint8_t)(f->data_ready_word & 0xFFU); /* LSB */
        uint8_t crc_in[2] = { data[0], data[1] };
        data[2] = FakeI2cBus_Scd41Crc(crc_in, 2U);
        return f->read_result;
    }

    /* After READ_MEASUREMENT (0xEC05), return the scripted 9-byte response. */
    size_t n = size;
    if (n > f->read_response_size) n = f->read_response_size;
    if (n > sizeof(f->read_response)) n = sizeof(f->read_response);
    if (n > 0)
        memcpy(data, f->read_response, n);
    if (n < size)
        memset(data + n, 0, size - n);
    return f->read_result;
}

static DriverStatus fake_probe(void *ctx, uint16_t addr)
{
    (void)addr;
    FakeI2cBus *f = (FakeI2cBus *)ctx;
    f->last_addr = addr;
    f->probe_call_count++;
    return f->probe_result;
}

void FakeI2cBus_Init(FakeI2cBus *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->write_result = DRIVER_STATUS_OK;
    fake->read_mem_result = DRIVER_STATUS_OK;
    fake->read_result = DRIVER_STATUS_OK;
    fake->probe_result = DRIVER_STATUS_OK;
}

void FakeI2cBus_GetBus(I2cBus *bus, FakeI2cBus *fake)
{
    bus->context = fake;
    bus->write = fake_write;
    bus->read_mem = fake_read_mem;
    bus->read = fake_read;
    bus->probe = fake_probe;
}

void FakeI2cBus_SetAlsRead(FakeI2cBus *fake, uint16_t raw)
{
    fake->regs[0x04] = (uint8_t)(raw & 0xFFU);
    fake->regs[0x05] = (uint8_t)(raw >> 8U);
}

void FakeI2cBus_SetScd41DataReady(FakeI2cBus *fake, bool ready)
{
    fake->data_ready_scripted = ready ? 1U : 0U;
    fake->data_ready_word = ready ? 0x0400U : 0x0000U;
}

void FakeI2cBus_SetScd41RawDataReady(FakeI2cBus *fake, uint8_t msb, uint8_t lsb)
{
    fake->data_ready_word = (uint16_t)(((uint16_t)msb << 8U) | (uint16_t)lsb);
}

void FakeI2cBus_SetScd41RawRead(FakeI2cBus *fake, const uint8_t raw9[9])
{
    memcpy(fake->read_response, raw9, 9);
    fake->read_response_size = 9U;
}

void FakeI2cBus_SetRawRead(FakeI2cBus *fake, const uint8_t *data, size_t size)
{
    if (size > sizeof(fake->read_response))
        size = sizeof(fake->read_response);
    memcpy(fake->read_response, data, size);
    fake->read_response_size = size;
}

void FakeI2cBus_SetScd41Measurement(FakeI2cBus *fake,
                                    uint16_t co2,
                                    uint16_t temp_raw,
                                    uint16_t rh_raw,
                                    bool corrupt_co2,
                                    bool corrupt_temp,
                                    bool corrupt_rh)
{
    uint8_t *buf = fake->read_response;
    /* Each SCD4x 16-bit word is transmitted MSB-first, followed by its CRC. */
    buf[0] = (uint8_t)(co2 >> 8U);           /* CO2 MSB */
    buf[1] = (uint8_t)(co2 & 0xFFU);         /* CO2 LSB */
    buf[2] = FakeI2cBus_Scd41Crc(&buf[0], 2U);
    buf[3] = (uint8_t)(temp_raw >> 8U);      /* T MSB */
    buf[4] = (uint8_t)(temp_raw & 0xFFU);    /* T LSB */
    buf[5] = FakeI2cBus_Scd41Crc(&buf[3], 2U);
    buf[6] = (uint8_t)(rh_raw >> 8U);        /* RH MSB */
    buf[7] = (uint8_t)(rh_raw & 0xFFU);      /* RH LSB */
    buf[8] = FakeI2cBus_Scd41Crc(&buf[6], 2U);

    if (corrupt_co2) buf[2] ^= 0xFFU;
    if (corrupt_temp) buf[5] ^= 0xFFU;
    if (corrupt_rh) buf[8] ^= 0xFFU;

    fake->read_response_size = 9U;
}

uint8_t FakeI2cBus_Scd41Crc(uint8_t *data, size_t count)
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

uint16_t FakeI2cBus_TempRaw(float temp_c)
{
    return (uint16_t)((temp_c + 45.0f) * 65535.0f / 175.0f);
}

uint16_t FakeI2cBus_RhRaw(float rh_pct)
{
    return (uint16_t)(rh_pct * 65535.0f / 100.0f);
}