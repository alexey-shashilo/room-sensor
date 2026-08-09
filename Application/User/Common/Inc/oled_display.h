#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include "stm32g4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

#define OLED_WIDTH     128U
#define OLED_HEIGHT    64U
#define OLED_PAGES     (OLED_HEIGHT / 8U)

typedef enum
{
    OLED_CONTROLLER_UNKNOWN = 0,
    OLED_CONTROLLER_SSD1306,
    OLED_CONTROLLER_SH1106
} OLED_ControllerType;

typedef struct
{
    I2C_HandleTypeDef *hi2c;
    uint8_t            addr;
    uint8_t            initialized;
    OLED_ControllerType controller;
    uint8_t            buffer[OLED_WIDTH * OLED_PAGES];
} OLED_HandleTypeDef;

bool OLED_Init(OLED_HandleTypeDef *dev, I2C_HandleTypeDef *hi2c);
void OLED_Clear(OLED_HandleTypeDef *dev);
void OLED_Update(OLED_HandleTypeDef *dev);
void OLED_DrawPixel(OLED_HandleTypeDef *dev, uint8_t x, uint8_t y, uint8_t color);
void OLED_DrawChar(OLED_HandleTypeDef *dev, uint8_t x, uint8_t y, char ch);
void OLED_DrawString(OLED_HandleTypeDef *dev, uint8_t x, uint8_t y, const char *str);
void OLED_TestPattern(OLED_HandleTypeDef *dev);
bool OLED_IsInitialized(const OLED_HandleTypeDef *dev);

#endif