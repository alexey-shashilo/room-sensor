#include "platform_flash.h"
#include "stm32g4xx_hal.h"

/* STM32G474: physical Flash erase page = 2 KiB (2048 bytes).
   Single bank mode (DBANK=0, default on NUCLEO-G474RE).
   Reserved storage: 6 pages × 2048 = 12 KiB at top of 512 KiB Flash.
   Page index = (address - 0x08000000) / 2048. */

#define FLASH_START_ADDR    0x0807D000U  /* 512KB - 12KB = 500KB offset */
#define STORAGE_PAGE_SIZE   2048U
#define FLASH_TOTAL_SIZE    0x3000U      /* 12 KiB = 6 × 2048 */

static uint32_t FlashAddr(uint32_t offset)
{
    return FLASH_START_ADDR + offset;
}

const PlatformFlashInfo *Platform_FlashGetInfo(void)
{
    static const PlatformFlashInfo info = {
        .start_address = FLASH_START_ADDR,
        .page_size     = STORAGE_PAGE_SIZE,
        .total_size    = FLASH_TOTAL_SIZE,
        .page_count    = 6
    };
    return &info;
}

PlatformFlashStatus Platform_FlashRead(uint32_t offset, void *data, size_t size)
{
    if ((data == NULL) || (size == 0)) return PLATFORM_FLASH_INVALID_ARG;
    if (offset > FLASH_TOTAL_SIZE) return PLATFORM_FLASH_INVALID_ARG;
    if (size > FLASH_TOTAL_SIZE - offset) return PLATFORM_FLASH_INVALID_ARG;

    uint8_t *src = (uint8_t *)(FLASH_START_ADDR + offset);
    for (size_t i = 0; i < size; i++)
        ((uint8_t *)data)[i] = src[i];

    return PLATFORM_FLASH_OK;
}

PlatformFlashStatus Platform_FlashWrite(uint32_t offset, const void *data, size_t size)
{
    if ((data == NULL) || (size == 0)) return PLATFORM_FLASH_INVALID_ARG;
    if (offset > FLASH_TOTAL_SIZE) return PLATFORM_FLASH_INVALID_ARG;
    if (size > FLASH_TOTAL_SIZE - offset) return PLATFORM_FLASH_INVALID_ARG;
    if ((offset % 8U) != 0U) return PLATFORM_FLASH_INVALID_ARG;
    if ((size % 8U) != 0U) return PLATFORM_FLASH_INVALID_ARG;

    HAL_FLASH_Unlock();

    const uint8_t *src = (const uint8_t *)data;
    uint32_t addr = FlashAddr(offset);

    for (size_t i = 0; i < size; i += 8U)
    {
        uint64_t val;
        val  = (uint64_t)src[i];
        val |= (uint64_t)src[i + 1] << 8U;
        val |= (uint64_t)src[i + 2] << 16U;
        val |= (uint64_t)src[i + 3] << 24U;
        val |= (uint64_t)src[i + 4] << 32U;
        val |= (uint64_t)src[i + 5] << 40U;
        val |= (uint64_t)src[i + 6] << 48U;
        val |= (uint64_t)src[i + 7] << 56U;

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, val) != HAL_OK)
        {
            HAL_FLASH_Lock();
            return PLATFORM_FLASH_ERROR;
        }
        addr += 8U;
    }

    HAL_FLASH_Lock();

    return PLATFORM_FLASH_OK;
}

PlatformFlashStatus Platform_FlashErase(uint32_t page_index)
{
    if (page_index >= 6) return PLATFORM_FLASH_INVALID_ARG;

    /* Page index is relative to the start of the storage region.
       Physical page number: pages 0-249 are firmware (0..500KB / 2KB - 1).
       Storage pages start at page 250 (0x0807D000 / 2048 = 250). */
    uint32_t phys_page = 250 + page_index;

    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase = {0};
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.Banks = FLASH_BANK_1;
    erase.Page = phys_page;
    erase.NbPages = 1;

    uint32_t page_error = 0;
    HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase, &page_error);

    HAL_FLASH_Lock();

    if ((status != HAL_OK) || (page_error != 0xFFFFFFFFU))
        return PLATFORM_FLASH_ERROR;

    return PLATFORM_FLASH_OK;
}