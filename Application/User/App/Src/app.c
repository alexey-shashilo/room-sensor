#include "app.h"
#include "room_state.h"
#include "config.h"
#include "veml7700.h"
#include "display.h"
#include "platform_time.h"
#include "platform_watchdog.h"
#include "storage.h"
#include "device_identity.h"
#include "telemetry.h"
#include "boot_session.h"
#include "device_lifecycle.h"
#include "communication.h"
#include "communication_debug.h"
#include "command.h"
#include "provisioning.h"
#include <stdio.h>
#include <string.h>

#define WATCHDOG_TIMEOUT_MS 4000U

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
static uint32_t s_last_telemetry_ms = 0;
static uint32_t s_start_ms = 0;

static SystemHealthState s_health = SYSTEM_HEALTH_BOOTING;
static ResetCause        s_reset_cause = RESET_CAUSE_UNKNOWN;
static bool              s_watchdog_active = false;

static SelfTestReport    s_self_test;
static DeviceIdentity    s_device_id;
static uint64_t          s_boot_id = 0;
static bool              s_device_id_valid = false;
static bool              s_device_id_persisted = false;

/* Boot persistence status: distinguishes healthy (config/identity loaded from
   Flash), blank/first-boot (NOT_FOUND), and degraded (CORRUPT/IO_ERROR where
   runtime values are used but the persistent record is preserved untouched). */
static StorageReadStatus s_config_storage_status = STORAGE_READ_NOT_FOUND;
static StorageReadStatus s_identity_storage_status = STORAGE_READ_NOT_FOUND;

/* Boot-level Storage geometry health. A failed Storage_Init keeps the device
   sensing but disables storage-backed services. This is treated as a static
   Platform capability (Flash geometry cannot normally change at runtime), so
   it is a boot invariant — unlike Provisioning health which is runtime-current
   and derived live via Provisioning_IsHealthy(). */
static bool              s_storage_init_ok = false;

static RoomState         s_room;

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
            float calibrated = sample.lux * Config_Get()->runtime.light_calibration_factor;
            RoomState_UpdateIlluminance(&s_room, calibrated, true);
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

void App_DoRetry(void)
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

static const char *ResetCauseStr(ResetCause c)
{
    switch (c)
    {
        case RESET_CAUSE_POWER_ON:  return "POWER_ON";
        case RESET_CAUSE_SOFTWARE:  return "SOFTWARE";
        case RESET_CAUSE_WATCHDOG:  return "WATCHDOG";
        case RESET_CAUSE_BROWNOUT:  return "BROWNOUT";
        case RESET_CAUSE_EXTERNAL:  return "EXTERNAL";
        default:                    return "UNKNOWN";
    }
}

static const char *SelfTestResultStr(SelfTestResult r)
{
    switch (r)
    {
        case SELF_TEST_PASS:    return "PASS";
        case SELF_TEST_FAIL:    return "FAIL";
        case SELF_TEST_SKIPPED: return "SKIPPED";
        default:                return "NOT_RUN";
    }
}

static void App_PrintBootDiag(void)
{
    char short_id[16];
    DeviceIdentity_GetShortId(&s_device_id, short_id, sizeof(short_id));

    printf("BOOT Reset=%s\r\n", ResetCauseStr(s_reset_cause));

    printf("SELFTEST Platform=%s I2C=%s Storage=%s Config=%s ID=%s VEML=%s Disp=%s\r\n",
           SelfTestResultStr(s_self_test.platform),
           SelfTestResultStr(s_self_test.i2c),
           SelfTestResultStr(s_self_test.storage),
           SelfTestResultStr(s_self_test.config),
           SelfTestResultStr(s_self_test.identity),
           SelfTestResultStr(s_self_test.light_sensor),
           SelfTestResultStr(s_self_test.display));

    printf("CONFIG %s seq=%u calib=%.3f ID=%s\r\n",
           (s_config_storage_status == STORAGE_READ_OK) ? "persisted" : "defaults",
           (unsigned)Config_Get()->storage.version,
           (double)Config_Get()->runtime.light_calibration_factor,
           short_id);

    printf("ID valid=%d persisted=%d\r\n",
           (int)s_device_id_valid, (int)s_device_id_persisted);

    printf("TELEMETRY schema=%u period=%lu\r\n",
           (unsigned)TELEMETRY_SCHEMA_VERSION,
           (unsigned long)Config_Get()->storage.telemetry_period_ms);

    printf("WDG active=%d\r\n", (int)s_watchdog_active);
    printf("Health=%d\r\n", (int)s_health);
}

