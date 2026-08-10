/* Host Platform — Unique ID */

#include "host_platform.h"
#include "platform_unique_id.h"
#include <string.h>

static uint8_t s_uid[12] = {0xAA,0xBB,0xCC,0xDD,0x01,0x02,0x03,0x04,0xFE,0xED,0xBE,0xEF};
static bool s_fail = false;
static int s_call_count = 0;

void HostUid_Set(const uint8_t uid[12]) { if (uid) memcpy(s_uid, uid, 12); else memset(s_uid, 0, 12); }
void HostUid_SetFail(bool fail) { s_fail = fail; }
int  HostUid_GetCallCount(void) { return s_call_count; }

bool Platform_GetUniqueId(uint8_t *out, size_t size)
{
    s_call_count++;
    if (out == NULL || size < 12) return false;
    if (s_fail) return false;
    memcpy(out, s_uid, 12);
    return true;
}