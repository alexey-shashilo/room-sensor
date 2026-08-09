#ifndef I2C_BUS_H
#define I2C_BUS_H

#include <stdint.h>
#include <stddef.h>
#include "room_sensor_types.h"

typedef struct I2cBus I2cBus;

typedef DriverStatus (*I2cBus_WriteFn)(void *context, uint16_t addr, const uint8_t *data, size_t size);
typedef DriverStatus (*I2cBus_ReadMemFn)(void *context, uint16_t addr, uint8_t reg, uint8_t *data, size_t size);
typedef DriverStatus (*I2cBus_ProbeFn)(void *context, uint16_t addr);

struct I2cBus
{
    void *context;
    I2cBus_WriteFn   write;
    I2cBus_ReadMemFn read_mem;
    I2cBus_ProbeFn   probe;
};

static inline DriverStatus I2cBus_Write(const I2cBus *bus, uint16_t addr, const uint8_t *data, size_t size)
{
    if (bus == NULL) return DRIVER_ERROR_ARGUMENT;
    return bus->write(bus->context, addr, data, size);
}

static inline DriverStatus I2cBus_ReadMem(const I2cBus *bus, uint16_t addr, uint8_t reg, uint8_t *data, size_t size)
{
    if (bus == NULL) return DRIVER_ERROR_ARGUMENT;
    return bus->read_mem(bus->context, addr, reg, data, size);
}

static inline DriverStatus I2cBus_Probe(const I2cBus *bus, uint16_t addr)
{
    if (bus == NULL) return DRIVER_ERROR_ARGUMENT;
    return bus->probe(bus->context, addr);
}

#endif