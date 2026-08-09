#include "fake_i2c_bus.h"
#include <string.h>

static DriverStatus fake_write(void *ctx, uint16_t addr, const uint8_t *data, size_t size)
{
    (void)addr;
    FakeI2cBus *f = (FakeI2cBus *)ctx;
    f->write_call_count++;
    if (size >= 1)
    {
        uint8_t reg = data[0];
        f->last_write_reg = reg;
        if (size >= 3)
        {
            uint16_t val = (uint16_t)data[2] << 8U | (uint16_t)data[1];
            f->last_write_value = val;
            f->regs[reg] = data[1];
            f->regs[reg + 1] = data[2];
        }
    }
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

static DriverStatus fake_probe(void *ctx, uint16_t addr)
{
    (void)addr;
    FakeI2cBus *f = (FakeI2cBus *)ctx;
    f->probe_call_count++;
    return f->probe_result;
}

void FakeI2cBus_Init(FakeI2cBus *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->write_result = DRIVER_STATUS_OK;
    fake->read_mem_result = DRIVER_STATUS_OK;
    fake->probe_result = DRIVER_STATUS_OK;
}

void FakeI2cBus_GetBus(I2cBus *bus, FakeI2cBus *fake)
{
    bus->context = fake;
    bus->write = fake_write;
    bus->read_mem = fake_read_mem;
    bus->probe = fake_probe;
}

void FakeI2cBus_SetAlsRead(FakeI2cBus *fake, uint16_t raw)
{
    fake->regs[0x04] = (uint8_t)(raw & 0xFFU);
    fake->regs[0x05] = (uint8_t)(raw >> 8U);
}