static void App_UpdateHealth(void)
{
    if (s_i2c_bus == NULL)
    {
        s_health = SYSTEM_HEALTH_FAULT;
        return;
    }

    bool veml_ready = (s_light_rt.state == DEVICE_STATE_READY);
    bool disp_ready = (s_disp_rt.state == DEVICE_STATE_READY);
    bool storage_ok = (s_self_test.storage == SELF_TEST_PASS) && s_storage_init_ok;
    bool config_ok  = (s_self_test.config == SELF_TEST_PASS);

    /* Provisioning health is runtime-CURRENT (not a boot snapshot): after an
       explicit recovery it returns to OK without a reboot. */
    if (veml_ready && disp_ready && storage_ok && config_ok &&
        Provisioning_IsHealthy())
        s_health = SYSTEM_HEALTH_OK;
    else
        s_health = SYSTEM_HEALTH_DEGRADED;
}

RoomSensor_Status App_Init(void)
{
    if (s_i2c_bus == NULL) return ROOM_SENSOR_ERROR;

    s_reset_cause = Platform_GetResetCause();
    Platform_ClearResetFlags();

    s_start_ms = Platform_GetTickMs();

    DeviceRuntime_Init(&s_light_rt, DEVICE_STATE_NOT_FOUND);
    DeviceRuntime_Init(&s_disp_rt, DEVICE_STATE_NOT_FOUND);

    RoomState_Init(&s_room);
    SelfTest_Init(&s_self_test);
    s_storage_init_ok = Storage_Init();
    Provisioning_Init();   /* runtime provisioning state queried live via API */

    DeviceLifecycle_Init(LIFECYCLE_BOOT);

    Communication_Init();
    {
        static CommunicationPort s_debug_port;
        CommunicationDebug_Init(&s_debug_port);
        Communication_SetPort(&s_debug_port);

        CommandServices cmd_svc;
        memset(&cmd_svc, 0, sizeof(cmd_svc));
        cmd_svc.room = RoomState_Get(&s_room);
        cmd_svc.config = Config_Get();
        cmd_svc.identity = &s_device_id;
        cmd_svc.self_test = &s_self_test;
        cmd_svc.bus = (struct I2cBus *)s_i2c_bus;
        cmd_svc.uptime_ms = 0;
        cmd_svc.watchdog_active = false;
        Command_Init(&cmd_svc);
    }

    /* Return ROOM_SENSOR_OK because the runtime started and can sense, even if
       Storage_Init() degraded (boot-level) or provisioning is currently in a
       CORRUPT/IO_ERROR state (recoverable at runtime). Health is exposed via
       AppStatus and GET_STATUS; provisioning health is read live via
       Provisioning_IsHealthy() so it recovers after an explicit recovery
       without requiring a reboot. Persistent writes fail closed at the storage
       layer. A failure of a boot-critical subsystem (I2C) returns ERROR above. */
    return ROOM_SENSOR_OK;
}

