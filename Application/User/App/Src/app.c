#include "app.h"
#include "room_state.h"
#include "config.h"
#include "veml7700.h"
#include "display.h"
#include "platform_time.h"
#include <stdio.h>

static RoomState            s_room;
static VEML7700_HandleTypeDef s_veml;
static Display_HandleTypeDef  s_display;

static const I2cBus *s_i2c_bus = NULL;

static DeviceRuntime s_light_rt = { .state = DEVICE_STATE_UNKNOWN };
static DeviceRuntime s_disp_rt  = { .state = DEVICE_STATE_UNKNOWN };

static uint8_t s_display_addr = 0U;
static bool    s_display_addr_valid = false;

static uint32_t s_last_light_ms = 0;
static uint32_t s_last_display_ms = 0;
static uint32_t s_last_retry_ms = 0;
static uint32_t s_last_diag_ms = 0;
static uint32_t s_start_ms = 0;

static void DeviceRuntime_Init(DeviceRuntime *rt, DeviceState initial)
{
    rt->state = initial;
    rt->init_attempts = 0;
    rt->init_failures = 0;
    rt->operation_successes = 0;
    rt->operation_failures = 0;
    rt->recovery_count = 0;
    rt->consecutive_errors = 0;
    rt->last_success_ms = 0;
    rt->last_failure_ms = 0;
}

static void DeviceRuntime_RecordSuccess(DeviceRuntime *rt)
{
    rt->operation_successes++;
    rt->consecutive_errors = 0;
    rt->last_success_ms = Platform_GetTickMs();
}

static void DeviceRuntime_RecordFailure(DeviceRuntime *rt)
{
    rt->operation_failures++;
    rt->consecutive_errors++;
    rt->last_failure_ms = Platform_GetTickMs();
}

void App_SetI2C(const I2cBus *bus)
{
    s_i2c_bus = bus;
}

static void App_DoProbeVeml(void)
{
    if (VEML7700_Probe(s_i2c_bus))
        s_light_rt.state = DEVICE_STATE_INITIALIZING;
    else
        s_light_rt.state = DEVICE_STATE_NOT_FOUND;
}

static void App_DoInitVeml(void)
{
    s_light_rt.init_attempts++;
    if (VEML7700_Init(&s_veml, s_i2c_bus))
    {
        s_light_rt.state = DEVICE_STATE_READY;
        DeviceRuntime_RecordSuccess(&s_light_rt);
    }
    else
    {
        s_light_rt.init_failures++;
        s_light_rt.state = DEVICE_STATE_ERROR;
        DeviceRuntime_RecordFailure(&s_light_rt);
    }
}

static void App_DoReadLight(void)
{
    VEML7700_Sample sample;
    if (VEML7700_ReadWithAutoRange(&s_veml, &sample))
    {
        if (sample.valid)
        {
            RoomState_UpdateIlluminance(&s_room, sample.lux, true);
            DeviceRuntime_RecordSuccess(&s_light_rt);
        }
        else
        {
            RoomState_UpdateIlluminance(&s_room, s_room.illuminance_lux, false);
        }
        return;
    }

    DeviceRuntime_RecordFailure(&s_light_rt);
    if (s_light_rt.consecutive_errors >= CONSECUTIVE_ERROR_THRESHOLD)
        s_light_rt.state = DEVICE_STATE_ERROR;
}

static void App_DoProbeDisplay(void)
{
    uint8_t addr;
    if (Display_Probe(s_i2c_bus, &addr))
    {
        s_display_addr = addr;
        s_display_addr_valid = true;
        s_disp_rt.state = DEVICE_STATE_INITIALIZING;
    }
    else
    {
        s_display_addr_valid = false;
        s_disp_rt.state = DEVICE_STATE_NOT_FOUND;
    }
}

static void App_DoInitDisplay(void)
{
    s_disp_rt.init_attempts++;

    if (!s_display_addr_valid)
    {
        s_disp_rt.state = DEVICE_STATE_ERROR;
        return;
    }

    if (Display_Init(&s_display, s_i2c_bus, s_display_addr, DISPLAY_CONTROLLER_SH1106))
    {
        s_disp_rt.state = DEVICE_STATE_READY;
        DeviceRuntime_RecordSuccess(&s_disp_rt);
    }
    else
    {
        s_disp_rt.init_failures++;
        s_disp_rt.state = DEVICE_STATE_ERROR;
        DeviceRuntime_RecordFailure(&s_disp_rt);
    }
}

