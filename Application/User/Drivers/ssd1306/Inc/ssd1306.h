#ifndef SSD1306_H
#define SSD1306_H

#include "stm32g4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

#define SSD1306_I2C_ADDR         (0x3CU << 1U)
#define SSD1306_I2C_ADDR_ALT     (0x3DU << 1U)
#define SSD1306_WIDTH            128U
#define SSD1306_HEIGHT           64U
#define SSD1306_PAGES            (SSD1306_HEIGHT / 8U)

#define SSD1306_COLUMN_OFFSET    0U
#define SH1106_COLUMN_OFFSET     2U

#define SSD1306_I2C_TIMEOUT_MS   100U
#define SSD1306_PAGE_CMD_BASE    0xB0U

#define FONT_WIDTH               5U
#define FONT_HEIGHT              8U
#define FONT_CHAR_SPACING        6U

typedef struct
{
    I2C_HandleTypeDef *hi2c;
    uint8_t            addr;
    uint8_t            initialized;
    uint8_t            column_offset;
    uint8_t            buffer[SSD1306_WIDTH * SSD1306_PAGES];
} SSD1306_HandleTypeDef;

bool SSD1306_Init(SSD1306_HandleTypeDef *dev, I2C_HandleTypeDef *hi2c);
bool SSD1306_InitSH1106(SSD1306_HandleTypeDef *dev, I2C_HandleTypeDef *hi2c);
void SSD1306_Clear(SSD1306_HandleTypeDef *dev);
void SSD1306_Update(SSD1306_HandleTypeDef *dev);
void SSD1306_DrawChar(SSD1306_HandleTypeDef *dev, uint8_t x, uint8_t y, char ch);
void SSD1306_DrawString(SSD1306_HandleTypeDef *dev, uint8_t x, uint8_t y, const char *str);
void SSD1306_DrawPixel(SSD1306_HandleTypeDef *dev, uint8_t x, uint8_t y, uint8_t color);
void SSD1306_TestPattern(SSD1306_HandleTypeDef *dev);
bool SSD1306_IsInitialized(const SSD1306_HandleTypeDef *dev);

#endif