void App_Run(void)
{
    uint32_t now = Platform_GetTickMs();

    Command_UpdateRuntime(now - s_start_ms, s_watchdog_active, &s_light_rt, &s_disp_rt, s_reset_cause);

    /* ================================================================
       Device Lifecycle State Machine
       ================================================================ */
    {
        LifecycleState lc = DeviceLifecycle_GetState();

        switch (lc)
        {
            case LIFECYCLE_BOOT:
                DeviceLifecycle_TransitionTo(LIFECYCLE_LOAD_CONFIGURATION);
                break;

            case LIFECYCLE_LOAD_CONFIGURATION:
                if (!Config_Load())
                    Config_LoadDefaults();
                s_config_storage_status = Config_GetStorageStatus();
                DeviceLifecycle_TransitionTo(LIFECYCLE_LOAD_IDENTITY);
                break;

            case LIFECYCLE_LOAD_IDENTITY:
            {
                if (DeviceIdentity_Load(&s_device_id))
                {
                    s_device_id_valid = true;
                    s_device_id_persisted = true;
                }
                else
                {
                    DeviceIdentity derived;
                    if (DeviceIdentity_Derive(&derived))
                    {
                        s_device_id = derived;
                        s_device_id_valid = true;
                        /* Persist a derived identity ONLY on a genuine first
                           boot (NOT_FOUND). A CORRUPT or IO_ERROR persistent
                           record is preserved untouched for diagnostics/
                           recovery — but the runtime identity stays usable. */
                        if (DeviceIdentity_GetLoadStatus() == STORAGE_READ_NOT_FOUND &&
                            DeviceIdentity_Save(&derived))
                            s_device_id_persisted = true;
                    }
                }
                s_identity_storage_status = DeviceIdentity_GetLoadStatus();
                DeviceLifecycle_TransitionTo(LIFECYCLE_CREATE_BOOT_SESSION);
                break;
            }

            case LIFECYCLE_CREATE_BOOT_SESSION:
            {
                BootSession bs;
                if (BootSession_Get(&bs))
                    s_boot_id = bs.boot_id;
                /* Persist config defaults ONLY for a genuine first boot
                   (NOT_FOUND). CORRUPT / IO_ERROR config is preserved untouched
                   (evidence kept for diagnostics/recovery). */
                if (s_config_storage_status == STORAGE_READ_NOT_FOUND)
                    Config_Save();
                DeviceLifecycle_TransitionTo(LIFECYCLE_SELF_TEST);
                break;
            }

            case LIFECYCLE_SELF_TEST:
                SelfTest_Run(&s_self_test, s_i2c_bus);
                DeviceLifecycle_TransitionTo(LIFECYCLE_PROBE_PERIPHERALS);
                break;

            case LIFECYCLE_PROBE_PERIPHERALS:
                App_DoRetry();
                s_watchdog_active = Platform_WatchdogInit(WATCHDOG_TIMEOUT_MS);
                DeviceLifecycle_TransitionTo(LIFECYCLE_INITIALIZE_DRIVERS);
                break;

            case LIFECYCLE_INITIALIZE_DRIVERS:
                App_DoRetry();
                DeviceLifecycle_TransitionTo(LIFECYCLE_READY);
                break;

            case LIFECYCLE_READY:
                App_UpdateHealth();
                App_PrintBootDiag();
                DeviceLifecycle_TransitionTo(LIFECYCLE_OPERATIONAL);
                break;

            case LIFECYCLE_OPERATIONAL:
            case LIFECYCLE_DEGRADED:
                /* Fall through to scheduler below */
                break;

            default:
                break;
        }

        if (!DeviceLifecycle_IsOperational())
            return;
    }

    /* ================================================================
       Cooperative Scheduler (only when OPERATIONAL or DEGRADED)
       ================================================================ */
    const RoomSensorConfig *cfg = Config_Get();

    if ((now - s_last_retry_ms) >= cfg->storage.retry_period_ms)
    {
        s_last_retry_ms = now;
        App_DoRetry();
    }

    if ((now - s_last_light_ms) >= cfg->storage.light_period_ms)
    {
        s_last_light_ms = now;
        if (s_light_rt.state == DEVICE_STATE_READY)
            App_DoReadLight();
    }

    if ((now - s_last_display_ms) >= cfg->storage.display_period_ms)
    {
        s_last_display_ms = now;
        if (s_disp_rt.state == DEVICE_STATE_READY)
            App_DoUpdateDisplay();
    }

    if ((now - s_last_diag_ms) >= cfg->storage.diagnostics_period_ms)
    {
        s_last_diag_ms = now;
        App_UpdateHealth();

        CommunicationRuntime cr;
        Communication_GetRuntime(&cr);

        printf("APP uptime=%lu\r\n"
               "LIGHT state=%d room_lux=%.0f ops=%lu err=%lu consec=%lu rec=%lu\r\n"
               "DISPLAY state=%d ops=%lu err=%lu consec=%lu rec=%lu\r\n"
               "HEALTH=%d WDG=%d CFG=%s COMM=%d sent=%lu failed=%lu\r\n",
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
               (unsigned long)s_disp_rt.recovery_count,
               (int)s_health, (int)s_watchdog_active,
               (s_config_storage_status == STORAGE_READ_OK) ? "persisted" : "defaults",
               (int)cr.state,
               (unsigned long)cr.send_successes,
               (unsigned long)cr.send_failures);
    }

    if ((now - s_last_telemetry_ms) >= cfg->storage.telemetry_period_ms)
    {
        s_last_telemetry_ms = now;

        if (s_device_id_valid)
        {
            TelemetrySnapshotInput input;
            input.device_id = s_device_id.device_uuid;
            input.boot_id = s_boot_id;
            input.room = RoomState_Get(&s_room);
            input.health = s_health;
            input.uptime_ms = now;

            TelemetrySnapshot snap;
            if (Telemetry_CreateSnapshot(&snap, &input))
                Communication_SubmitSnapshot(&snap);
        }
    }

    Communication_Run();
    Command_Run();

    Platform_WatchdogRefresh();
}

void App_GetStatus(AppStatus *status)
{
    if (status == NULL) return;

    status->light_sensor = s_light_rt;
    status->display = s_disp_rt;
    status->health = s_health;
    status->reset_cause = s_reset_cause;
    status->watchdog_active = s_watchdog_active;
    status->self_test = s_self_test;
    status->storage_initialized = s_storage_init_ok;
    status->provisioning_initialized = Provisioning_IsHealthy();
    status->config_storage_status = s_config_storage_status;
    status->identity_storage_status = s_identity_storage_status;
    status->uptime_ms = Platform_GetTickMs() - s_start_ms;
}