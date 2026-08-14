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

/* boot_id is derived as: (first 8 unique-ID bytes) XOR (reset-cause << 56),
   so it is deterministic per device and reset-cause class.

   KNOWN DESIGN LIMITATION (documented, not a bug to fix in this change):
   because it is UID-derived, boot_id does NOT guarantee a NEW value for two
   boots that share the same reset cause — an MCU-only reset of the same class
   can reproduce the identical boot_id, so it is a device/reset-class
   identifier rather than a true unique boot-session identifier.

   Before network/server end-to-end integration we must decide the contract:
     A) boot_id = true unique boot-session identifier (preferred future), or
     B) boot_id = device/reset-class identifier (current behavior).
   No persistent flash boot counter is written on boot, and none is introduced
   here. */
bool Platform_CreateBootId(uint64_t *boot_id);

#endif