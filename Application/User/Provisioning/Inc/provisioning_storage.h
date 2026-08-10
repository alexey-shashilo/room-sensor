#ifndef PROVISIONING_STORAGE_H
#define PROVISIONING_STORAGE_H

#include "provisioning.h"

typedef struct
{
    uint32_t schema_version;
    uint32_t revision;
    bool     registered;
    uint8_t  reserved[3];
    EntityId installation_id;
    EntityId building_id;
    EntityId room_id;
} __attribute__((packed)) RegistrationStorageV1;

/* sizeof = 4 + 1 + 3*16 + 4 = 57 (packed: bool=1, padding=3) */

#endif