#ifndef I2C_BUS_H
#define I2C_BUS_H

#include <stdint.h>
#include <stddef.h>
#include "room_sensor_types.h"

/* Portable I2C bus abstraction. Each operation is a thin wrapper that validates
   its arguments and dispatches to the implementation's function pointer.

   Defensive contract (consistent across all four operations):
     - bus == NULL                 -> DRIVER_STATUS_INVALID_ARG
     - missing operation fn pointer -> DRIVER_STATUS_NOT_SUPPORTED
   For data-transfer operations (Write/ReadMem/Read):
     - size == 0                   -> DRIVER_STATUS_INVALID_ARG. A zero-length
       I2C transfer has no meaningful hardware semantics and is not used by any
       driver, so it is rejected uniformly rather than left to HAL behavior.
     - size > 0 && data == NULL    -> DRIVER_STATUS_INVALID_ARG

   The I2C address argument is the on-wire 8-bit (left-shifted) address and is
   passed through unchanged; this contract does not alter the address
   convention. No STM32 HAL types appear here; the abstraction stays portable. */

typedef struct I2cBus I2cBus;

typedef DriverStatus (*I2cBus_WriteFn)(void *context, uint16_t addr, const uint8_t *data, size_t size);
typedef DriverStatus (*I2cBus_ReadMemFn)(void *context, uint16_t addr, uint8_t reg, uint8_t *data, size_t size);
typedef DriverStatus (*I2cBus_ReadFn)(void *context, uint16_t addr, uint8_t *data, size_t size);
typedef DriverStatus (*I2cBus_ProbeFn)(void *context, uint16_t addr);

/* Optional platform-neutral recovery hook. Re-derives a usable I2C peripheral
   from a transport-level failure (bus stuck/busy/error state) WITHOUT involving
   any HAL type. A driver or the shared-bus health policy may call it only when
   evidence indicates a real shared-bus failure; it must never be used for a
   single device-local problem. If the implementation does not provide recovery,
   the pointer is NULL and I2cBus_Recover returns DRIVER_STATUS_NOT_SUPPORTED. */
typedef DriverStatus (*I2cBus_RecoverFn)(void *context);

struct I2cBus
{
    void *context;
    I2cBus_WriteFn   write;
    I2cBus_ReadMemFn read_mem;
    I2cBus_ReadFn    read;
    I2cBus_ProbeFn   probe;
    I2cBus_RecoverFn  recover;
};

static inline DriverStatus I2cBus_Write(const I2cBus *bus, uint16_t addr, const uint8_t *data, size_t size)
{
    if (bus == NULL) return DRIVER_STATUS_INVALID_ARG;
    if (bus->write == NULL) return DRIVER_STATUS_NOT_SUPPORTED;
    if (size == 0U || (data == NULL)) return DRIVER_STATUS_INVALID_ARG;
    return bus->write(bus->context, addr, data, size);
}

static inline DriverStatus I2cBus_ReadMem(const I2cBus *bus, uint16_t addr, uint8_t reg, uint8_t *data, size_t size)
{
    if (bus == NULL) return DRIVER_STATUS_INVALID_ARG;
    if (bus->read_mem == NULL) return DRIVER_STATUS_NOT_SUPPORTED;
    if (size == 0U || (data == NULL)) return DRIVER_STATUS_INVALID_ARG;
    return bus->read_mem(bus->context, addr, reg, data, size);
}

/* Plain read with no preceding register/data byte. Used by command-response
   sensors (e.g. Sensirion SCD4x) where the master sends a command then reads
   the response via a repeated-START read. Not all bus implementations use it
   (register-mapped devices call I2cBus_ReadMem instead). */
static inline DriverStatus I2cBus_Read(const I2cBus *bus, uint16_t addr, uint8_t *data, size_t size)
{
    if (bus == NULL) return DRIVER_STATUS_INVALID_ARG;
    if (bus->read == NULL) return DRIVER_STATUS_NOT_SUPPORTED;
    if (size == 0U || (data == NULL)) return DRIVER_STATUS_INVALID_ARG;
    return bus->read(bus->context, addr, data, size);
}

static inline DriverStatus I2cBus_Probe(const I2cBus *bus, uint16_t addr)
{
    if (bus == NULL) return DRIVER_STATUS_INVALID_ARG;
    if (bus->probe == NULL) return DRIVER_STATUS_NOT_SUPPORTED;
    return bus->probe(bus->context, addr);
}

/* Invoke the optional shared-bus recovery hook. Returns NOT_SUPPORTED when the
   bus does not provide recovery (a missing hook is a normal, healthy state). */
static inline DriverStatus I2cBus_Recover(const I2cBus *bus)
{
    if (bus == NULL) return DRIVER_STATUS_INVALID_ARG;
    if (bus->recover == NULL) return DRIVER_STATUS_NOT_SUPPORTED;
    return bus->recover(bus->context);
}

#endif