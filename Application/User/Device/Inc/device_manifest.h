#ifndef DEVICE_MANIFEST_H
#define DEVICE_MANIFEST_H

#include <stdint.h>
#include "device_identity.h"
#include "firmware_metadata.h"
#include "device_capabilities.h"
#include "boot_session.h"

#define DEVICE_MANIFEST_SCHEMA_VERSION 1U

typedef struct
{
    uint32_t telemetry_schema;
    uint32_t command_schema;
    uint32_t config_schema;
    uint32_t identity_schema;
    uint32_t storage_record_format;
} ProtocolCapabilities;

typedef struct
{
    DeviceIdentity identity;
    FirmwareMetadata firmware;
    DeviceCapabilities capabilities;
    ProtocolCapabilities protocols;
    BootSession session;
} DeviceManifest;

void DeviceManifest_Build(DeviceManifest *manifest, const DeviceIdentity *authoritative_identity);

#endif