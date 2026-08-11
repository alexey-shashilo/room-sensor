#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "storage.h"

#define DEVICE_UUID_SIZE            16U
#define IDENTITY_SCHEMA_VERSION     2U
#define ROOM_SENSOR_HW_REVISION     1U

/* SECURITY NOTE: the DeviceIdentity UUID is a STABLE DEVICE IDENTIFIER derived
   from the hardware UID via deterministic custom mixing. It is NOT
   cryptographic authentication, NOT a secret, and NOT proof of device
   authenticity. It may be used to identify a device (boot ID, telemetry tag),
   never to authenticate or authorize it. Do not rely on it as a credential. */

typedef struct
{
    uint8_t  device_uuid[DEVICE_UUID_SIZE];
    uint32_t hardware_revision;
} DeviceIdentity;

bool DeviceIdentity_Load(DeviceIdentity *id);
bool DeviceIdentity_Derive(DeviceIdentity *id);
bool DeviceIdentity_Save(const DeviceIdentity *id);
bool DeviceIdentity_Validate(const DeviceIdentity *id);
void DeviceIdentity_GetShortId(const DeviceIdentity *id, char *out, size_t max_len);

/* Historical load status observed during the last DeviceIdentity_Load attempt.
   NOT_FOUND = blank, CORRUPT = invalid/corrupt record (derive used,
   degradation flagged), IO_ERROR = Flash failure. */
StorageReadStatus DeviceIdentity_GetLoadStatus(void);

/* Current persistence status of the identity record. Updated by the last
   DeviceIdentity_Load() and every DeviceIdentity_Save(), so a successful
   durable save reports OK. NOT_FOUND = blank/first-boot. */
StorageReadStatus DeviceIdentity_GetPersistenceStatus(void);

/* Redundancy (A/B mirror) health of the persisted identity record, separate
   from persistence status. A readable VALID+IO record reads OK yet reports
   DEGRADED_IO. Updated by DeviceIdentity_Load / DeviceIdentity_Save. */
StorageHealth DeviceIdentity_GetStorageHealth(void);

/* Exact result of the LAST identity write attempt (OK / INVALID_ARGUMENT /
   UNSAFE_STATE / IO_ERROR / VERIFY_FAILED) — a separate fact from current
   readability and A/B health. A failed write does NOT mean the existing VALID
   identity record was lost or corrupt. */
StorageWriteStatus DeviceIdentity_GetLastWriteStatus(void);

/* Non-mutating diagnostic inspection of the persisted identity record.
   Reads and validates into a LOCAL candidate and returns the observed state
   without touching the global runtime identity or persistence status.
   OK = persisted healthy, NOT_FOUND = blank, CORRUPT = corrupt record,
   IO_ERROR = Flash failure. */
StorageReadStatus DeviceIdentity_SelfCheck(void);

#endif