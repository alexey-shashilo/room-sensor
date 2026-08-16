/* DISPLAY BLOCKING CONTRACT (P2-5, decision A):
   Display_Init() performs a bounded blocking init/recovery sequence — two
   Platform_DelayMs(10) power-on/controller-settle delays plus ~20 sub-ms I2C
   writes. Worst-case synchronous cost is ~25 ms, comfortably within the
   WATCHDOG_TIMEOUT_MS (4000) and the App scheduler budget. This is the
   DOCUMENTED truth: display init/recovery is a SHORT, bounded blocking
   operation, NOT part of the fully non-blocking sensor runtime state machines
   (SCD41/SHT45/BMP390). The display never holds the I2C bus or runs a loop, so
   sensor acquisition is only momentarily serialized (<< 1 sensor poll). This
   contract was chosen over a phased non-blocking display state machine because
   the measured worst-case (~25 ms << 4 s watchdog) does not require it and the
   display is an output device, not an environmental sensor. */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>
#include "room_sensor_types.h"
#include "i2c_bus.h"

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
    const I2cBus *bus;
    uint8_t i2c_addr;
    uint8_t initialized;
    uint8_t addr_valid;
    DisplayController controller;
    uint8_t column_offset;
    uint8_t buffer[DISPLAY_WIDTH * DISPLAY_PAGES];
} Display_HandleTypeDef;

bool         Display_Probe(const I2cBus *bus, uint8_t *out_addr);
bool         Display_Init(Display_HandleTypeDef *dev, const I2cBus *bus, uint8_t i2c_addr, DisplayController controller);
void         Display_Clear(Display_HandleTypeDef *dev);
DriverStatus Display_Update(Display_HandleTypeDef *dev);
void         Display_DrawPixel(Display_HandleTypeDef *dev, uint8_t x, uint8_t y, uint8_t color);
void         Display_DrawChar(Display_HandleTypeDef *dev, uint8_t x, uint8_t y, char ch);
void         Display_DrawString(Display_HandleTypeDef *dev, uint8_t x, uint8_t y, const char *str);
void         Display_TestPattern(Display_HandleTypeDef *dev);
bool         Display_IsInitialized(const Display_HandleTypeDef *dev);

#endif