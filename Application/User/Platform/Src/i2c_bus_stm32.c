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
    /* P2-4: HAL accepts uint16_t; refuse a size that would truncate. */
    if (size > UINT16_MAX) return DRIVER_STATUS_INVALID_ARG;
    return I2cBus_Stm32_MapHAL(HAL_I2C_Master_Transmit(ctx->hi2c, addr, (uint8_t *)data, (uint16_t)size, ctx->timeout_ms));
}

static DriverStatus I2cBus_Stm32_ReadMem(void *context, uint16_t addr, uint8_t reg, uint8_t *data, size_t size)
{
    I2cBus_Stm32Context *ctx = (I2cBus_Stm32Context *)context;
    if (size > UINT16_MAX) return DRIVER_STATUS_INVALID_ARG;
    return I2cBus_Stm32_MapHAL(HAL_I2C_Mem_Read(ctx->hi2c, addr, reg, I2C_MEMADD_SIZE_8BIT, data, (uint16_t)size, ctx->timeout_ms));
}

static DriverStatus I2cBus_Stm32_Read(void *context, uint16_t addr, uint8_t *data, size_t size)
{
    I2cBus_Stm32Context *ctx = (I2cBus_Stm32Context *)context;
    if (size > UINT16_MAX) return DRIVER_STATUS_INVALID_ARG;
    return I2cBus_Stm32_MapHAL(HAL_I2C_Master_Receive(ctx->hi2c, addr, data, (uint16_t)size, ctx->timeout_ms));
}

static DriverStatus I2cBus_Stm32_Probe(void *context, uint16_t addr)
{
    I2cBus_Stm32Context *ctx = (I2cBus_Stm32Context *)context;
    return I2cBus_Stm32_MapHAL(HAL_I2C_IsDeviceReady(ctx->hi2c, addr, 3U, ctx->timeout_ms));
}

/* Shared-bus recovery. Reinitializes the I2C peripheral in place by:

     1. deinitializing the peripheral (HAL_I2C_DeInit, which also clears any
        latched BUSY / lockup state);
     3. reinitializing it (HAL_I2C_Init), which re-applies the previously
        configured Timing/addressing mode;
     4. restoring the analog/digital filter configuration that CubeMX set at
        boot (I2CEx_ConfigAnalogFilter / I2CEx_ConfigDigitalFilter);

   No manual SCL clock-pulsing is performed. Pulse-toggling SCL is only needed
   to release a criminal output-low slave after a bus fault; there is NO
   demonstrated stuck-slave scenario here, so speculative GPIO clock-pulsing
   would add risk (reconfiguring the shared SCL pin to GPIO while another device
   may be mid-transaction) without evidence. If a future observation shows a
   genuine stuck-slave (SCL held low after timeout) a bounded SCL-recovery step
   can be added then, together with its own test.

   The re-init uses the existing HARDWARE register state as HAL already bound
   (Instance, Init, State). HAL_I2C_Init preserves the handle's Init config, so
   re-initialization reproduces the boot-time peripheral setup. */
static DriverStatus I2cBus_Stm32_Recover(void *context)
{
    I2cBus_Stm32Context *ctx = (I2cBus_Stm32Context *)context;
    if (ctx == NULL || ctx->hi2c == NULL)
        return DRIVER_STATUS_INVALID_ARG;

    /* A full deinit+reinit of the peripheral clears any latched BUSY / error
       state and reproduces the boot-time configuration. HAL_I2C_DeInit disables
       the peripheral (PE=0), which resets the BUSY status bit; there is no
       need to poke read-only ISR flags with __HAL_I2C_CLEAR_FLAG (the G4 I2C
       BUSY bit is not in the software-clearable set). */

    /* Fail-closed: every mandatory step must succeed. HAL_I2C_DeInit /
       HAL_I2C_Init / both filter configurations are straight-line; ANY failure
       returns DRIVER_STATUS_BUS_ERROR and I2cBus_Recover is reported as failed.
       No partial success is ever pretended. */

    if (HAL_I2C_DeInit(ctx->hi2c) != HAL_OK)
        return DRIVER_STATUS_BUS_ERROR;
    if (HAL_I2C_Init(ctx->hi2c) != HAL_OK)
        return DRIVER_STATUS_BUS_ERROR;

    /* Re-apply the same analog + digital filter configuration MX_I2C1_Init set
       at boot so the post-recovery peripheral matches the original profile. */
    if (HAL_I2CEx_ConfigAnalogFilter(ctx->hi2c, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
        return DRIVER_STATUS_BUS_ERROR;
    if (HAL_I2CEx_ConfigDigitalFilter(ctx->hi2c, 0) != HAL_OK)
        return DRIVER_STATUS_BUS_ERROR;

    /* The handle State/ErrorCode are cleared ONLY after every mandatory HAL call
       above returned HAL_OK. Required because HAL_I2C_Init succeeds with the
       handle left in HAL_I2C_STATE_READY only when there is no error, but a
       previously-armed error/lockup may leave the cached ErrorCode/State set;
       forcing READY/ERROR_NONE here makes the POST-recovery handle usable by the
       subsequent I2cBus operations without relying on a power-cycle. This is a
       post-success cleanup, NOT a way of pretending a failed init succeeded —
       a failure at any mandatory step already returned earlier. */
    ctx->hi2c->State = HAL_I2C_STATE_READY;
    ctx->hi2c->ErrorCode = HAL_I2C_ERROR_NONE;

    return DRIVER_STATUS_OK;
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
    bus->recover = I2cBus_Stm32_Recover;
}