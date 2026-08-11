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

void   FakeFlash_Init(void);
void   FakeFlash_Corrupt(uint32_t offset, size_t size);
void   FakeFlash_SetWriteFail(bool fail);
void   FakeFlash_SetReadFail(bool fail, uint32_t start_offset, uint32_t end_offset);
void  *FakeFlash_GetData(void);

#ifdef __cplusplus
}
#endif

#endif