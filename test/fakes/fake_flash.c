#include "fake_flash.h"
#include <string.h>

static uint8_t s_flash[FAKE_FLASH_SIZE * FAKE_FLASH_PAGES];
static bool s_write_fail = false;

void FakeFlash_Init(void)
{
    memset(s_flash, 0xFF, sizeof(s_flash));
    s_write_fail = false;
}

void FakeFlash_Corrupt(uint32_t offset, size_t size)
{
    if ((offset + size) > sizeof(s_flash)) return;
    for (size_t i = 0; i < size; i++)
        s_flash[offset + i] ^= 0xFF;
}

void FakeFlash_SetWriteFail(bool fail)
{
    s_write_fail = fail;
}

void *FakeFlash_GetData(void)
{
    return s_flash;
}

const PlatformFlashInfo *Platform_FlashGetInfo(void)
{
    static const PlatformFlashInfo info = {
        .start_address = 0U,
        .page_size     = FAKE_FLASH_SIZE,
        .total_size    = FAKE_FLASH_SIZE * FAKE_FLASH_PAGES,
        .page_count    = FAKE_FLASH_PAGES
    };
    return &info;
}

PlatformFlashStatus Platform_FlashRead(uint32_t offset, void *data, size_t size)
{
    if ((data == NULL) || (size == 0)) return PLATFORM_FLASH_INVALID_ARG;
    if ((offset + size) > sizeof(s_flash)) return PLATFORM_FLASH_INVALID_ARG;
    memcpy(data, s_flash + offset, size);
    return PLATFORM_FLASH_OK;
}

PlatformFlashStatus Platform_FlashWrite(uint32_t offset, const void *data, size_t size)
{
    if ((data == NULL) || (size == 0)) return PLATFORM_FLASH_INVALID_ARG;
    if ((offset + size) > sizeof(s_flash)) return PLATFORM_FLASH_INVALID_ARG;
    if (s_write_fail) return PLATFORM_FLASH_ERROR;
    memcpy(s_flash + offset, data, size);
    return PLATFORM_FLASH_OK;
}

PlatformFlashStatus Platform_FlashErase(uint32_t page_index)
{
    if (s_write_fail) return PLATFORM_FLASH_ERROR;
    if (page_index >= FAKE_FLASH_PAGES) return PLATFORM_FLASH_INVALID_ARG;
    memset(s_flash + page_index * FAKE_FLASH_SIZE, 0xFF, FAKE_FLASH_SIZE);
    return PLATFORM_FLASH_OK;
}