#ifndef FAKE_I2C_BUS_H
#define FAKE_I2C_BUS_H

#include <stdint.h>
#include <stddef.h>
#include "i2c_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    DriverStatus write_result;
    DriverStatus read_mem_result;
    DriverStatus probe_result;
    int          write_call_count;
    int          read_mem_call_count;
    int          probe_call_count;

    uint8_t      regs[256];
    uint16_t     last_write_reg;
    uint16_t     last_write_value;
} FakeI2cBus;

void FakeI2cBus_Init(FakeI2cBus *fake);
void FakeI2cBus_GetBus(I2cBus *bus, FakeI2cBus *fake);
void FakeI2cBus_SetAlsRead(FakeI2cBus *fake, uint16_t raw);

#ifdef __cplusplus
}
#endif

#endif