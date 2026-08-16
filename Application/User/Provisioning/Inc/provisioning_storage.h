#ifndef PROVISIONING_STORAGE_H
#define PROVISIONING_STORAGE_H

#include "provisioning.h"
#include <stdint.h>
#include <stddef.h>

typedef struct
{
    uint32_t schema_version;      /* offset 0, 4 */
    uint32_t revision;            /* offset 4, 4 */
    bool     registered;          /* offset 8, 1 */
    uint8_t  reserved[3];         /* offset 9, 3 */
    EntityId installation_id;     /* offset 12, 16 */
    EntityId building_id;         /* offset 28, 16 */
    EntityId room_id;             /* offset 44, 16 */
} __attribute__((packed)) RegistrationStorageV1;

/* CORRECT packed size: 4+4+1+3+3*16 = 60 bytes.
   (Historical header comment claimed 57; that was wrong. The ABI is frozen at
   60 bytes on-flash.) */
#define REGISTRATION_STORAGE_V1_SIZE 60U

/* ABI freeze (P2-1): the persisted registration record format is FROZEN at
   these exact sizes/offsets. Changing it is a storage-format migration and is
   NOT performed in this remediation. `bool registered` is explicitly retained
   as packed bool (1 byte); converting to uint8_t would not change the on-flash
   size but is deferred because any signalling change is out of scope here. */
_Static_assert(sizeof(RegistrationStorageV1) == REGISTRATION_STORAGE_V1_SIZE,
               "RegistrationStorageV1 ABI size must remain 60 bytes");
_Static_assert(offsetof(RegistrationStorageV1, schema_version) == 0,
               "schema_version offset frozen at 0");
_Static_assert(offsetof(RegistrationStorageV1, revision) == 4,
               "revision offset frozen at 4");
_Static_assert(offsetof(RegistrationStorageV1, installation_id) == 12,
               "installation_id offset frozen at 12");
_Static_assert(offsetof(RegistrationStorageV1, building_id) == 28,
               "building_id offset frozen at 28");
_Static_assert(offsetof(RegistrationStorageV1, room_id) == 44,
               "room_id offset frozen at 44");
_Static_assert(sizeof(EntityId) == ENTITY_ID_SIZE,
               "EntityId must be exactly 16 bytes");

#endif