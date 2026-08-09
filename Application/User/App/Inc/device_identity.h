#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint8_t  device_uuid[16];
    uint32_t hardware_revision;
    uint32_t firmware_config_version;
} DeviceIdentity;

bool DeviceIdentity_Load(DeviceIdentity *id);
bool DeviceIdentity_Generate(DeviceIdentity *id);
bool DeviceIdentity_Save(const DeviceIdentity *id);
void DeviceIdentity_GetShortId(const DeviceIdentity *id, char *out, size_t max_len);

#endif