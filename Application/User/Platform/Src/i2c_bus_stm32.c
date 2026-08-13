#include "i2c_bus_stm32.h"

typedef struct
{
    I2C_HandleTypeDef *hi2c;
    uint32_t timeout_ms;
} I2cBus_Stm32Context;

static DriverStatus I2cBus_Stm32_MapHAL(HAL_StatusTypeDef hal)
{
    switch (hal)
    {
        case HAL_OK:       return DRIVER_STATUS_OK;
        case HAL_TIMEOUT:  return DRIVER_STATUS_TIMEOUT;
        case HAL_BUSY:     return DRIVER_STATUS_BUS_ERROR;
        default:           return DRIVER_STATUS_BUS_ERROR;
    }
}

static DriverStatus I2cBus_Stm32_Write(void *context, uint16_t addr, const uint8_t *data, size_t size)
{
    I2cBus_Stm32Context *ctx = (I2cBus_Stm32Context *)context;
    return I2cBus_Stm32_MapHAL(HAL_I2C_Master_Transmit(ctx->hi2c, addr, (uint8_t *)data, (uint16_t)size, ctx->timeout_ms));
}

static DriverStatus I2cBus_Stm32_ReadMem(void *context, uint16_t addr, uint8_t reg, uint8_t *data, size_t size)
{
    I2cBus_Stm32Context *ctx = (I2cBus_Stm32Context *)context;
    return I2cBus_Stm32_MapHAL(HAL_I2C_Mem_Read(ctx->hi2c, addr, reg, I2C_MEMADD_SIZE_8BIT, data, (uint16_t)size, ctx->timeout_ms));
}

static DriverStatus I2cBus_Stm32_Read(void *context, uint16_t addr, uint8_t *data, size_t size)
{
    I2cBus_Stm32Context *ctx = (I2cBus_Stm32Context *)context;
    return I2cBus_Stm32_MapHAL(HAL_I2C_Master_Receive(ctx->hi2c, addr, data, (uint16_t)size, ctx->timeout_ms));
}

static DriverStatus I2cBus_Stm32_Probe(void *context, uint16_t addr)
{
    I2cBus_Stm32Context *ctx = (I2cBus_Stm32Context *)context;
    return I2cBus_Stm32_MapHAL(HAL_I2C_IsDeviceReady(ctx->hi2c, addr, 3U, ctx->timeout_ms));
}

void I2cBus_Stm32_Init(I2cBus *bus, I2C_HandleTypeDef *hi2c, uint32_t timeout_ms)
{
    static I2cBus_Stm32Context ctx;
    ctx.hi2c = hi2c;
    ctx.timeout_ms = timeout_ms;

    bus->context = &ctx;
    bus->write = I2cBus_Stm32_Write;
    bus->read_mem = I2cBus_Stm32_ReadMem;
    bus->read = I2cBus_Stm32_Read;
    bus->probe = I2cBus_Stm32_Probe;
}