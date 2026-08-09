#ifndef APP_H
#define APP_H

#include "room_sensor_types.h"
#include "stm32g4xx_hal.h"

RoomSensor_Status App_Init(void);
void               App_Run(void);
void               App_SetI2C(I2C_HandleTypeDef *hi2c);
I2C_HandleTypeDef *App_GetI2C(void);

extern bool    g_veml7700_present;
extern float   g_last_lux;
extern uint8_t g_veml7700_initialized;
extern bool    g_ssd1306_present;
extern uint8_t g_ssd1306_initialized;
extern uint8_t g_ssd1306_addr;

#endif