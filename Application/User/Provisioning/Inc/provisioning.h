#ifndef PROVISIONING_H
#define PROVISIONING_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "storage.h"

#define REGISTRATION_SCHEMA_VERSION 1U

#define ENTITY_ID_SIZE 16U

typedef struct
{
    uint8_t bytes[ENTITY_ID_SIZE];
} EntityId;

typedef enum
{
    PROVISIONING_UNPROVISIONED = 0,
    PROVISIONING_DISCOVERABLE,
    PROVISIONING_REGISTRATION_PENDING,
    PROVISIONING_REGISTERED,
    PROVISIONING_CONFIGURATION_PENDING,
    PROVISIONING_OPERATIONAL,
    PROVISIONING_ERROR
} ProvisioningState;

typedef struct
{
    bool registered;
    bool installation_valid;
    bool building_valid;
    bool room_valid;

    EntityId installation_id;
    EntityId building_id;
    EntityId room_id;
} DeviceRegistration;

typedef struct
{
    ProvisioningState state;

    bool registered;
    bool installation_valid;
    bool building_valid;
    bool room_valid;

    uint32_t revision;
} ProvisioningStatus;

/* Runtime provisioning owner — loaded once at boot and kept in RAM so that
   GET_PROVISIONING_STATUS performs zero Flash reads and mutation handlers
   operate on a single authoritative copy. storage_status records how the
   runtime was loaded (OK / NOT_FOUND / CORRUPT / IO_ERROR). */
typedef struct
{
    DeviceRegistration current;
    ProvisioningStatus status;
    StorageReadStatus  storage_status;
} ProvisioningRuntime;

bool Provisioning_Init(void);

const ProvisioningRuntime *Provisioning_GetRuntime(void);

bool Provisioning_Load(DeviceRegistration *reg);
bool Provisioning_Save(const DeviceRegistration *reg);
bool Provisioning_Clear(void);

bool Provisioning_ValidateRegistration(const DeviceRegistration *reg);
bool Provisioning_IsRegistered(const DeviceRegistration *reg);
bool Provisioning_IsOperational(const DeviceRegistration *reg);

/* Current provisioning storage health, reflecting the latest Init/Save/Clear
   result (runtime-current, NOT a stale boot snapshot). True when registration
   storage is in a known-good state (OK or NOT_FOUND); false when CORRUPT or
   IO_ERROR. After an explicit recovery the value changes without reboot. */
bool Provisioning_IsHealthy(void);

void Provisioning_GetStatus(const DeviceRegistration *reg, ProvisioningStatus *status);

bool EntityId_IsZero(const EntityId *id);
bool EntityId_Parse(EntityId *out, const char *hex, size_t len);
void EntityId_Format(const EntityId *id, char *out, size_t max_len);
bool Provisioning_ValidEntityId(const EntityId *id);

#endif