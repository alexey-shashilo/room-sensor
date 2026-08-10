#ifndef BOOT_SESSION_H
#define BOOT_SESSION_H

#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    uint64_t boot_id;
} BootSession;

bool BootSession_Get(BootSession *session);

#endif