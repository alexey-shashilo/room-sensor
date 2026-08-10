#include "device_manifest.h"
#include "platform_hardware.h"
#include "telemetry.h"
#include "command.h"
#include "config.h"
#include "storage.h"
#include "device_identity.h"

void DeviceManifest_Build(DeviceManifest *manifest)
{
    if (manifest == NULL) return;

    DeviceIdentity_Load(&manifest->identity);
    FirmwareMetadata_Get(&manifest->firmware);
    DeviceCapabilities_Get(&manifest->capabilities);
    BootSession_Get(&manifest->session);

    manifest->protocols.telemetry_schema = TELEMETRY_SCHEMA_VERSION;
    manifest->protocols.command_schema = COMMAND_SCHEMA_VERSION;
    manifest->protocols.config_schema = CONFIG_SCHEMA_VERSION;
    manifest->protocols.identity_schema = IDENTITY_SCHEMA_VERSION;
    manifest->protocols.storage_record_format = STORAGE_RECORD_FORMAT_VERSION;
}