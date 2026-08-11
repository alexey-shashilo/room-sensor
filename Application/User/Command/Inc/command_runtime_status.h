#ifndef COMMAND_RUNTIME_STATUS_H
#define COMMAND_RUNTIME_STATUS_H

#include <stdbool.h>
#include <stdint.h>
#include "storage.h"
#include "provisioning.h"
#include "room_sensor_types.h"

/* Portable command-facing runtime-status snapshot.

   The Command layer MUST NOT depend on App: CommandRuntimeStatus is a plain
   data-transfer struct that App fills from the authoritative owning modules
   (Storage, Config, DeviceIdentity, Provisioning, App health) immediately
   before Command_Run(). Command reads this snapshot only; it does not own or
   mutate the underlying state.

   Read status (StorageReadStatus) and redundancy health (StorageHealth) are
   deliberately separate. A VALID+IO region reads OK (STORAGE_READ_OK) yet
   reports DEGRADED_IO health — the record is usable but the A/B mirror is
   unhealthy, which system diagnostics must surface. */
typedef struct
{
    SystemHealthState system_health;

    bool storage_initialized;

    StorageReadStatus config_persistence;
    StorageHealth     config_storage_health;

    StorageReadStatus identity_persistence;
    StorageHealth     identity_storage_health;

    ProvisioningState provisioning_state;
    StorageReadStatus provisioning_persistence;
    StorageHealth     provisioning_storage_health;
} CommandRuntimeStatus;

#endif /* COMMAND_RUNTIME_STATUS_H */