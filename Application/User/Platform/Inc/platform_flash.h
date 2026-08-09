#ifndef PLATFORM_FLASH_H
#define PLATFORM_FLASH_H

#include <stdint.h>
#include <stddef.h>

typedef enum
{
    PLATFORM_FLASH_OK = 0,
    PLATFORM_FLASH_ERROR,
    PLATFORM_FLASH_INVALID_ARG,
    PLATFORM_FLASH_VERIFY_ERROR
} PlatformFlashStatus;

typedef struct
{
    uint32_t start_address;
    uint32_t page_size;
    uint32_t total_size;
    uint32_t page_count;
} PlatformFlashInfo;

const PlatformFlashInfo *Platform_FlashGetInfo(void);

PlatformFlashStatus Platform_FlashRead(uint32_t offset, void *data, size_t size);
PlatformFlashStatus Platform_FlashWrite(uint32_t offset, const void *data, size_t size);
PlatformFlashStatus Platform_FlashErase(uint32_t page_index);

#endif