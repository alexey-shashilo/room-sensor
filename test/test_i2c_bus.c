#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "i2c_bus.h"

/* Portable I2cBus wrapper contract tests. Exercises only the static-inline
   wrappers in i2c_bus.h with a minimal local stub — no STM32 HAL, no driver
   dependency. Required cases:
     NULL bus, missing write/read_mem/read/probe, NULL data with nonzero size,
     zero size rejection, valid dispatch reaches the callback, and callback
     DriverStatus propagation unchanged. */

static int s_pass = 0, s_fail = 0, s_case = 0;

static void check(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

/* Local stub tracking which callback dispatched and what it returns. */
typedef struct
{
    int write_calls;
    int readmem_calls;
    int read_calls;
    int probe_calls;
    DriverStatus write_ret;
    DriverStatus readmem_ret;
    DriverStatus read_ret;
    DriverStatus probe_ret;
} Stub;

static DriverStatus stub_write(void *ctx, uint16_t addr, const uint8_t *data, size_t size)
{
    (void)addr; (void)data; (void)size;
    Stub *s = (Stub *)ctx;
    s->write_calls++;
    return s->write_ret;
}

static DriverStatus stub_readmem(void *ctx, uint16_t addr, uint8_t reg, uint8_t *data, size_t size)
{
    (void)addr; (void)reg; (void)data; (void)size;
    Stub *s = (Stub *)ctx;
    s->readmem_calls++;
    return s->readmem_ret;
}

static DriverStatus stub_read(void *ctx, uint16_t addr, uint8_t *data, size_t size)
{
    (void)addr; (void)data; (void)size;
    Stub *s = (Stub *)ctx;
    s->read_calls++;
    return s->read_ret;
}

static DriverStatus stub_probe(void *ctx, uint16_t addr)
{
    (void)addr;
    Stub *s = (Stub *)ctx;
    s->probe_calls++;
    return s->probe_ret;
}

static void fill_bus(I2cBus *bus, Stub *stub)
{
    memset(stub, 0, sizeof(*stub));
    stub->write_ret = DRIVER_STATUS_OK;
    stub->readmem_ret = DRIVER_STATUS_OK;
    stub->read_ret = DRIVER_STATUS_OK;
    stub->probe_ret = DRIVER_STATUS_OK;
    bus->context = stub;
    bus->write = stub_write;
    bus->read_mem = stub_readmem;
    bus->read = stub_read;
    bus->probe = stub_probe;
}

int main(void)
{
    printf("I2cBus wrapper contract tests\n");
    uint8_t data[4] = {0U, 0U, 0U, 0U};

    /* NULL bus */
    check(I2cBus_Write(NULL, 0x10, data, 1U) == DRIVER_STATUS_INVALID_ARG, "Write NULL bus -> INVALID_ARG");
    check(I2cBus_ReadMem(NULL, 0x10, 0, data, 1U) == DRIVER_STATUS_INVALID_ARG, "ReadMem NULL bus -> INVALID_ARG");
    check(I2cBus_Read(NULL, 0x10, data, 1U) == DRIVER_STATUS_INVALID_ARG, "Read NULL bus -> INVALID_ARG");
    check(I2cBus_Probe(NULL, 0x10) == DRIVER_STATUS_INVALID_ARG, "Probe NULL bus -> INVALID_ARG");

    /* Missing operation function pointer */
    {
        I2cBus bus; Stub stub;
        fill_bus(&bus, &stub);
        bus.write = NULL;
        check(I2cBus_Write(&bus, 0x10, data, 1U) == DRIVER_STATUS_NOT_SUPPORTED, "missing write -> NOT_SUPPORTED");
        bus.write = stub_write;
        bus.read_mem = NULL;
        check(I2cBus_ReadMem(&bus, 0x10, 0, data, 1U) == DRIVER_STATUS_NOT_SUPPORTED, "missing read_mem -> NOT_SUPPORTED");
        bus.read_mem = stub_readmem;
        bus.read = NULL;
        check(I2cBus_Read(&bus, 0x10, data, 1U) == DRIVER_STATUS_NOT_SUPPORTED, "missing read -> NOT_SUPPORTED");
        bus.read = stub_read;
        bus.probe = NULL;
        check(I2cBus_Probe(&bus, 0x10) == DRIVER_STATUS_NOT_SUPPORTED, "missing probe -> NOT_SUPPORTED");
    }

    /* NULL data with nonzero size */
    {
        I2cBus bus; Stub stub;
        fill_bus(&bus, &stub);
        check(I2cBus_Write(&bus, 0x10, NULL, 1U) == DRIVER_STATUS_INVALID_ARG, "Write NULL data,size>0 -> INVALID_ARG");
        check(I2cBus_ReadMem(&bus, 0x10, 0, NULL, 1U) == DRIVER_STATUS_INVALID_ARG, "ReadMem NULL data,size>0 -> INVALID_ARG");
        check(I2cBus_Read(&bus, 0x10, NULL, 1U) == DRIVER_STATUS_INVALID_ARG, "Read NULL data,size>0 -> INVALID_ARG");
    }

    /* Zero-size transfers rejected consistently */
    {
        I2cBus bus; Stub stub;
        fill_bus(&bus, &stub);
        check(I2cBus_Write(&bus, 0x10, data, 0U) == DRIVER_STATUS_INVALID_ARG, "Write size==0 -> INVALID_ARG");
        check(I2cBus_ReadMem(&bus, 0x10, 0, data, 0U) == DRIVER_STATUS_INVALID_ARG, "ReadMem size==0 -> INVALID_ARG");
        check(I2cBus_Read(&bus, 0x10, data, 0U) == DRIVER_STATUS_INVALID_ARG, "Read size==0 -> INVALID_ARG");
        check(stub.write_calls == 0 && stub.readmem_calls == 0 && stub.read_calls == 0,
              "rejected args do NOT reach callbacks");
    }

    /* Valid dispatch reaches callback + DriverStatus propagates unchanged */
    {
        I2cBus bus; Stub stub;
        fill_bus(&bus, &stub);
        stub.write_ret = DRIVER_STATUS_TIMEOUT;
        stub.readmem_ret = DRIVER_STATUS_CRC_ERROR;
        stub.read_ret = DRIVER_STATUS_BUS_ERROR;
        stub.probe_ret = DRIVER_STATUS_OK;

        check(I2cBus_Write(&bus, 0x10, data, 2U) == DRIVER_STATUS_TIMEOUT, "write dispatch + status propagated");
        check(stub.write_calls == 1, "write reached callback");
        check(I2cBus_ReadMem(&bus, 0x10, 3, data, 2U) == DRIVER_STATUS_CRC_ERROR, "read_mem dispatch + status propagated");
        check(stub.readmem_calls == 1, "read_mem reached callback");
        check(I2cBus_Read(&bus, 0x10, data, 2U) == DRIVER_STATUS_BUS_ERROR, "read dispatch + status propagated");
        check(stub.read_calls == 1, "read reached callback");
        check(I2cBus_Probe(&bus, 0x10) == DRIVER_STATUS_OK, "probe dispatch + status propagated");
        check(stub.probe_calls == 1, "probe reached callback");
    }

    /* UINT16_MAX size boundary (P2-4): UINT16_MAX must dispatch to the callback;
       UINT16_MAX+1 must be rejected WITHOUT dispatching (no HAL narrowing). */
    {
        I2cBus bus; Stub stub;
        fill_bus(&bus, &stub);
        /* UINT16_MAX accepted by the portable contract (reaches callback). */
        size_t max16 = (size_t)UINT16_MAX;
        check(I2cBus_Write(&bus, 0x10, data, max16) == DRIVER_STATUS_OK, "Write size=UINT16_MAX reaches callback");
        check(stub.write_calls == 1, "UINT16_MAX write dispatched");
        /* UINT16_MAX+1 rejected before any callback. */
        size_t over = (size_t)UINT16_MAX + 1U;
        check(I2cBus_Write(&bus, 0x10, data, over) == DRIVER_STATUS_INVALID_ARG, "Write size=UINT16_MAX+1 -> INVALID_ARG");
        check(I2cBus_ReadMem(&bus, 0x10, 0, data, over) == DRIVER_STATUS_INVALID_ARG, "ReadMem size=UINT16_MAX+1 -> INVALID_ARG");
        check(I2cBus_Read(&bus, 0x10, data, over) == DRIVER_STATUS_INVALID_ARG, "Read size=UINT16_MAX+1 -> INVALID_ARG");
        check(stub.write_calls == 1 && stub.readmem_calls == 0 && stub.read_calls == 0,
              "oversize rejected without dispatching to any callback");
    }

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}