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
    uint32_t program_unit;   /* minimum write granularity in bytes (power of two) */
} PlatformFlashInfo;

const PlatformFlashInfo *Platform_FlashGetInfo(void);

/* Validate that the actual MCU Flash configuration matches the mapping this
   driver assumes (e.g. STM32G474 single-bank DBANK=0, storage pages at the
   mapped region). Returns PLATFORM_FLASH_OK when supported; otherwise the
   driver must fail closed and MUST NOT erase or program. Called at Storage_Init
   and re-checked before every erase/program. */
PlatformFlashStatus Platform_FlashValidateConfiguration(void);

PlatformFlashStatus Platform_FlashRead(uint32_t offset, void *data, size_t size);
PlatformFlashStatus Platform_FlashWrite(uint32_t offset, const void *data, size_t size);
PlatformFlashStatus Platform_FlashErase(uint32_t page_index);

#endif