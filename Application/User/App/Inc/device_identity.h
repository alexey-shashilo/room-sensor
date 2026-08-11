#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "storage.h"

#define DEVICE_UUID_SIZE            16U
#define IDENTITY_SCHEMA_VERSION     2U
#define ROOM_SENSOR_HW_REVISION     1U

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

/* Non-mutating diagnostic inspection of the persisted identity record.
   Reads and validates into a LOCAL candidate and returns the observed state
   without touching the global runtime identity or persistence status.
   OK = persisted healthy, NOT_FOUND = blank, CORRUPT = corrupt record,
   IO_ERROR = Flash failure. */
StorageReadStatus DeviceIdentity_SelfCheck(void);

#endif