#ifndef PLATFORM_HARDWARE_H
#define PLATFORM_HARDWARE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    const char *platform_family;
    const char *board_name;
} PlatformHardwareInfo;

bool Platform_GetHardwareInfo(PlatformHardwareInfo *info);
bool Platform_CreateBootId(uint64_t *boot_id);

#endif