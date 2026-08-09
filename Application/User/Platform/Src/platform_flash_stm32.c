#include "platform_flash.h"
#include "stm32g4xx_hal.h"

#define FLASH_START_ADDR  0x0807E000U
#define STORAGE_PAGE_SIZE  0x1000U
#define FLASH_TOTAL_SIZE   0x2000U

static const PlatformFlashInfo s_info = {
    .start_address = FLASH_START_ADDR,
    .page_size     = STORAGE_PAGE_SIZE,
    .total_size    = FLASH_TOTAL_SIZE,
    .page_count    = 2
};

const PlatformFlashInfo *Platform_FlashGetInfo(void)
{
    return &s_info;
}

PlatformFlashStatus Platform_FlashRead(uint32_t offset, void *data, size_t size)
{
    if ((data == NULL) || (size == 0)) return PLATFORM_FLASH_INVALID_ARG;
    if ((offset + size) > FLASH_TOTAL_SIZE) return PLATFORM_FLASH_INVALID_ARG;

    uint8_t *src = (uint8_t *)(FLASH_START_ADDR + offset);
    for (size_t i = 0; i < size; i++)
        ((uint8_t *)data)[i] = src[i];

    return PLATFORM_FLASH_OK;
}

PlatformFlashStatus Platform_FlashWrite(uint32_t offset, const void *data, size_t size)
{
    if ((data == NULL) || (size == 0)) return PLATFORM_FLASH_INVALID_ARG;
    if ((offset + size) > FLASH_TOTAL_SIZE) return PLATFORM_FLASH_INVALID_ARG;
    if ((offset % 8U) != 0U) return PLATFORM_FLASH_INVALID_ARG;
    if ((size % 8U) != 0U) return PLATFORM_FLASH_INVALID_ARG;

    HAL_FLASH_Unlock();

    const uint8_t *src = (const uint8_t *)data;
    uint32_t addr = FLASH_START_ADDR + offset;

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

    uint8_t verify[256];
    size_t check = (size > sizeof(verify)) ? sizeof(verify) : size;
    if (Platform_FlashRead(offset, verify, check) != PLATFORM_FLASH_OK)
        return PLATFORM_FLASH_ERROR;

    for (size_t i = 0; i < check; i++)
    {
        if (verify[i] != ((const uint8_t *)data)[i])
            return PLATFORM_FLASH_VERIFY_ERROR;
    }

    return PLATFORM_FLASH_OK;
}

PlatformFlashStatus Platform_FlashErase(uint32_t page_index)
{
    uint32_t page = (FLASH_START_ADDR + page_index * STORAGE_PAGE_SIZE - 0x08000000U) / STORAGE_PAGE_SIZE;

    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase = {0};
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.Page = page;
    erase.NbPages = 1;

    uint32_t page_error = 0;
    HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase, &page_error);

    HAL_FLASH_Lock();

    if ((status != HAL_OK) || (page_error != 0xFFFFFFFFU))
        return PLATFORM_FLASH_ERROR;

    return PLATFORM_FLASH_OK;
}