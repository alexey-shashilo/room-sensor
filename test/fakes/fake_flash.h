#ifndef FAKE_FLASH_H
#define FAKE_FLASH_H

#include "platform_flash.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_FLASH_SIZE 2048U
#define FAKE_FLASH_PAGES 6U
#define FAKE_FLASH_MAX_PAGES 16U
#define FAKE_FLASH_PROGRAM_UNIT 8U

void   FakeFlash_Init(void);
void   FakeFlash_Corrupt(uint32_t offset, size_t size);
void   FakeFlash_SetWriteFail(bool fail);
void   FakeFlash_SetReadFail(bool fail, uint32_t start_offset, uint32_t end_offset);
void  *FakeFlash_GetData(void);
void   FakeFlash_ResetReadCount(void);
uint32_t FakeFlash_GetReadCount(void);
void   FakeFlash_ResetIoCounters(void);
uint32_t FakeFlash_GetEraseCount(void);
uint32_t FakeFlash_GetWriteCount(void);
void   FakeFlash_SetPageCount(uint32_t pages);
uint32_t FakeFlash_GetPageCount(void);
/* Simulate an unsupported MCU Flash bank configuration. When false,
   Platform_FlashValidateConfiguration() fails and erase/program fail closed. */
void   FakeFlash_SetBankSupported(bool supported);

#ifdef __cplusplus
}
#endif

#endif