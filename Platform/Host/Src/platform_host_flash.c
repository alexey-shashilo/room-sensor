/* Host Platform — Flash implementation (in-memory byte array, erased=0xFF) */

#include "host_platform.h"
#include "platform_flash.h"
#include <string.h>

#define HOST_FLASH_PAGE_SIZE  2048U
#define HOST_FLASH_PAGES      6U
#define HOST_FLASH_SIZE       (HOST_FLASH_PAGE_SIZE * HOST_FLASH_PAGES)
#define HOST_PROGRAM_UNIT     8U
static uint8_t s_flash[HOST_FLASH_SIZE];
static bool s_write_fail = false;

void HostFlash_Init(void)
{
    memset(s_flash, 0xFF, sizeof(s_flash));
    s_write_fail = false;
}

void *HostFlash_GetData(void) { return s_flash; }
void HostFlash_SetWriteFail(bool fail) { s_write_fail = fail; }

const PlatformFlashInfo *Platform_FlashGetInfo(void)
{
    static const PlatformFlashInfo info = {
        .start_address = 0U,
        .page_size     = HOST_FLASH_PAGE_SIZE,
        .total_size    = HOST_FLASH_SIZE,
        .page_count    = HOST_FLASH_PAGES,
        .program_unit  = HOST_PROGRAM_UNIT
    };
    return &info;
}

/* Host in-memory mapping always matches the driver layout. */
PlatformFlashStatus Platform_FlashValidateConfiguration(void)
{
    return PLATFORM_FLASH_OK;
}

PlatformFlashStatus Platform_FlashRead(uint32_t offset, void *data, size_t size)
{
    if ((data == NULL) || (size == 0)) return PLATFORM_FLASH_INVALID_ARG;
    if (offset > sizeof(s_flash)) return PLATFORM_FLASH_INVALID_ARG;
    if (size > sizeof(s_flash) - offset) return PLATFORM_FLASH_INVALID_ARG;
    memcpy(data, s_flash + offset, size);
    return PLATFORM_FLASH_OK;
}

PlatformFlashStatus Platform_FlashWrite(uint32_t offset, const void *data, size_t size)
{
    if ((data == NULL) || (size == 0)) return PLATFORM_FLASH_INVALID_ARG;
    if (offset > sizeof(s_flash)) return PLATFORM_FLASH_INVALID_ARG;
    if (size > sizeof(s_flash) - offset) return PLATFORM_FLASH_INVALID_ARG;
    if ((offset % HOST_PROGRAM_UNIT) != 0U) return PLATFORM_FLASH_INVALID_ARG;
    if ((size % HOST_PROGRAM_UNIT) != 0U) return PLATFORM_FLASH_INVALID_ARG;
    if (s_write_fail) return PLATFORM_FLASH_ERROR;
    memcpy(s_flash + offset, data, size);
    return PLATFORM_FLASH_OK;
}

PlatformFlashStatus Platform_FlashErase(uint32_t page_index)
{
    if (s_write_fail) return PLATFORM_FLASH_ERROR;
    if (page_index >= HOST_FLASH_PAGES) return PLATFORM_FLASH_INVALID_ARG;
    uint32_t off = page_index * HOST_FLASH_PAGE_SIZE;
    if (off >= HOST_FLASH_SIZE) return PLATFORM_FLASH_INVALID_ARG;
    memset(s_flash + off, 0xFF, HOST_FLASH_PAGE_SIZE);
    return PLATFORM_FLASH_OK;
}