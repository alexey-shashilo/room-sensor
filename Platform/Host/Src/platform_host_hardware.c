/* Host Platform — Hardware info and boot ID */

#include "host_platform.h"
#include "platform_hardware.h"
#include "platform_unique_id.h"
#include <string.h>

static bool s_boot_id_seeded = false;
static uint64_t s_boot_id = 0;

void HostPlatform_SetBootId(uint64_t id)
{
    s_boot_id = id;
    s_boot_id_seeded = true;
}

bool Platform_GetHardwareInfo(PlatformHardwareInfo *info)
{
    if (info == NULL) return false;
    info->platform_family = "host";
    info->board_name = "room-sensor-host";
    return true;
}

bool Platform_CreateBootId(uint64_t *boot_id)
{
    if (boot_id == NULL) return false;

    if (s_boot_id_seeded)
    {
        *boot_id = s_boot_id;
        return true;
    }

    uint8_t uid[PLATFORM_UNIQUE_ID_SIZE];
    if (!Platform_GetUniqueId(uid, PLATFORM_UNIQUE_ID_SIZE))
    {
        *boot_id = 0;
        return false;
    }

    uint64_t id = 0;
    for (int i = 0; i < 8; i++)
        id = (id << 8U) | (uint64_t)uid[i];

    *boot_id = id;
    return true;
}