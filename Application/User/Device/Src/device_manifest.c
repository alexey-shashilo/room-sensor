#include "device_manifest.h"
#include "platform_hardware.h"
#include "telemetry.h"
#include "command.h"
#include "config.h"
#include "storage.h"
#include "device_identity.h"
#include <string.h>

/* SECURE-CRITICAL (P1-3): the manifest output must be FULLY initialized on
   every path. The identity is taken from the authoritative runtime identity
   supplied by the command services layer (already validated/owned by App),
   NOT by re-reading persistent storage inside the builder. Re-reading
   identity here would (a) duplicate the authoritative source, (b) risk picking
   up a corrupt/absent record and (c) could leave manifest.identity
   uninitialized when DeviceIdentity_Load fails — a stack-disclosure path for
   the read-only GET_MANIFEST command. If no authoritative identity is supplied
   the identity is deterministically zero-filled (never uninitialized bytes). */
void DeviceManifest_Build(DeviceManifest *manifest, const DeviceIdentity *authoritative_identity)
{
    if (manifest == NULL) return;

    /* Deterministic initialization of EVERY field so no garbage can serialize. */
    memset(manifest, 0, sizeof(*manifest));

    if (authoritative_identity != NULL)
        manifest->identity = *authoritative_identity;

    FirmwareMetadata_Get(&manifest->firmware);
    DeviceCapabilities_Get(&manifest->capabilities);
    BootSession_Get(&manifest->session);

    manifest->protocols.telemetry_schema = TELEMETRY_SCHEMA_VERSION;
    manifest->protocols.command_schema = COMMAND_SCHEMA_VERSION;
    manifest->protocols.config_schema = CONFIG_SCHEMA_VERSION;
    manifest->protocols.identity_schema = IDENTITY_SCHEMA_VERSION;
    manifest->protocols.storage_record_format = STORAGE_RECORD_FORMAT_VERSION;
}