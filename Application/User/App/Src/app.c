#include "app.h"
#include "veml7700.h"
#include "ssd1306.h"
#include <stdio.h>

#define LINE_BUF_SIZE 32U

static VEML7700_HandleTypeDef s_veml7700;
static SSD1306_HandleTypeDef  s_ssd1306;
static I2C_HandleTypeDef     *s_hi2c1 = NULL;

static bool s_veml7700_present = false;
static bool s_ssd1306_present = false;
static float s_last_lux = 0.0f;

bool g_veml7700_present = false;
float g_last_lux = 0.0f;
uint8_t g_veml7700_initialized = 0U;
bool g_ssd1306_present = false;
uint8_t g_ssd1306_initialized = 0U;
uint8_t g_ssd1306_addr = 0U;

void App_SetI2C(I2C_HandleTypeDef *hi2c)
{
    s_hi2c1 = hi2c;
}

I2C_HandleTypeDef *App_GetI2C(void)
{
    return s_hi2c1;
}

static void App_DetectAndInit(void)
{
    bool ssd1306_found = false;
    uint8_t ssd1306_addr = 0U;

    s_veml7700_present =
        (HAL_I2C_IsDeviceReady(s_hi2c1, VEML7700_I2C_ADDR, 3U, 100U) == HAL_OK);
    g_veml7700_present = s_veml7700_present;

    if (s_veml7700_present)
    {
        g_veml7700_initialized = VEML7700_Init(&s_veml7700, s_hi2c1) ? 1U : 0U;
        s_veml7700_present = (g_veml7700_initialized != 0U);
        g_veml7700_present = s_veml7700_present;
    }

    if (HAL_I2C_IsDeviceReady(s_hi2c1, SSD1306_I2C_ADDR, 3U, 150U) == HAL_OK)
    {
        ssd1306_addr = SSD1306_I2C_ADDR;
        ssd1306_found = true;
    }
    else if (HAL_I2C_IsDeviceReady(s_hi2c1, SSD1306_I2C_ADDR_ALT, 3U, 150U) == HAL_OK)
    {
        ssd1306_addr = SSD1306_I2C_ADDR_ALT;
        ssd1306_found = true;
    }

    s_ssd1306_present = ssd1306_found;
    g_ssd1306_present = s_ssd1306_present;
    g_ssd1306_addr = ssd1306_addr;

if (s_ssd1306_present)
        {
            s_ssd1306.addr = ssd1306_addr;
            if (SSD1306_Init(&s_ssd1306, s_hi2c1))
            {
                g_ssd1306_initialized = 1U;
            }
        }
}

RoomSensor_Status App_Init(void)
{
    return (s_hi2c1 != NULL) ? ROOM_SENSOR_OK : ROOM_SENSOR_ERROR;
}

void App_Run(void)
{
    static bool s_init_done = false;

    if (!s_init_done)
    {
        s_init_done = true;
        App_DetectAndInit();
    }

    if (s_veml7700_present)
    {
        float test_lux = 0.0f;
        bool lux_ok = VEML7700_ReadLux(&s_veml7700, &test_lux);
        if (lux_ok)
        {
            g_last_lux = test_lux;
            s_last_lux = test_lux;
        }
    }

    {
        static uint32_t s_log_cnt = 0U;
        if ((s_log_cnt++ % 20U) == 0U)
        {
            printf("SSD present=%d init=%d addr=0x%02X  VEML present=%d lux=%.0f\r\n",
                   (int)g_ssd1306_present, (int)g_ssd1306_initialized,
                   g_ssd1306_addr, (int)g_veml7700_present, (double)g_last_lux);
        }
    }

    if (s_ssd1306_present)
    {
        char line[LINE_BUF_SIZE];
        int len;

        SSD1306_Clear(&s_ssd1306);
        SSD1306_DrawString(&s_ssd1306, 0, 0, "Room Sensor");

        if (s_veml7700_present)
        {
            len = snprintf(line, sizeof(line), "Light: %.0f lx", (double)s_last_lux);
            if ((len > 0) && ((size_t)len < sizeof(line)))
            {
                SSD1306_DrawString(&s_ssd1306, 0, 16, line);
            }
            else
            {
                SSD1306_DrawString(&s_ssd1306, 0, 16, "Light: N/A");
            }
        }
        else
        {
            SSD1306_DrawString(&s_ssd1306, 0, 16, "No light sensor");
        }

        SSD1306_Update(&s_ssd1306);
    }
}