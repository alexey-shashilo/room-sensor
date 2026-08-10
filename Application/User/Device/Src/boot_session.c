#include "boot_session.h"
#include "platform_hardware.h"
#include <stddef.h>

static uint64_t s_boot_id = 0;
static bool s_initialized = false;

bool BootSession_Get(BootSession *session)
{
    if (session == NULL) return false;

    if (!s_initialized)
    {
        Platform_CreateBootId(&s_boot_id);
        s_initialized = true;
    }

    session->boot_id = s_boot_id;
    return true;
}