static void App_DoUpdateDisplay(void)
{
    char buf[22];
    const RoomState *room = RoomState_Get(&s_room);

    Display_Clear(&s_display);
    Display_DrawString(&s_display, 0, 0, "Room Sensor");

    if (s_light_rt.state == DEVICE_STATE_READY && room->illuminance_valid)
    {
        snprintf(buf, sizeof(buf), "Light: %.0f lx", (double)room->illuminance_lux);
        Display_DrawString(&s_display, 0, 16, buf);
    }
    else if (s_light_rt.state == DEVICE_STATE_READY)
    {
        Display_DrawString(&s_display, 0, 16, "Light: ---");
    }
    else if (s_light_rt.state == DEVICE_STATE_RECOVERING ||
             s_light_rt.state == DEVICE_STATE_INITIALIZING ||
             s_light_rt.state == DEVICE_STATE_PROBING)
    {
        Display_DrawString(&s_display, 0, 16, "Light: ---");
    }
    else
    {
        Display_DrawString(&s_display, 0, 16, "Light: N/A");
    }

    DriverStatus status = Display_Update(&s_display);
    if (status == DRIVER_STATUS_OK)
    {
        s_disp_rt.consecutive_errors = 0;
        DeviceRuntime_RecordSuccess(&s_disp_rt);
    }
    else
    {
        DeviceRuntime_RecordFailure(&s_disp_rt);
        if (s_disp_rt.consecutive_errors >= CONSECUTIVE_ERROR_THRESHOLD)
            s_disp_rt.state = DEVICE_STATE_ERROR;
    }
}

static void App_DoRetry(void)
{
    switch (s_light_rt.state)
    {
        case DEVICE_STATE_NOT_FOUND:
            s_light_rt.state = DEVICE_STATE_PROBING;
            break;
        case DEVICE_STATE_ERROR:
            s_light_rt.recovery_count++;
            s_light_rt.state = DEVICE_STATE_RECOVERING;
            break;
        case DEVICE_STATE_RECOVERING:
            s_light_rt.state = DEVICE_STATE_PROBING;
            break;
        default:
            break;
    }

    if (s_light_rt.state != DEVICE_STATE_READY)
        RoomState_UpdateIlluminance(&s_room, s_room.illuminance_lux, false);
    else if (s_veml.initialized == 0U)
        RoomState_UpdateIlluminance(&s_room, s_room.illuminance_lux, false);

    switch (s_disp_rt.state)
    {
        case DEVICE_STATE_NOT_FOUND:
            s_disp_rt.state = DEVICE_STATE_PROBING;
            break;
        case DEVICE_STATE_ERROR:
            s_disp_rt.recovery_count++;
            s_disp_rt.state = DEVICE_STATE_RECOVERING;
            break;
        case DEVICE_STATE_RECOVERING:
            s_disp_rt.state = DEVICE_STATE_PROBING;
            break;
        default:
            break;
    }

    if (s_light_rt.state == DEVICE_STATE_PROBING)
        App_DoProbeVeml();

    if (s_light_rt.state == DEVICE_STATE_INITIALIZING)
        App_DoInitVeml();

    if (s_disp_rt.state == DEVICE_STATE_PROBING)
        App_DoProbeDisplay();

    if (s_disp_rt.state == DEVICE_STATE_INITIALIZING)
        App_DoInitDisplay();
}

RoomSensor_Status App_Init(void)
{
    if (s_i2c_bus == NULL) return ROOM_SENSOR_ERROR;

    Config_LoadDefaults();

    s_start_ms = Platform_GetTickMs();

    DeviceRuntime_Init(&s_light_rt, DEVICE_STATE_NOT_FOUND);
    DeviceRuntime_Init(&s_disp_rt, DEVICE_STATE_NOT_FOUND);

    RoomState_Init(&s_room);

    App_DoRetry();

    return ROOM_SENSOR_OK;
}

void App_Run(void)
{
    const RoomSensorConfig *cfg = Config_Get();
    uint32_t now = Platform_GetTickMs();

    if ((now - s_last_retry_ms) >= cfg->retry_period_ms)
    {
        s_last_retry_ms = now;
        App_DoRetry();
    }

    if ((now - s_last_light_ms) >= cfg->light_period_ms)
    {
        s_last_light_ms = now;
        if (s_light_rt.state == DEVICE_STATE_READY)
            App_DoReadLight();
    }

    if ((now - s_last_display_ms) >= cfg->display_period_ms)
    {
        s_last_display_ms = now;
        if (s_disp_rt.state == DEVICE_STATE_READY)
            App_DoUpdateDisplay();
    }

    if ((now - s_last_diag_ms) >= cfg->diag_period_ms)
    {
        s_last_diag_ms = now;
        printf("APP uptime=%lu\r\n"
               "LIGHT state=%d room_lux=%.0f ops=%lu err=%lu consec=%lu rec=%lu\r\n"
               "DISPLAY state=%d ops=%lu err=%lu consec=%lu rec=%lu\r\n",
               (unsigned long)(now - s_start_ms),
               (int)s_light_rt.state,
               (double)s_room.illuminance_lux,
               (unsigned long)s_light_rt.operation_successes,
               (unsigned long)s_light_rt.operation_failures,
               (unsigned long)s_light_rt.consecutive_errors,
               (unsigned long)s_light_rt.recovery_count,
               (int)s_disp_rt.state,
               (unsigned long)s_disp_rt.operation_successes,
               (unsigned long)s_disp_rt.operation_failures,
               (unsigned long)s_disp_rt.consecutive_errors,
               (unsigned long)s_disp_rt.recovery_count);
    }
}

void App_GetStatus(AppStatus *status)
{
    if (status == NULL) return;

    status->light_sensor = s_light_rt;
    status->display = s_disp_rt;
    status->uptime_ms = Platform_GetTickMs() - s_start_ms;
}