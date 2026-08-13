/* Host Platform — I2C implementation */

#include "host_platform.h"
#include "i2c_bus.h"
#include <string.h>

static HostI2cDevice s_devices[8];
static uint16_t s_device_addrs[8];
static int s_device_count = 0;
static int s_call_count = 0;
static int s_write_count = 0;
static uint8_t s_als_regs[2] = {0, 0};

void HostI2c_RegisterDevice(uint16_t addr)
{
    if (s_device_count >= 8) return;
    s_device_addrs[s_device_count] = addr;
    memset(&s_devices[s_device_count], 0, sizeof(HostI2cDevice));
    s_devices[s_device_count].present = true;
    s_device_count++;
}

void HostI2c_RemoveDevice(uint16_t addr)
{
    for (int i = 0; i < s_device_count; i++)
        if (s_device_addrs[i] == addr) { s_devices[i].present = false; break; }
}

void HostI2c_SetAlsRead(uint16_t raw)
{
    s_als_regs[0] = (uint8_t)(raw & 0xFF);
    s_als_regs[1] = (uint8_t)(raw >> 8);
}

int HostI2c_GetCallCount(void) { return s_call_count; }
int HostI2c_GetWriteCount(void) { return s_write_count; }
void HostI2c_ResetCounters(void) { s_call_count = 0; s_write_count = 0; }

static HostI2cDevice *FindDevice(uint16_t addr)
{
    for (int i = 0; i < s_device_count; i++)
        if (s_device_addrs[i] == addr && s_devices[i].present)
            return &s_devices[i];
    return NULL;
}

static DriverStatus host_write(void *ctx, uint16_t addr, const uint8_t *data, size_t size)
{
    (void)ctx; s_call_count++;
    HostI2cDevice *dev = FindDevice(addr);
    if (dev == NULL || data == NULL || size == 0) return DRIVER_STATUS_BUS_ERROR;
    s_write_count++;
    if (size >= 3) { dev->regs[data[0]] = data[1]; dev->regs[data[0] + 1] = data[2]; }
    return DRIVER_STATUS_OK;
}

static DriverStatus host_read_mem(void *ctx, uint16_t addr, uint8_t reg, uint8_t *data, size_t size)
{
    (void)ctx; s_call_count++;
    HostI2cDevice *dev = FindDevice(addr);
    if (dev == NULL || data == NULL || size == 0) return DRIVER_STATUS_BUS_ERROR;
    if (reg == 0x04 && (s_als_regs[0] != 0 || s_als_regs[1] != 0))
    {
        data[0] = s_als_regs[0];
        if (size > 1) data[1] = s_als_regs[1];
        return DRIVER_STATUS_OK;
    }
    for (size_t i = 0; i < size && (reg + i) < 256; i++)
        data[i] = dev->regs[reg + i];
    return DRIVER_STATUS_OK;
}

static DriverStatus host_read(void *ctx, uint16_t addr, uint8_t *data, size_t size)
{
    (void)ctx; s_call_count++;
    HostI2cDevice *dev = FindDevice(addr);
    if (dev == NULL || data == NULL || size == 0) return DRIVER_STATUS_BUS_ERROR;
    for (size_t i = 0; i < size; i++)
        data[i] = dev->regs[i];
    return DRIVER_STATUS_OK;
}

static DriverStatus host_probe(void *ctx, uint16_t addr)
{
    (void)ctx; s_call_count++;
    return (FindDevice(addr) != NULL) ? DRIVER_STATUS_OK : DRIVER_STATUS_BUS_ERROR;
}

void HostPlatform_GetI2cBus(I2cBus *bus)
{
    if (bus == NULL) return;
    bus->context = NULL;
    bus->write = host_write;
    bus->read_mem = host_read_mem;
    bus->read = host_read;
    bus->probe = host_probe;
}