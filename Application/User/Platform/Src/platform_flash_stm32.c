#include "platform_flash.h"
#include "stm32g4xx_hal.h"

/* ================================================================
   STM32G474 persistence mapping (single-bank DBANK=0 only).
   Physical Flash erase page = 2 KiB. Single-bank (DBANK=0) is the
   default on NUCLEO-G474RE and gives 256 pages (0..255).
   Reserved storage: 6 pages x 2048 = 12 KiB at top of 512 KiB Flash.
   Page index = (address - 0x08000000) / 2048. Storage pages 250..255.
   ================================================================ */

#define FLASH_START_ADDR    0x0807D000U  /* 512KB - 12KB = 500KB offset */
#define STORAGE_PAGE_SIZE   2048U
#define FLASH_TOTAL_SIZE    0x3000U      /* 12 KiB = 6 x 2048 */
#define STORAGE_PROGRAM_UNIT 8U          /* doubleword = 8-byte program granularity */
#define STORAGE_PHYS_BASE   0x08000000U  /* Flash base address */

/* Physical page index of the storage region start. */
#define STORAGE_FIRST_PHYS_PAGE \
    ((FLASH_START_ADDR - STORAGE_PHYS_BASE) / STORAGE_PAGE_SIZE)  /* 250 */

static uint32_t FlashAddr(uint32_t offset)
{
    return FLASH_START_ADDR + offset;
}

/* ================================================================
   Configuration validation.
   The driver's mapping is only correct when the MCU is in single-bank
   (DBANK=0) mode. In dual-bank (DBANK=1, 64-bit) mode the page numbering
   and bank layout differ; erasing with FLASH_BANK_1 + this mapping could
   target the wrong memory. Therefore we REQUIRE single-bank mode and
   fail closed otherwise (persistent storage unavailable, NO erase).
   ================================================================ */
PlatformFlashStatus Platform_FlashValidateConfiguration(void)
{
    /* FLASH->OPTR bit FLASH_OPTR_DBANK set = dual-bank (64-bit) mode. */
    if (FLASH->OPTR & FLASH_OPTR_DBANK)
        return PLATFORM_FLASH_ERROR;
    return PLATFORM_FLASH_OK;
}

const PlatformFlashInfo *Platform_FlashGetInfo(void)
{
    static const PlatformFlashInfo info = {
        .start_address = FLASH_START_ADDR,
        .page_size     = STORAGE_PAGE_SIZE,
        .total_size    = FLASH_TOTAL_SIZE,
        .page_count    = 6,
        .program_unit  = STORAGE_PROGRAM_UNIT
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
    if ((offset % STORAGE_PROGRAM_UNIT) != 0U) return PLATFORM_FLASH_INVALID_ARG;
    if ((size % STORAGE_PROGRAM_UNIT) != 0U) return PLATFORM_FLASH_INVALID_ARG;

    /* Fail closed if the MCU Flash layout does not match the driver mapping. */
    if (Platform_FlashValidateConfiguration() != PLATFORM_FLASH_OK)
        return PLATFORM_FLASH_ERROR;

    if (HAL_FLASH_Unlock() != HAL_OK)
        return PLATFORM_FLASH_ERROR;

    const uint8_t *src = (const uint8_t *)data;
    uint32_t addr = FlashAddr(offset);

    for (size_t i = 0; i < size; i += STORAGE_PROGRAM_UNIT)
    {
        uint64_t val = 0;
        for (size_t b = 0; b < STORAGE_PROGRAM_UNIT; b++)
            val |= (uint64_t)src[i + b] << (8U * b);

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, val) != HAL_OK)
        {
            __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
            HAL_FLASH_Lock();
            return PLATFORM_FLASH_ERROR;
        }
        addr += STORAGE_PROGRAM_UNIT;
    }

    HAL_FLASH_Lock();

    return PLATFORM_FLASH_OK;
}

PlatformFlashStatus Platform_FlashErase(uint32_t page_index)
{
    if (page_index >= 6) return PLATFORM_FLASH_INVALID_ARG;

    /* Critical invariant: if the actual MCU Flash configuration does not match
       the mapping assumed by the driver, do NOT calculate page numbers and
       erase anyway. Fail closed (zero erase). */
    if (Platform_FlashValidateConfiguration() != PLATFORM_FLASH_OK)
        return PLATFORM_FLASH_ERROR;

    if (HAL_FLASH_Unlock() != HAL_OK)
        return PLATFORM_FLASH_ERROR;

    /* Physical page number: storage pages start at 250 (0x0807D000 / 2048). */
    uint32_t phys_page = STORAGE_FIRST_PHYS_PAGE + page_index;

    FLASH_EraseInitTypeDef erase = {0};
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.Banks = FLASH_BANK_1;
    erase.Page = phys_page;
    erase.NbPages = 1;

    uint32_t page_error = 0;
    HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase, &page_error);

    HAL_FLASH_Lock();

    if ((status != HAL_OK) || (page_error != 0xFFFFFFFFU))
    {
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
        return PLATFORM_FLASH_ERROR;
    }

    return PLATFORM_FLASH_OK;
}