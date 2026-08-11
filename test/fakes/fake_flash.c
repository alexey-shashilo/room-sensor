#include "fake_flash.h"
#include <string.h>

static uint8_t s_flash[FAKE_FLASH_SIZE * FAKE_FLASH_MAX_PAGES];
static uint32_t s_page_count = FAKE_FLASH_PAGES;
static bool s_write_fail = false;
static bool s_read_fail = false;
static uint32_t s_read_fail_start = 0;
static uint32_t s_read_fail_end = 0;
static uint32_t s_read_count = 0;
static uint32_t s_erase_count = 0;
static uint32_t s_write_count = 0;
static bool s_bank_supported = true;

void FakeFlash_Init(void)
{
    memset(s_flash, 0xFF, sizeof(s_flash));
    s_page_count = FAKE_FLASH_PAGES;
    s_write_fail = false;
    s_read_fail = false;
    s_read_fail_start = 0;
    s_read_fail_end = 0;
    s_read_count = 0;
    s_erase_count = 0;
    s_write_count = 0;
    s_bank_supported = true;
}

void FakeFlash_SetBankSupported(bool supported)
{
    s_bank_supported = supported;
}

/* Host / fake mapping is supported unless the test overrides it (simulates an
   unsupported dual-bank configuration). */
PlatformFlashStatus Platform_FlashValidateConfiguration(void)
{
    return s_bank_supported ? PLATFORM_FLASH_OK : PLATFORM_FLASH_ERROR;
}

void FakeFlash_SetPageCount(uint32_t pages)
{
    if (pages < 1U || pages > FAKE_FLASH_MAX_PAGES) return;
    s_page_count = pages;
}

uint32_t FakeFlash_GetPageCount(void)
{
    return s_page_count;
}

void FakeFlash_Corrupt(uint32_t offset, size_t size)
{
    if ((uint64_t)offset + size > sizeof(s_flash)) return;
    for (size_t i = 0; i < size; i++)
        s_flash[offset + i] ^= 0xFF;
}

void FakeFlash_SetWriteFail(bool fail)
{
    s_write_fail = fail;
}

void FakeFlash_SetReadFail(bool fail, uint32_t start_offset, uint32_t end_offset)
{
    s_read_fail = fail;
    s_read_fail_start = start_offset;
    s_read_fail_end = end_offset;
}

void *FakeFlash_GetData(void)
{
    return s_flash;
}

void FakeFlash_ResetReadCount(void)
{
    s_read_count = 0;
}

uint32_t FakeFlash_GetReadCount(void)
{
    return s_read_count;
}

void FakeFlash_ResetIoCounters(void)
{
    s_read_count = 0;
    s_erase_count = 0;
    s_write_count = 0;
}

uint32_t FakeFlash_GetEraseCount(void)
{
    return s_erase_count;
}

uint32_t FakeFlash_GetWriteCount(void)
{
    return s_write_count;
}

static uint64_t FakeRegionSize(void)
{
    return (uint64_t)FAKE_FLASH_SIZE * (uint64_t)s_page_count;
}

const PlatformFlashInfo *Platform_FlashGetInfo(void)
{
    static PlatformFlashInfo info;
    info.start_address = 0U;
    info.page_size     = FAKE_FLASH_SIZE;
    info.total_size    = (uint32_t)FakeRegionSize();
    info.page_count    = s_page_count;
    info.program_unit  = FAKE_FLASH_PROGRAM_UNIT;
    return &info;
}

PlatformFlashStatus Platform_FlashRead(uint32_t offset, void *data, size_t size)
{
    if ((data == NULL) || (size == 0)) return PLATFORM_FLASH_INVALID_ARG;
    if ((uint64_t)offset > FakeRegionSize()) return PLATFORM_FLASH_INVALID_ARG;
    if ((uint64_t)size > FakeRegionSize() - (uint64_t)offset) return PLATFORM_FLASH_INVALID_ARG;

    if (s_read_fail &&
        offset < s_read_fail_end && (offset + size) > s_read_fail_start)
        return PLATFORM_FLASH_ERROR;

    s_read_count++;
    memcpy(data, s_flash + offset, size);
    return PLATFORM_FLASH_OK;
}

PlatformFlashStatus Platform_FlashWrite(uint32_t offset, const void *data, size_t size)
{
    if ((data == NULL) || (size == 0)) return PLATFORM_FLASH_INVALID_ARG;
    if ((uint64_t)offset > FakeRegionSize()) return PLATFORM_FLASH_INVALID_ARG;
    if ((uint64_t)size > FakeRegionSize() - (uint64_t)offset) return PLATFORM_FLASH_INVALID_ARG;
    if ((offset % FAKE_FLASH_PROGRAM_UNIT) != 0U) return PLATFORM_FLASH_INVALID_ARG;
    if ((size % FAKE_FLASH_PROGRAM_UNIT) != 0U) return PLATFORM_FLASH_INVALID_ARG;
    if (s_write_fail) return PLATFORM_FLASH_ERROR;
    /* Unsupported MCU Flash config: fail closed, zero writes. */
    if (!s_bank_supported) return PLATFORM_FLASH_ERROR;
    s_write_count++;
    memcpy(s_flash + offset, data, size);
    return PLATFORM_FLASH_OK;
}

PlatformFlashStatus Platform_FlashErase(uint32_t page_index)
{
    if (s_write_fail) return PLATFORM_FLASH_ERROR;
    if (page_index >= s_page_count) return PLATFORM_FLASH_INVALID_ARG;
    /* Unsupported MCU Flash config: fail closed, zero erases. */
    if (!s_bank_supported) return PLATFORM_FLASH_ERROR;
    s_erase_count++;
    memset(s_flash + page_index * FAKE_FLASH_SIZE, 0xFF, FAKE_FLASH_SIZE);
    return PLATFORM_FLASH_OK;
}