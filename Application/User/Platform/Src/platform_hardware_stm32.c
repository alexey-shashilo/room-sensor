#include "platform_hardware.h"
#include "platform_unique_id.h"
#include "platform_reset.h"
#include "string.h"

bool Platform_GetHardwareInfo(PlatformHardwareInfo *info)
{
    if (info == NULL) return false;
    info->platform_family = "stm32g4";
    info->board_name = "nucleo-g474re";
    return true;
}

bool Platform_CreateBootId(uint64_t *boot_id)
{
    if (boot_id == NULL) return false;

    uint8_t uid[PLATFORM_UNIQUE_ID_SIZE];
    if (!Platform_GetUniqueId(uid, PLATFORM_UNIQUE_ID_SIZE))
    {
        *boot_id = 0;
        return false;
    }

    uint64_t id = 0;
    for (int i = 0; i < 8; i++)
        id = (id << 8U) | (uint64_t)uid[i];

    ResetCause rc = Platform_GetResetCause();
    id ^= ((uint64_t)rc << 56U);

    *boot_id = id;
    return true;
}