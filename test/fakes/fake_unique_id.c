#include "fake_unique_id.h"
#include "platform_unique_id.h"
#include <string.h>

static uint8_t s_fake_uid[12] = {0xAA, 0xBB, 0xCC, 0xDD, 0x01, 0x02, 0x03, 0x04, 0xFE, 0xED, 0xBE, 0xEF};
static bool s_fail_mode = false;
static int s_call_count = 0;

void FakeUniqueId_Set(const uint8_t uid[12])
{
    if (uid)
        memcpy(s_fake_uid, uid, 12);
    else
        memset(s_fake_uid, 0, 12);
}

void FakeUniqueId_SetFail(bool fail)
{
    s_fail_mode = fail;
}

int FakeUniqueId_GetCallCount(void)
{
    return s_call_count;
}

bool Platform_GetUniqueId(uint8_t *out, size_t size)
{
    s_call_count++;
    if (out == NULL) return false;
    if (size < 12) return false;
    if (s_fail_mode) return false;
    memcpy(out, s_fake_uid, 12);
    return true;
}