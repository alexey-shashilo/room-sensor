#include "fake_i2c_bus.h"
#include "platform_time.h"
#include <string.h>

/* SCD4x command IDs (independent copies so the fake has no driver dependency;
   test_scd41 cross-checks the two CRCs agree). */
#define SCD41_FAKE_CMD_GET_DATA_READY  0xE4B8U
#define SCD41_FAKE_CMD_READ_MEASUREMENT 0xEC05U
#define SCD41_FAKE_CMD_START_PERIODIC  0x21B1U
#define SCD41_FAKE_CMD_STOP_PERIODIC   0x3F86U

#define SCD41_FAKE_WIRE_ADDR   (0xC4U)   /* SCD41 7-bit 0x62 left-shifted */
#define SHT45_FAKE_WIRE_ADDR   (0x88U)   /* SHT45 7-bit 0x44 left-shifted */

/* SGP41 7-bit 0x59 left-shifted -> wire byte 0xB2. */
#define SGP41_FAKE_WIRE_ADDR   (0xB2U)

static DriverStatus fake_write(void *ctx, uint16_t addr, const uint8_t *data, size_t size)
{
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

    /* SGP41: a 2-byte MSB-first command word written to the SGP41 wire address.
       The measure/conditioning/self-test commands count so tests can prove the
       exact protocol sequence. */
    if (addr == SGP41_FAKE_WIRE_ADDR && f->last_write_size >= 2U)
    {
        f->sgp41_last_cmd = (uint16_t)(((uint16_t)f->last_write_data[0] << 8U) |
                                       (uint16_t)f->last_write_data[1]);
        if (f->sgp41_last_cmd == 0x2619U)
            f->sgp41_measure_cmd_count++;
    }

    /* SHT45: a single-byte command write (measure etc.). */
    if (addr == SHT45_FAKE_WIRE_ADDR && f->last_write_size == 1U)
    {
        f->sht45_last_cmd = f->last_write_data[0];
        if (f->sht45_last_cmd == 0xFDU)   /* high-precision measure */
            f->sht45_measure_cmd_count++;
    }

    /* Append to the SCD41 command log (SCD41 wire address only). */
    if (addr == SCD41_FAKE_WIRE_ADDR && f->last_scd41_cmd != 0U)
    {
        if (f->scd41_cmd_log_count < (int)(sizeof(f->scd41_cmd_log) / sizeof(f->scd41_cmd_log[0])))
            f->scd41_cmd_log[f->scd41_cmd_log_count] = f->last_scd41_cmd;
        f->scd41_cmd_log_count++;
    }

    /* Model SCD4x mode legality on the 2-byte command write. This reproduces the
       real retained-periodic behavior: a NACK-style failure is returned for
       start_periodic while the sensor is already PERIODIC. Only applies to the
       SCD41 wire address so display/VEML 2-byte writes are unaffected. The
       write_result override (when non-NONE) takes precedence so tests can still
       inject arbitrary transport failures. */
    if (f->last_scd41_cmd != 0U && addr == SCD41_FAKE_WIRE_ADDR)
    {
        switch (f->last_scd41_cmd)
        {
            case SCD41_FAKE_CMD_START_PERIODIC:
                if (f->scd41_mode == FAKE_SCD41_MODE_PERIODIC)
                    return DRIVER_STATUS_BUS_ERROR;   /* datasheet: refused while measuring */
                f->scd41_mode = FAKE_SCD41_MODE_PERIODIC;
                break;
            case SCD41_FAKE_CMD_STOP_PERIODIC:
                if (f->scd41_mode == FAKE_SCD41_MODE_PERIODIC)
                    f->scd41_mode = FAKE_SCD41_MODE_IDLE;
                break;
            default:
                break;
        }
    }

    if (f->write_result != DRIVER_STATUS_OK)
        return f->write_result;
    return DRIVER_STATUS_OK;
}

static DriverStatus fake_read_mem(void *ctx, uint16_t addr, uint8_t reg, uint8_t *data, size_t size)
{
    (void)addr;
    FakeI2cBus *f = (FakeI2cBus *)ctx;
    f->read_mem_call_count++;

    /* BMP390 relay: the shared flat regs[] map is used by VEML at reg 0x00
       (ALS_CONF), which collides with the BMP390 CHIP_ID at the same register.
       When a BMP390 wire address is configured, serve CHIP_ID (0x00) and
       CALIB_DATA (0x31) from dedicated fields so BMP390 app-level tests can run
       without corrupting VEML/display. */
    if (f->bmp390_wire_addr != 0 && addr == f->bmp390_wire_addr)
    {
        if (reg == 0x00U && size >= 1U && f->bmp390_chip_id != 0)
        {
            data[0] = f->bmp390_chip_id;
            return f->read_mem_result;
        }
        if (reg == 0x31U && f->bmp390_chip_id != 0)
        {
            size_t n = (size < 21U) ? size : 21U;
            memcpy(data, f->bmp390_calib, n);
            return f->read_mem_result;
        }
    }

    if (size > 256) size = 256;
    memcpy(data, &f->regs[reg], size);
    return f->read_mem_result;
}

