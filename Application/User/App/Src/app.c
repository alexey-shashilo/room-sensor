#include "app.h"
#include "veml7700.h"
#include "display.h"
#include "i2c_bus.h"
#include <stdio.h>

extern uint32_t HAL_GetTick(void);

#define LINE_BUF_SIZE 32U

#define PERIOD_LIGHT_MS     500U
#define PERIOD_DISPLAY_MS   500U
#define PERIOD_HEALTH_MS    5000U
#define PERIOD_LOG_MS       10000U

#define RETRY_PROBE_MS      5000U
#define RETRY_INIT_MS       5000U

static VEML7700_HandleTypeDef s_veml;
static Display_HandleTypeDef  s_display;
static I2cBus                 s_i2c_bus;
static bool s_i2c_ready = false;

static float   s_last_lux = 0.0f;
static bool    s_lux_valid = false;

static DeviceState s_veml_state = DEVICE_STATE_UNKNOWN;
static DeviceState s_display_state = DEVICE_STATE_UNKNOWN;

static uint32_t s_last_light_ms = 0;
static uint32_t s_last_display_ms = 0;
static uint32_t s_last_health_ms = 0;
static uint32_t s_last_log_ms = 0;

static void App_HealthCheck(void);

static RoomSensor_Status App_InitDevices(void)
{
    if (!s_i2c_ready) return ROOM_SENSOR_ERROR;

    s_veml_state = DEVICE_STATE_NOT_FOUND;
    s_display_state = DEVICE_STATE_NOT_FOUND;

    App_HealthCheck();

    return ROOM_SENSOR_OK;
}

RoomSensor_Status App_Init(void)
{
    if (!s_i2c_ready) return ROOM_SENSOR_ERROR;
    return App_InitDevices();
}

void App_SetI2C(void *bus)
{
    if (bus == NULL) return;
    s_i2c_bus = *(const I2cBus *)bus;
    s_i2c_ready = true;
}

static void App_ProbeVeml(void)
{
    if (VEML7700_Probe(&s_veml, &s_i2c_bus))
    {
        s_veml_state = DEVICE_STATE_INITIALIZING;
    }
    else
    {
        s_veml_state = DEVICE_STATE_PROBING;
    }
}

static void App_ProbeDisplay(void)
{
    uint8_t addr;
    if (Display_Probe(&s_i2c_bus, &addr))
    {
        s_display_state = DEVICE_STATE_INITIALIZING;
    }
    else
    {
        s_display_state = DEVICE_STATE_PROBING;
    }
}

static void App_InitVeml(void)
{
    if (VEML7700_Init(&s_veml, &s_i2c_bus,
                      VEML7700_GAIN_1_8,
                      VEML7700_IT_25_MS,
                      VEML7700_PERS_1))
    {
        s_veml_state = DEVICE_STATE_READY;
    }
    else
    {
        s_veml_state = DEVICE_STATE_ERROR;
    }
}

static void App_InitDisplay(void)
{
    uint8_t addr;
    Display_Probe(&s_i2c_bus, &addr);

    if (Display_Init(&s_display, &s_i2c_bus, addr, DISPLAY_CONTROLLER_SH1106))
    {
        s_display_state = DEVICE_STATE_READY;
    }
    else
    {
        s_display_state = DEVICE_STATE_ERROR;
    }
}

static void App_HealthCheck(void)
{
    if ((s_veml_state == DEVICE_STATE_NOT_FOUND) || (s_veml_state == DEVICE_STATE_PROBING) || (s_veml_state == DEVICE_STATE_ERROR))
    {
        App_ProbeVeml();
    }

    if (s_veml_state == DEVICE_STATE_INITIALIZING)
    {
        App_InitVeml();
    }

    if ((s_display_state == DEVICE_STATE_NOT_FOUND) || (s_display_state == DEVICE_STATE_PROBING) || (s_display_state == DEVICE_STATE_ERROR))
    {
        App_ProbeDisplay();
    }

    if (s_display_state == DEVICE_STATE_INITIALIZING)
    {
        App_InitDisplay();
    }
}

void App_Run(void)
{
    uint32_t now = HAL_GetTick();

    if ((now - s_last_health_ms) >= PERIOD_HEALTH_MS)
    {
        s_last_health_ms = now;
        App_HealthCheck();
    }

    if ((now - s_last_light_ms) >= PERIOD_LIGHT_MS)
    {
        s_last_light_ms = now;
        if (s_veml_state == DEVICE_STATE_READY)
        {
            float lux;
            if (VEML7700_ReadLux(&s_veml, &lux))
            {
                s_last_lux = lux;
                s_lux_valid = true;
            }
        }
    }

    if ((now - s_last_display_ms) >= PERIOD_DISPLAY_MS)
    {
        s_last_display_ms = now;
        if (s_display_state == DEVICE_STATE_READY)
        {
            char line[LINE_BUF_SIZE];
            int len;

            Display_Clear(&s_display);
            Display_DrawString(&s_display, 0, 0, "Room Sensor");

            if (s_lux_valid)
            {
                len = snprintf(line, sizeof(line), "Light: %.0f lx", (double)s_last_lux);
                if ((len > 0) && ((size_t)len < sizeof(line)))
                {
                    Display_DrawString(&s_display, 0, 16, line);
                }
                else
                {
                    Display_DrawString(&s_display, 0, 16, "Light: N/A");
                }
            }
            else
            {
                Display_DrawString(&s_display, 0, 16, "No light sensor");
            }

            Display_Update(&s_display);
        }
    }

    if ((now - s_last_log_ms) >= PERIOD_LOG_MS)
    {
        s_last_log_ms = now;
        printf("VEML state=%d lux=%.0f disp state=%d\r\n",
               (int)s_veml_state, (double)s_last_lux, (int)s_display_state);
    }
}

void App_GetStatus(App_Status *status)
{
    if (status == NULL) return;
    status->illuminance_lux = s_last_lux;
    status->illuminance_valid = s_lux_valid;
    status->veml7700_state = s_veml_state;
    status->display_state = s_display_state;
    status->veml7700_counters = s_veml.counters;
    status->display_counters = s_display.counters;
}