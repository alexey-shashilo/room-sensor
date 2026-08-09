#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>
#include "room_sensor_types.h"

#define DISPLAY_WIDTH     128U
#define DISPLAY_HEIGHT    64U
#define DISPLAY_PAGES     (DISPLAY_HEIGHT / 8U)

typedef enum
{
    DISPLAY_CONTROLLER_SSD1306,
    DISPLAY_CONTROLLER_SH1106
} DisplayController;

typedef struct
{
    void *i2c_bus;
    uint8_t i2c_addr;
    uint8_t initialized;
    DisplayController controller;
    uint8_t column_offset;
    uint8_t buffer[DISPLAY_WIDTH * DISPLAY_PAGES];
    DeviceCounters counters;
} Display_HandleTypeDef;

bool Display_Probe(void *i2c_bus, uint8_t *out_addr);
bool Display_Init(Display_HandleTypeDef *dev, void *i2c_bus, uint8_t i2c_addr, DisplayController controller);
void Display_Clear(Display_HandleTypeDef *dev);
void Display_Update(Display_HandleTypeDef *dev);
void Display_DrawPixel(Display_HandleTypeDef *dev, uint8_t x, uint8_t y, uint8_t color);
void Display_DrawChar(Display_HandleTypeDef *dev, uint8_t x, uint8_t y, char ch);
void Display_DrawString(Display_HandleTypeDef *dev, uint8_t x, uint8_t y, const char *str);
void Display_TestPattern(Display_HandleTypeDef *dev);
bool Display_IsInitialized(const Display_HandleTypeDef *dev);

#endif