static DriverStatus fake_read(void *ctx, uint16_t addr, uint8_t *data, size_t size)
{
    FakeI2cBus *f = (FakeI2cBus *)ctx;
    f->last_addr = addr;
    f->read_call_count++;
    f->last_read_tick_ms = Platform_GetTickMs();

    /* SHT45: a 6-byte read (T+CRC, RH+CRC). If the conversion is not complete
       (respond==false) the fake NACKs (BUS_ERROR), matching the datasheet
       "NACK to read header while busy". */
    if (addr == SHT45_FAKE_WIRE_ADDR && size == 6U)
    {
        if (!f->sht45_respond)
            return DRIVER_STATUS_BUS_ERROR;
        memcpy(data, f->sht45_read_response, 6);
        return f->read_result;
    }

    /* SGP41 (0xB2): a plain read returns the scripted response verbatim. */
    if (addr == SGP41_FAKE_WIRE_ADDR)
    {
        size_t n = size;
        if (n > f->sgp41_read_response_size)
            n = f->sgp41_read_response_size;
        if (n > sizeof(f->sgp41_read_response))
            n = sizeof(f->sgp41_read_response);
        if (n > 0)
            memcpy(data, f->sgp41_read_response, n);
        if (n < size)
            memset(data + n, 0, size - n);
        return f->sgp41_read_result;
    }

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
    FakeI2cBus *f = (FakeI2cBus *)ctx;
    f->last_addr = addr;
    f->probe_call_count++;

    /* SGP41-specific absence: a declared-absent SGP41 NACKs its own probe
       without disturbing the shared global probe_result used by others. */
    if (f->sgp41_absent && addr == SGP41_FAKE_WIRE_ADDR)
        return DRIVER_STATUS_NOT_FOUND;

    return f->probe_result;
}

static DriverStatus fake_recover(void *ctx)
{
    FakeI2cBus *f = (FakeI2cBus *)ctx;
    f->recover_call_count++;
    return f->recover_result;
}

void FakeI2cBus_Init(FakeI2cBus *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->write_result = DRIVER_STATUS_OK;
    fake->read_mem_result = DRIVER_STATUS_OK;
    fake->read_result = DRIVER_STATUS_OK;
    fake->probe_result = DRIVER_STATUS_OK;
    fake->recover_result = DRIVER_STATUS_OK;
    fake->sgp41_read_result = DRIVER_STATUS_OK;
}

void FakeI2cBus_GetBus(I2cBus *bus, FakeI2cBus *fake)
{
    bus->context = fake;
    bus->write = fake_write;
    bus->read_mem = fake_read_mem;
    bus->read = fake_read;
    bus->probe = fake_probe;
    bus->recover = fake_recover;
}

void FakeI2cBus_SetAlsRead(FakeI2cBus *fake, uint16_t raw)
{
    fake->regs[0x04] = (uint8_t)(raw & 0xFFU);
    fake->regs[0x05] = (uint8_t)(raw >> 8U);
}

void FakeI2cBus_SetScd41Mode(FakeI2cBus *fake, FakeScd41Mode mode)
{
    fake->scd41_mode = mode;
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

void FakeI2cBus_SetSht45Response(FakeI2cBus *fake, const uint8_t raw6[6], bool respond)
{
    if (fake == NULL || raw6 == NULL) return;
    memcpy(fake->sht45_read_response, raw6, 6);
    fake->sht45_respond = respond;
}

/* Present a BMP390 at `wire_addr` (left-shifted 0xEC or 0xEE) with the given
   chip id and 21-byte calibration block. wa=bmp390_wire_addr; the shared probe
   reports present, and read_mem relays CHIP_ID/CALIB_DATA from the dedicated
   fields (isolated from VEML's reg-0 usage). */
void FakeI2cBus_SetBmp390Present(FakeI2cBus *fake, uint16_t wire_addr,
                                 uint8_t chip_id, const uint8_t calib[21])
{
    if (fake == NULL) return;
    fake->bmp390_wire_addr = wire_addr;
    fake->bmp390_chip_id = chip_id;
    if (calib != NULL)
        memcpy(fake->bmp390_calib, calib, 21);
    else
        memset(fake->bmp390_calib, 0, 21);
}

void FakeI2cBus_SetBmp390Absent(FakeI2cBus *fake)
{
    if (fake == NULL) return;
    fake->bmp390_wire_addr = 0;
    fake->bmp390_chip_id = 0;
}

void FakeI2cBus_SetSgp41Response(FakeI2cBus *fake, const uint8_t *raw, size_t size)
{
    if (fake == NULL || raw == NULL) return;
    size_t n = (size < sizeof(fake->sgp41_read_response)) ? size : sizeof(fake->sgp41_read_response);
    memcpy(fake->sgp41_read_response, raw, n);
    fake->sgp41_read_response_size = n;
}

void FakeI2cBus_SetSgp41Absent(FakeI2cBus *fake)
{
    if (fake == NULL) return;
    /* Declare the SGP41 absent: its probe NACKs (address-aware) and the
       scripted read response is cleared. Other devices' probes are unaffected. */
    fake->sgp41_absent = true;
    fake->sgp41_read_response_size = 0U;
}

uint16_t FakeI2cBus_Sgp41Cmd(const uint8_t data[2])
{
    if (data == NULL) return 0U;
    return (uint16_t)(((uint16_t)data[0] << 8U) | (uint16_t)data[1]);
}