#include "platform_uid.h"
#include "stm32g4xx_hal.h"

bool Platform_UidGet(uint8_t *out, size_t size)
{
    if (out == NULL) return false;
    if (size < PLATFORM_UID_SIZE) return false;

    uint32_t uid[3];
    uid[0] = *(const uint32_t *)0x1FFF7590;
    uid[1] = *(const uint32_t *)0x1FFF7594;
    uid[2] = *(const uint32_t *)0x1FFF7598;

    for (int i = 0; i < 3; i++)
    {
        out[i * 4 + 0] = (uint8_t)(uid[i] >> 0U);
        out[i * 4 + 1] = (uint8_t)(uid[i] >> 8U);
        out[i * 4 + 2] = (uint8_t)(uid[i] >> 16U);
        out[i * 4 + 3] = (uint8_t)(uid[i] >> 24U);
    }
    return true;
}