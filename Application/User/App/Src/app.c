#include "app.h"
#include "room_state.h"
#include "config.h"
#include "veml7700.h"
#include "display.h"
#include "display_pages.h"
#include "scd41.h"
#include "scd41_runtime.h"
#include "sht45.h"
#include "sht45_runtime.h"
#include "bmp390.h"
#include "bmp390_runtime.h"
#include "bmp380.h"
#include "bmp380_runtime.h"
#include "sgp41.h"
#include "sgp41_runtime.h"
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
#include "recovery_policy.h"
#include "i2c_bus_health.h"
#include <stdio.h>
#include <string.h>

#define WATCHDOG_TIMEOUT_MS 4000U

static VEML7700_HandleTypeDef s_veml;
static Display_HandleTypeDef  s_display;
static Scd41Runtime           s_scd41;
static Sht45Runtime           s_sht45;
static Bmp390Runtime          s_bmp390;
static Bmp380Runtime          s_bmp380;
static Sgp41Runtime           s_sgp41;

static const I2cBus *s_i2c_bus = NULL;

static DeviceRuntime s_light_rt = { .state = DEVICE_STATE_UNKNOWN };
static DeviceRuntime s_disp_rt  = { .state = DEVICE_STATE_UNKNOWN };
static DeviceRuntime s_co2_rt   = { .state = DEVICE_STATE_UNKNOWN };
static DeviceRuntime s_temp_rt  = { .state = DEVICE_STATE_UNKNOWN };   /* SHT45 */
static DeviceRuntime s_pres_rt  = { .state = DEVICE_STATE_UNKNOWN };   /* active barometer */
static DeviceRuntime s_bmp390_rt = { .state = DEVICE_STATE_UNKNOWN };  /* BMP390 diag */
static DeviceRuntime s_pres_rt_bmp380 = { .state = DEVICE_STATE_UNKNOWN }; /* BMP380 diag */
static DeviceRuntime s_gas_rt   = { .state = DEVICE_STATE_UNKNOWN };   /* SGP41 */

static uint8_t s_display_addr = 0U;
static bool    s_display_addr_valid = false;

static uint32_t s_last_light_ms = 0;
static uint32_t s_last_display_ms = 0;
static uint32_t s_last_retry_ms = 0;
static uint32_t s_last_diag_ms = 0;
static uint32_t s_last_telemetry_ms = 0;
static uint32_t s_last_scd41_ms = 0;
static uint32_t s_last_sht45_ms = 0;
static uint32_t s_last_bmp390_ms = 0;
static uint32_t s_last_bmp380_ms = 0;
static uint32_t s_last_sgp41_ms = 0;
static uint32_t s_start_ms = 0;
/* Display page alternation (Phase 17.6): last tick a page switch was applied
   and the currently active page (DISPLAY_PAGE_ENV or DISPLAY_PAGE_AIR_QUALITY).
   Advanced every DISPLAY_PAGE_PERIOD_MS with wrap-safe modular timing. */
static uint32_t s_display_page_switch_ms = 0U;
static uint8_t  s_display_page = DISPLAY_PAGE_ENV;
/* Commit tracker for BMP390 (see s_sht45_room_commit_ms). */
static uint32_t s_bmp390_room_commit_ms = 0xFFFFFFFFu;
/* Tick timestamp of the last SHT45 sample actually committed into RoomState, so
   App commits a NEW sample only when the runtime accepted one (avoiding both
   validity flicker and needless RoomState timestamp churn on every poll).
   Initialized to a sentinel so the first accepted sample always commits. */
static uint32_t s_sht45_room_commit_ms = 0xFFFFFFFFu;

/* Shared-bus health monitor (Phase 5/7). A single device failure never triggers
   shared-bus recovery; only >= 2 distinct previously-healthy devices reporting
   transport failures within the window (or a persistent bus-busy state) does. */
static I2cBusHealth s_bus_health;

/* Per-device NOT_FOUND backoff gating (Phase 4). Each present-but-possibly-
   absent device tracks its own consecutive-absence count and next-probe time so
   a physically absent optional sensor is not probed on every retry tick. */
static uint32_t s_veml_absent = 0U;
static uint32_t s_veml_next_probe_ms = 0U;
static uint32_t s_disp_absent = 0U;
static uint32_t s_disp_next_probe_ms = 0U;

static SystemHealthState s_health = SYSTEM_HEALTH_BOOTING;
static ResetCause        s_reset_cause = RESET_CAUSE_UNKNOWN;
static bool              s_watchdog_active = false;

static SelfTestReport    s_self_test;
static DeviceIdentity    s_device_id;
static uint64_t          s_boot_id = 0;
static bool              s_device_id_valid = false;

/* Boot-LOAD snapshot for config/identity. NOT_FOUND = first boot (defaults or
   derived identity used and persisted once); CORRUPT/IO_ERROR = runtime values
   used but the persistent record is preserved untouched (degraded). These are
   HISTORICAL boot-load snapshots that drive the first-boot persistence policy
   only. The CURRENT runtime persistence state is always queried live from each
   module (Config_GetStorageStatus / DeviceIdentity_GetPersistenceStatus) at
   status-report time and for runtime diagnostics, so it is never stale. */
static StorageReadStatus s_config_boot_load_status = STORAGE_READ_NOT_FOUND;
static StorageReadStatus s_identity_boot_load_status = STORAGE_READ_NOT_FOUND;

/* Boot-level Storage geometry health. A failed Storage_Init keeps the device
   sensing but disables storage-backed services. This is treated as a static
   Platform capability (Flash geometry cannot normally change at runtime), so
   it is a boot invariant — unlike Provisioning health which is runtime-current
   and derived live via Provisioning_IsHealthy(). */
static bool              s_storage_init_ok = false;

static RoomState         s_room;

/* Live AppStatus snapshot exposed via App_GetStatus(). */
static AppStatus         s_status;

/* Command-facing runtime-status DTO. App (the owner) fills it from the
   authoritative modules before Command_Run(); Command only reads the snapshot
   and has no dependency on App concrete types. */
static CommandRuntimeStatus s_cmd_status;

static void App_FillCommandRuntimeStatus(void);

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
    uint32_t now = Platform_GetTickMs();
    /* Phase 4 backoff: a physically absent VEML is not probed every retry tick.
       After a probe-and-fail, gate future probes behind the per-device ladder. */
    if (s_veml_absent > 0U)
    {
        if (RecoveryPolicy_Elapsed(now, s_veml_next_probe_ms, 0U) == false ||
            (uint32_t)(now - s_veml_next_probe_ms) >= 0x80000000U)
        {
            s_light_rt.state = DEVICE_STATE_NOT_FOUND;
            return;
        }
    }

    if (VEML7700_Probe(s_i2c_bus))
    {
        s_veml_absent = 0U;
        s_light_rt.state = DEVICE_STATE_INITIALIZING;
    }
    else
    {
        s_veml_absent = RecoveryPolicy_TrackAbsence(s_veml_absent, true);
        s_veml_next_probe_ms = now + RecoveryPolicy_BackoffMs(s_veml_absent);
        s_light_rt.state = DEVICE_STATE_NOT_FOUND;
    }
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

/* Synchronize the portable SCD41 diagnostic snapshot (s_co2_rt) from the
   SCD41 runtime so GET_STATUS / AppStatus expose live CO2 sensor state. */
static void App_RefreshScd41Diagnostics(void)
{
    Scd41Runtime_GetDiagnostics(&s_scd41, &s_co2_rt);
    /* Health semantics mirror the runtime's error/recovery flow. */
}

/* Start SCD41 periodic measurement (first boot or bounded recovery). */
static bool App_DoStartScd41(void)
{
    DriverStatus s = Scd41Runtime_Start(&s_scd41);
    if (s == DRIVER_STATUS_OK)
        return true;
    App_RefreshScd41Diagnostics();
    return false;
}

/* Advance the SCD41 non-blocking state machine and reflect any accepted /
   invalidated sample into RoomState. */
static void App_DoPollScd41(void)
{
    bool had_valid = Scd41Runtime_HasValidSample(&s_scd41);
    Scd41Runtime_Poll(&s_scd41);
    App_RefreshScd41Diagnostics();

    bool has_valid = Scd41Runtime_HasValidSample(&s_scd41);
    if (has_valid)
    {
        /* Commit the fully-valid sample into RoomState. */
        const Scd41Measurement *m = &s_scd41.last_sample;
        RoomState_UpdateScd41(&s_room,
                              (float)m->co2_ppm, true,
                              m->temperature_c, true,
                              m->relative_humidity_pct, true);
    }
    else if (had_valid)
    {
        /* A previously-valid value went stale or the sensor was lost.
           Preserve numeric last-good values but clear validity. */
        RoomState_InvalidateScd41(&s_room);
    }

    if (s_scd41.state == DEVICE_STATE_ERROR && had_valid)
    {
        /* Sensor disappeared after working: invalidate CO2 now (freshness
           enforcement / explicit loss) — the runtime already invalidated via
           stale, but ensure RoomState follows. */
        RoomState_InvalidateScd41(&s_room);
    }
}

/* Synchronize the portable SHT45 diagnostic snapshot (s_temp_rt) from the SHT45
   runtime so GET_STATUS / AppStatus expose live temperature/humidity state. */
static void App_RefreshSht45Diagnostics(void)
{
    Sht45Runtime_GetDiagnostics(&s_sht45, &s_temp_rt);
}

/* Start SHT45 single-shot measurement (first boot or bounded recovery). */
static bool App_DoStartSht45(void)
{
    DriverStatus s = Sht45Runtime_Start(&s_sht45);
    if (s == DRIVER_STATUS_OK)
        return true;
    App_RefreshSht45Diagnostics();
    return false;
}

/* Advance the SHT45 non-blocking state machine and reflect any accepted /
   invalidated sample into RoomState. */
static void App_DoPollSht45(void)
{
    bool had_valid = Sht45Runtime_HasValidSample(&s_sht45);
    Sht45Runtime_Poll(&s_sht45);
    App_RefreshSht45Diagnostics();

    bool has_valid = Sht45Runtime_HasValidSample(&s_sht45);

    if (has_valid)
    {
        /* Commit a genuinely NEW sample (last_valid_measurement_ms advanced past
           the last RoomState commit). During a normal in-flight conversion the
           last-good sample stays valid and is NOT re-rendered as invalid, so
           RoomState retains the last-good value without a valid->invalid->valid
           toggle. */
        if (s_sht45.last_valid_measurement_ms != s_sht45_room_commit_ms)
        {
            const Sht45Measurement *m = &s_sht45.last_sample;
            RoomState_UpdateSht45(&s_room,
                                  m->temperature_c, true,
                                  m->relative_humidity_pct, true);
            s_sht45_room_commit_ms = s_sht45.last_valid_measurement_ms;
        }
    }
    else if (had_valid)
    {
        /* The last-good sample went stale or was invalidated (durable error /
           confirmed missing). Clear RoomState validity. */
        RoomState_InvalidateSht45(&s_room);
    }

    /* Durable error: invalidate RoomState regardless of the had/has compare. */
    if (s_sht45.state == DEVICE_STATE_ERROR && had_valid)
        RoomState_InvalidateSht45(&s_room);
}

/* Sync the portable BMP390 diagnostic snapshot from the runtime. */
static void App_RefreshBmp390Diagnostics(void)
{
    Bmp390Runtime_GetDiagnostics(&s_bmp390, &s_bmp390_rt);
}

/* Start BMP390 (identity detect + configure + first forced measurement). */
static bool App_DoStartBmp390(void)
{
    DriverStatus s = Bmp390Runtime_Start(&s_bmp390);
    if (s == DRIVER_STATUS_OK)
        return true;
    App_RefreshBmp390Diagnostics();
    return false;
}

/* Advance the BMP390 non-blocking state machine and reflect accepted /
   invalidated pressure into RoomState. */
static void App_DoPollBmp390(void)
{
    bool had_valid = Bmp390Runtime_HasValidSample(&s_bmp390);
    Bmp390Runtime_Poll(&s_bmp390);
    App_RefreshBmp390Diagnostics();

    bool has_valid = Bmp390Runtime_HasValidSample(&s_bmp390);

    if (has_valid)
    {
        if (s_bmp390.last_valid_measurement_ms != s_bmp390_room_commit_ms)
        {
            const Bmp390Sample *m = &s_bmp390.last_sample;
            RoomState_UpdateBmp390(&s_room,
                                   m->pressure_pa, true,
                                   m->temperature_c, true);
            s_bmp390_room_commit_ms = s_bmp390.last_valid_measurement_ms;
        }
    }
    else if (had_valid)
    {
        RoomState_InvalidateBmp390(&s_room);
    }

    if (s_bmp390.state == DEVICE_STATE_ERROR && had_valid)
        RoomState_InvalidateBmp390(&s_room);
}

/* ------------------------------------------------------------------ */
/* BMP380 runtime (Phase 17.7B) — mirrors BMP390 lifecycle + quality.  */
/* ------------------------------------------------------------------ */

static void App_RefreshBmp380Diagnostics(void)
{
    Bmp380Runtime_GetDiagnostics(&s_bmp380, &s_pres_rt_bmp380);
}

/* Start BMP380 (identity detect + configure + first forced measurement). */
static bool App_DoStartBmp380(void)
{
    DriverStatus s = Bmp380Runtime_Start(&s_bmp380);
    if (s == DRIVER_STATUS_OK)
        return true;
    App_RefreshBmp380Diagnostics();
    return false;
}

/* Advance the BMP380 non-blocking state machine. No direct RoomState commit here;
   the active provider is selected in App_CommitBarometricRoomState. */
static void App_DoPollBmp380(void)
{
    Bmp380Runtime_Poll(&s_bmp380);
    App_RefreshBmp380Diagnostics();
}

/* ------------------------------------------------------------------ */
/* Barometric provider selection (Phase 17.7B).                        */
/*                                                                    */
/* Deterministic, exactly ONE active provider:                        */
/*   BMP390 fresh-valid  -> provider BMP390                           */
/*   else BMP380 fresh-valid -> provider BMP380                       */
/*   else provider NONE                                               */
/* Selection consumes PRODUCTION runtime validity (fresh last-good),   */
/* not raw device ACK, so a single transient operation does not        */
/* oscillate the provider while the prior provider still holds a       */
/* fresh valid last-good sample.                                      */
/*                                                                    */
/* RoomState is updated ATOMICALLY (provider + pressure + temperature + */
/* validity as one coherent snapshot) via RoomState_UpdateBarometric.  */
/* ------------------------------------------------------------------ */
static void App_CommitBarometricRoomState(void)
{
    /* BMP390 is preferred when it holds a fresh valid last-good sample. */
    if (Bmp390Runtime_HasValidSample(&s_bmp390))
    {
        const Bmp390Sample *m = &s_bmp390.last_sample;
        RoomState_UpdateBarometric(&s_room,
                                   BAROMETER_PROVIDER_BMP390,
                                   m->pressure_pa, true,
                                   m->temperature_c, true);
        s_pres_rt = s_bmp390_rt;
        return;
    }

    /* Else BMP380 when it holds a fresh valid last-good sample. */
    if (Bmp380Runtime_HasValidSample(&s_bmp380))
    {
        const Bmp380Sample *m = &s_bmp380.last_sample;
        RoomState_UpdateBarometric(&s_room,
                                   BAROMETER_PROVIDER_BMP380,
                                   m->pressure_pa, true,
                                   m->temperature_c, true);
        s_pres_rt = s_pres_rt_bmp380;
        return;
    }

    /* Neither provider fresh-valid: NONE; no pressure is fabricated. */
    RoomState_InvalidateBarometric(&s_room);
    s_pres_rt = s_pres_rt_bmp380.state != DEVICE_STATE_UNKNOWN ? s_pres_rt_bmp380 : s_bmp390_rt;
}

/* Sync the portable SGP41 diagnostic snapshot from the runtime. */
static void App_RefreshSgp41Diagnostics(void)
{
    Sgp41Runtime_GetDiagnostics(&s_sgp41, &s_gas_rt);
}

/* Start SGP41 (probe + conditioning + measurement). */
static bool App_DoStartSgp41(void)
{
    DriverStatus s = Sgp41Runtime_Start(&s_sgp41);
    if (s == DRIVER_STATUS_OK)
        return true;
    App_RefreshSgp41Diagnostics();
    return false;
}

/* Advance the SGP41 non-blocking state machine, supplying compensation from the
   fresh SHT45 reading when available (else the SGP41 defaults), and reflect any
   accepted/invalidated sample into RoomState. */
static void App_DoPollSgp41(void)
{
    bool had_valid = Sgp41Runtime_HasValidSample(&s_sgp41);

    /* Compensation: use a fresh, valid SHT45 T/RH when available; otherwise the
       SGP41 documented defaults (SHT45 absent/stale must NOT make SGP41 error).
       SGP41 therefore needs no remaining->no dependency on RoomState here: the
       runtime consumes it via the explicit SetCompensation interface. */
    Sgp41Compensation comp;
    memset(&comp, 0, sizeof(comp));
    comp.valid = false;
    if (s_sht45.state == DEVICE_STATE_READY &&
        s_room.sht45_temperature_valid && s_room.sht45_humidity_valid)
    {
        comp.valid = true;
        comp.temperature_c = s_room.sht45_temperature_c;
        comp.relative_humidity_pct = s_room.sht45_humidity_pct;
    }
    Sgp41Runtime_SetCompensation(&s_sgp41, &comp);

    Sgp41Runtime_Poll(&s_sgp41);
    App_RefreshSgp41Diagnostics();

    bool has_raw = Sgp41Runtime_HasValidSample(&s_sgp41);
    bool has_voc = Sgp41Runtime_HasValidVocIndex(&s_sgp41);
    bool has_nox = Sgp41Runtime_HasValidNoxIndex(&s_sgp41);

    if (has_raw)
    {
        const Sgp41RawMeasurement *m = &s_sgp41.last_sample;
        RoomState_UpdateSgp41(&s_room,
                              (float)m->raw_voc, true,
                              (float)m->raw_nox, true,
                              (float)s_sgp41.voc_index, has_voc,
                              (float)s_sgp41.nox_index, has_nox);
    }
    else if (had_valid)
    {
        RoomState_InvalidateSgp41(&s_room);
    }

    if (s_sgp41.state == DEVICE_STATE_ERROR && had_valid)
        RoomState_InvalidateSgp41(&s_room);
}

static void App_DoProbeDisplay(void)
{
    uint32_t now = Platform_GetTickMs();
    /* Phase 4 backoff for a physically absent display. */
    if (s_disp_absent > 0U)
    {
        if (RecoveryPolicy_Elapsed(now, s_disp_next_probe_ms, 0U) == false ||
            (uint32_t)(now - s_disp_next_probe_ms) >= 0x80000000U)
        {
            s_disp_rt.state = DEVICE_STATE_NOT_FOUND;
            return;
        }
    }

    uint8_t addr;
    if (Display_Probe(s_i2c_bus, &addr))
    {
        s_disp_absent = 0U;
        s_display_addr = addr;
        s_display_addr_valid = true;
        s_disp_rt.state = DEVICE_STATE_INITIALIZING;
    }
    else
    {
        s_disp_absent = RecoveryPolicy_TrackAbsence(s_disp_absent, true);
        s_disp_next_probe_ms = now + RecoveryPolicy_BackoffMs(s_disp_absent);
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

/* PAGE 1: existing environmental page (Room Sensor, CO2, RH/T, Light). */
static void App_RenderDisplayPageEnv(void)
{
    char buf[22];
    const RoomState *room = RoomState_Get(&s_room);

    Display_Clear(&s_display);
    Display_DrawString(&s_display, 0, 0, "Room Sensor");

    /* CO2 line. When invalid (startup / missing / stale) render "-- ppm", never
       a numeric 0 — "not measured" must not look like 0 ppm. */
    if (s_co2_rt.state == DEVICE_STATE_READY && room->co2_valid)
    {
        snprintf(buf, sizeof(buf), "CO2: %lu ppm", (unsigned long)room->co2_ppm);
        Display_DrawString(&s_display, 0, 16, buf);
    }
    else
    {
        Display_DrawString(&s_display, 0, 16, "CO2: -- ppm");
    }

    /* SHT45 is the preferred environmental T/RH source; fall back to SCD41
       local/internal T/RH only when SHT45 is invalid. Both stay separately
       observable in telemetry. */
    double disp_t = 0.0, disp_rh = 0.0;
    bool disp_valid = false;
    if (room->sht45_temperature_valid && room->sht45_humidity_valid)
    {
        disp_t = (double)room->sht45_temperature_c;
        disp_rh = (double)room->sht45_humidity_pct;
        disp_valid = true;
    }
    else if (room->scd41_temperature_valid && room->scd41_humidity_valid)
    {
        disp_t = (double)room->scd41_temperature_c;
        disp_rh = (double)room->scd41_humidity_pct;
        disp_valid = true;
    }

    if (disp_valid)
    {
        snprintf(buf, sizeof(buf), "RH: %.0f%%  T: %.1f", disp_rh, disp_t);
        Display_DrawString(&s_display, 0, 32, buf);
    }
    else
    {
        Display_DrawString(&s_display, 0, 32, "RH: --  T: --");
    }

    if (s_light_rt.state == DEVICE_STATE_READY && room->illuminance_valid)
    {
        snprintf(buf, sizeof(buf), "Light: %.0f lx", (double)room->illuminance_lux);
        Display_DrawString(&s_display, 0, 48, buf);
    }
    else if (s_light_rt.state == DEVICE_STATE_READY)
    {
        Display_DrawString(&s_display, 0, 48, "Light: ---");
    }
    else if (s_light_rt.state == DEVICE_STATE_RECOVERING ||
             s_light_rt.state == DEVICE_STATE_INITIALIZING ||
             s_light_rt.state == DEVICE_STATE_PROBING)
    {
        Display_DrawString(&s_display, 0, 48, "Light: ---");
    }
    else
    {
        Display_DrawString(&s_display, 0, 48, "Light: N/A");
    }
}

/* Render one gas-index line (VOC/NOx) from the existing production validity
   flags. Invalid indices never render a numeric value; the display contract is
   enforced via DisplayPages_GasState/FormatGasLine. */
static void App_RenderGasLine(const char *label, bool index_valid, bool raw_valid,
                              int32_t value, uint8_t line_y)
{
    char buf[22];
    DisplayGasState st = DisplayPages_GasState(index_valid, raw_valid);
    DisplayPages_FormatGasLine(buf, sizeof(buf), label, value, st);
    Display_DrawString(&s_display, 0, line_y, buf);
}

/* PAGE 2: SGP41 VOC/NOx air-quality page. Uses only production RoomState values;
   never talks to SGP41 over I2C and never computes a gas index itself. */
static void App_RenderDisplayPageAirQuality(void)
{
    const RoomState *room = RoomState_Get(&s_room);

    Display_Clear(&s_display);
    Display_DrawString(&s_display, 0, 0, "Air Quality");

    App_RenderGasLine("VOC", room->voc_index_valid, room->voc_raw_valid,
                      (int32_t)room->voc_index, 16);
    App_RenderGasLine("NOx", room->nox_index_valid, room->nox_raw_valid,
                      (int32_t)room->nox_index, 32);
}

static void App_DoUpdateDisplay(void)
{
    s_display_page = DisplayPages_Advance(Platform_GetTickMs(),
                                          &s_display_page_switch_ms,
                                          s_display_page);

    if (s_display_page == DISPLAY_PAGE_AIR_QUALITY)
        App_RenderDisplayPageAirQuality();
    else
        App_RenderDisplayPageEnv();

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
    uint32_t now = Platform_GetTickMs();

    /* --- Shared-bus health reporting (Phase 5) --- */
    /* Each previously-established-healthy I2C device reports its latest
       transport outcome to the bus monitor. The monitor's previously_healthy
       latch is MONOTONIC within a boot (P1-2) so a device that was READY and
       has since gone NOT_FOUND still retains its healthy history and may
       contribute transport evidence while it was a genuine bus participant.

       PARTICIPANT SET (P1-2B): only the SCD41 / SHT45 / BMP390 runtimes
       participate, because they (and only they) surface a real DriverStatus
       last-error from their I2C transactions. VEML7700 and the SSD1306 display
       drivers return bool (success/fail) and do NOT retain a portable
       DriverStatus, so a BUS_ERROR/TIMEOUT from them cannot be distinguished
       from a device-local or config failure. Per the audit, they are therefore
       EXCLUDED from shared-bus evidence rather than reporting a fabricated
       BUS_ERROR derived from DEVICE_STATE_ERROR. This reduces the participant
       set to three runtimes, which still meet the >=2-distinct-healthy-device
       evidence threshold. Their device-local errors still trigger per-sensor
       recovery at their own level. */
    {
        /* Establish healthy history from actual READY evidence (monotonic). */
        I2cBusHealth_MarkHealth(&s_bus_health, 2U, Scd41Runtime_HasValidSample(&s_scd41));
        I2cBusHealth_MarkHealth(&s_bus_health, 3U, Sht45Runtime_HasValidSample(&s_sht45));
        I2cBusHealth_MarkHealth(&s_bus_health, 4U, Bmp390Runtime_HasValidSample(&s_bmp390));
        I2cBusHealth_MarkHealth(&s_bus_health, 5U, Sgp41Runtime_HasValidSample(&s_sgp41));
        /* BMP380 (Phase 17.7B) participates under the SAME rules: MarkHealth only
           records "was previously healthy" evidence; it never marks an absent
           sensor healthy. A lone BMP380 failure alone cannot reach the shared-bus
           recovery criterion (>= 2 distinct previously-healthy devices). */
        I2cBusHealth_MarkHealth(&s_bus_health, 6U, Bmp380Runtime_HasValidSample(&s_bmp380));

        DriverStatus e2 = Scd41Runtime_LastError(&s_scd41);
        DriverStatus e3 = Sht45Runtime_LastError(&s_sht45);
        DriverStatus e4 = Bmp390Runtime_LastError(&s_bmp390);
        DriverStatus e5 = Sgp41Runtime_LastError(&s_sgp41);
        DriverStatus e6 = Bmp380Runtime_LastError(&s_bmp380);

        I2cBusHealth_Report(&s_bus_health, 2U, e2, now);
        I2cBusHealth_Report(&s_bus_health, 3U, e3, now);
        I2cBusHealth_Report(&s_bus_health, 4U, e4, now);
        I2cBusHealth_Report(&s_bus_health, 5U, e5, now);
        I2cBusHealth_Report(&s_bus_health, 6U, e6, now);
    }

    /* --- Shared-bus recovery orchestration (Phase 5/6/7) --- */
    if (I2cBusHealth_ShouldRecover(&s_bus_health) &&
        I2cBusHealth_RecoveryEligible(&s_bus_health, now) && s_i2c_bus != NULL)
    {
        /* Begin the attempt FIRST: this clears the evidence epoch (so old
           evidence can never survive) and records the cooldown base — whether
           the attempt then succeeds or fails. */
        I2cBusHealth_BeginRecovery(&s_bus_health, now);
        DriverStatus rs = I2cBus_Recover(s_i2c_bus);
        if (rs == DRIVER_STATUS_OK)
        {
            I2cBusHealth_OnRecoverySuccess(&s_bus_health);
            /* Bus recovered: force controlled reinitialization of every
               previously-present I2C runtime. Sensors are NOT marked valid;
               each must obtain a fresh sample. */
            s_veml_absent = 0U;
            s_disp_absent = 0U;
            s_veml.initialized = 0U;
            s_display.initialized = 0U;
            s_display_addr_valid = false;
            /* Reset runtimes to re-probe/re-init so they recover independently. */
            s_light_rt.state = DEVICE_STATE_NOT_FOUND;
            s_disp_rt.state = DEVICE_STATE_NOT_FOUND;
            Scd41Runtime_Recover(&s_scd41);
            Sht45Runtime_Recover(&s_sht45);
            Bmp390Runtime_Recover(&s_bmp390);
            Sgp41Runtime_Recover(&s_sgp41);
            Bmp380Runtime_Recover(&s_bmp380);
            /* Consistent last-good policy after a bus reset (Phase 13): the
               communication substrate was reset. All I2C sensor samples are
               invalidated by their runtimes through the Recover() path above
               (each Recover clears the last sample's validity); a fresh sample
               is required before any is trusted again. */
        }
        else
        {
            I2cBusHealth_OnRecoveryFailure(&s_bus_health);
            /* Recovery failed: the cooldown (RECOVERY_BUS_COOLDOWN_MS) now gates
               the next attempt; the caller owns scheduling so there is no tight
               loop and the watchdog path stays clear. */
        }
        App_RefreshScd41Diagnostics();
        App_RefreshSht45Diagnostics();
        App_RefreshBmp390Diagnostics();
        App_RefreshSgp41Diagnostics();
        App_RefreshBmp380Diagnostics();
    }

    switch (s_light_rt.state)
    {
        case DEVICE_STATE_NOT_FOUND:
        case DEVICE_STATE_RECOVERING:
            s_light_rt.state = DEVICE_STATE_PROBING;
            break;
        case DEVICE_STATE_ERROR:
            s_light_rt.recovery_count++;
            s_light_rt.state = DEVICE_STATE_RECOVERING;
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

    /* --- SCD41 periodic runtime (bounded recovery) --- */
    switch (s_scd41.state)
    {
        case DEVICE_STATE_NOT_FOUND:
        case DEVICE_STATE_RECOVERING:
            /* Re-probe + restart periodic measurement. */
            App_DoStartScd41();
            break;

        case DEVICE_STATE_ERROR:
            Scd41Runtime_Recover(&s_scd41);
            App_RefreshScd41Diagnostics();
            RoomState_InvalidateScd41(&s_room);
            break;

        default:
            break;
    }

    /* --- SHT45 single-shot runtime (bounded recovery) --- */
    switch (s_sht45.state)
    {
        case DEVICE_STATE_NOT_FOUND:
        case DEVICE_STATE_RECOVERING:
            App_DoStartSht45();
            break;

        case DEVICE_STATE_ERROR:
            App_RefreshSht45Diagnostics();
            RoomState_InvalidateSht45(&s_room);
            /* Begin a bounded recovery epoch via the runtime API (rather than
               mutating s_sht45.state directly): increments recovery_count and
               resets the consecutive-error budget so the next probe/start/read
               sequence can actually run. */
            Sht45Runtime_Recover(&s_sht45);
            App_RefreshSht45Diagnostics();
            break;

        default:
            break;
    }

    /* --- BMP390 forced-mode runtime (bounded recovery) --- */
    switch (s_bmp390.state)
    {
        case DEVICE_STATE_NOT_FOUND:
        case DEVICE_STATE_RECOVERING:
            App_DoStartBmp390();
            break;

        case DEVICE_STATE_ERROR:
            App_RefreshBmp390Diagnostics();
            RoomState_InvalidateBmp390(&s_room);
            Bmp390Runtime_Recover(&s_bmp390);
            App_RefreshBmp390Diagnostics();
            break;

        default:
            break;
    }

    /* --- BMP380 forced-mode runtime (Phase 17.7B, bounded recovery) --- */
    switch (s_bmp380.state)
    {
        case DEVICE_STATE_NOT_FOUND:
        case DEVICE_STATE_RECOVERING:
            App_DoStartBmp380();
            break;

        case DEVICE_STATE_ERROR:
            App_RefreshBmp380Diagnostics();
            Bmp380Runtime_Recover(&s_bmp380);
            App_RefreshBmp380Diagnostics();
            break;

        default:
            break;
    }

    /* --- SGP41 VOC/NOx runtime (bounded recovery) --- */
    switch (s_sgp41.state)
    {
        case DEVICE_STATE_NOT_FOUND:
        case DEVICE_STATE_RECOVERING:
            App_DoStartSgp41();
            break;

        case DEVICE_STATE_ERROR:
            App_RefreshSgp41Diagnostics();
            RoomState_InvalidateSgp41(&s_room);
            Sgp41Runtime_Recover(&s_sgp41);
            App_RefreshSgp41Diagnostics();
            break;

        default:
            break;
    }
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
        case SELF_TEST_DEGRADED:return "DEGRADED";
        default:                return "NOT_RUN";
    }
}

static const char *StorageReadStatus_Str(StorageReadStatus s)
{
    switch (s)
    {
        case STORAGE_READ_OK:               return "OK";
        case STORAGE_READ_NOT_FOUND:        return "NOT_FOUND";
        case STORAGE_READ_CORRUPT:          return "CORRUPT";
        case STORAGE_READ_IO_ERROR:         return "IO_ERROR";
        case STORAGE_READ_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        default:                            return "UNKNOWN";
    }
}

static const char *StorageHealth_Str(StorageHealth h)
{
    switch (h)
    {
        case STORAGE_HEALTH_HEALTHY:     return "HEALTHY";
        case STORAGE_HEALTH_DEGRADED:    return "DEGRADED";
        case STORAGE_HEALTH_DEGRADED_IO: return "DEGRADED_IO";
        case STORAGE_HEALTH_CORRUPT:     return "CORRUPT";
        case STORAGE_HEALTH_IO_ERROR:    return "IO_ERROR";
        default:                         return "UNKNOWN";
    }
}

static void App_PrintBootDiag(void)
{
    char short_id[16];
    DeviceIdentity_GetShortId(&s_device_id, short_id, sizeof(short_id));

    printf("BOOT Reset=%s\r\n", ResetCauseStr(s_reset_cause));

    printf("SELFTEST Platform=%s I2C=%s Storage=%s Config=%s ID=%s VEML=%s Disp=%s CO2=%s SHT45=%s SGP41=%s\n",
           SelfTestResultStr(s_self_test.platform),
           SelfTestResultStr(s_self_test.i2c),
           SelfTestResultStr(s_self_test.storage),
           SelfTestResultStr(s_self_test.config),
           SelfTestResultStr(s_self_test.identity),
           SelfTestResultStr(s_self_test.light_sensor),
           SelfTestResultStr(s_self_test.display),
           SelfTestResultStr(s_self_test.co2_sensor),
           SelfTestResultStr(s_self_test.temp_humidity_sensor),
           SelfTestResultStr(s_self_test.air_quality_sensor));

    printf("CONFIG boot_source=%s cur=read=%s health=%s seq=%u calib=%.3f ID=%s\r\n",
           (s_config_boot_load_status == STORAGE_READ_OK) ? "persisted" : "defaults",
           StorageReadStatus_Str(Config_GetStorageStatus()),
           StorageHealth_Str(Config_GetStorageHealth()),
           (unsigned)Config_Get()->storage.version,
           (double)Config_Get()->runtime.light_calibration_factor,
           short_id);

    printf("IDENTITY valid=%d boot_source=%s cur=read=%s health=%s\r\n",
           (int)s_device_id_valid,
           (s_identity_boot_load_status == STORAGE_READ_OK) ? "persisted" : "derived",
           StorageReadStatus_Str(DeviceIdentity_GetPersistenceStatus()),
           StorageHealth_Str(DeviceIdentity_GetStorageHealth()));

    printf("TELEMETRY schema=%u period=%lu\r\n",
           (unsigned)TELEMETRY_SCHEMA_VERSION,
           (unsigned long)Config_Get()->storage.telemetry_period_ms);

    printf("WDG active=%d\r\n", (int)s_watchdog_active);
    printf("Health=%d\r\n", (int)s_health);
}

/* Boot maintenance: establish A/B redundancy for each known-valid persistent
   record so a normal first boot (or a power loss during a prior mirror write)
   reaches HEALTHY rather than remaining single-copy forever. App delegates to
   the Config and DeviceIdentity OWNER modules (Config_EnsureRedundancy /
   DeviceIdentity_EnsureRedundancy) — App never manipulates CONFIG/IDENTITY
   persistent records directly, so the owner caches of read-status and mirror
   health are refreshed by the owner and can never go stale. Safe mirror repair
   never erases the valid source and refuses IO uncertainty. A BLANK/absent
   provisioning record is deliberately left untouched (factory-new DISCOVERABLE
   state is healthy); registration mirrors form only from a real persisted
   record. Non-destructive by construction. */
static void App_BootEnsureRedundancy(void)
{
    Config_EnsureRedundancy();
    DeviceIdentity_EnsureRedundancy();
}

/* SCD41 is a real Room Sensor v1 capability. Its contribution to SystemHealth:
     STARTING/WAITING/READY -> acceptable (present and warming up, or measuring).
     NOT_FOUND/ERROR/RECOVERING -> NOT OK (degrades health).
   Merely not having the first 5 s sample yet (STARTING/WAITING) must NOT degrade
   health; a genuinely missing/errored/recovering SCD41 MUST degrade health (but
   never a FAULT, and never stops VEML/display/App). Exported so the mapping is
   directly regression-testable. */
bool App_Scd41HealthOk(DeviceState state)
{
    return (state == DEVICE_STATE_STARTING ||
            state == DEVICE_STATE_WAITING ||
            state == DEVICE_STATE_READY);
}

/* SHT45 SystemHealth contribution: STARTING/WAITING/READY -> acceptable;
   NOT_FOUND/ERROR/RECOVERING -> NOT OK (degrades health, never FAULT). */
bool App_Sht45HealthOk(DeviceState state)
{
    return (state == DEVICE_STATE_STARTING ||
            state == DEVICE_STATE_WAITING ||
            state == DEVICE_STATE_READY);
}

/* BMP390 SystemHealth contribution: STARTING/WAITING/READY -> acceptable;
   NOT_FOUND/ERROR/RECOVERING -> NOT OK (degrades health, never FAULT). */
bool App_Bmp390HealthOk(DeviceState state)
{
    return (state == DEVICE_STATE_STARTING ||
            state == DEVICE_STATE_WAITING ||
            state == DEVICE_STATE_READY);
}

/* Barometer health (Phase 17.7B): OK if EITHER BMP390 or BMP380 is acceptable.
   A missing BMP390 does not degrade barometric health when BMP380 provides the
   barometric capability. Both absent/errored -> degrades health, never FAULT. */
bool App_BarometerHealthOk(DeviceState state390, DeviceState state380)
{
    return App_Bmp390HealthOk(state390) || App_Bmp390HealthOk(state380);
}

/* SGP41 SystemHealth contribution: STARTING/WAITING/READY -> acceptable;
   NOT_FOUND/ERROR/RECOVERING -> NOT OK (degrades health, never FAULT). */
bool App_Sgp41HealthOk(DeviceState state)
{
    return (state == DEVICE_STATE_STARTING ||
            state == DEVICE_STATE_WAITING ||
            state == DEVICE_STATE_READY);
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
    /* Authoritative runtime health, NOT the SelfTest report. Redundancy health
       is separate from read status: a readable VALID+IO record reads OK but its
       mirror (A/B) is DEGRADED_IO and must degrade system health. */
    bool runtime_ok = veml_ready && disp_ready &&
                      s_storage_init_ok && Storage_IsInitialized();

    /* SCD41 SystemHealth contribution (see App_Scd41HealthOk). */
    bool scd41_ok = App_Scd41HealthOk(s_scd41.state);

    /* SHT45 SystemHealth contribution (see App_Sht45HealthOk). A missing or
       errored SHT45 degrades health but never faults the device. */
    bool sht45_ok = App_Sht45HealthOk(s_sht45.state);

    /* Barometer SystemHealth contribution (Phase 17.7B): OK if either BMP390 or
       BMP380 is acceptable (one active barometric provider). */
    bool barometer_ok = App_BarometerHealthOk(s_bmp390.state, s_bmp380.state);

    /* SGP41 SystemHealth contribution (see App_Sgp41HealthOk). */
    bool sgp41_ok = App_Sgp41HealthOk(s_sgp41.state);

    /* Persistence redundancy: healthy only when every A/B mirror is HEALTHY.
       A degraded mirror degrades system health without stopping sensing. */
    bool config_healthy  = (Config_GetStorageHealth() == STORAGE_HEALTH_HEALTHY);
    bool identity_healthy = (DeviceIdentity_GetStorageHealth() == STORAGE_HEALTH_HEALTHY);
    bool prov_healthy    = (Provisioning_GetStorageHealth() == STORAGE_HEALTH_HEALTHY);

    bool persistence_redundant_ok =
        config_healthy && identity_healthy && prov_healthy &&
        (Config_GetStorageStatus() != STORAGE_READ_CORRUPT) &&
        (Config_GetStorageStatus() != STORAGE_READ_IO_ERROR) &&
        (DeviceIdentity_GetPersistenceStatus() != STORAGE_READ_CORRUPT) &&
        (DeviceIdentity_GetPersistenceStatus() != STORAGE_READ_IO_ERROR) &&
        Provisioning_IsHealthy();

    /* A non-OK/FAULT state here is DEGRADED: sensing continues (the scheduler is
       never gated on health) and only the reported health reflects the mirror. */
    if (runtime_ok && persistence_redundant_ok && scd41_ok && sht45_ok && barometer_ok && sgp41_ok)
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

    I2cBusHealth_Init(&s_bus_health);
    DeviceRuntime_Init(&s_light_rt, DEVICE_STATE_NOT_FOUND);
    DeviceRuntime_Init(&s_disp_rt, DEVICE_STATE_NOT_FOUND);
    Scd41Runtime_Init(&s_scd41, s_i2c_bus);
    App_RefreshScd41Diagnostics();   /* s_co2_rt mirrors runtime initial state */
    Sht45Runtime_Init(&s_sht45, s_i2c_bus);
    App_RefreshSht45Diagnostics();   /* s_temp_rt mirrors runtime initial state */
    Bmp390Runtime_Init(&s_bmp390, s_i2c_bus);
    App_RefreshBmp390Diagnostics();  /* s_pres_rt mirrors runtime initial state */
    Bmp380Runtime_Init(&s_bmp380, s_i2c_bus);
    App_RefreshBmp380Diagnostics();  /* s_pres_rt_bmp380 mirrors BMP380 initial state */
    Sgp41Runtime_Init(&s_sgp41, s_i2c_bus);
    App_RefreshSgp41Diagnostics();   /* s_gas_rt mirrors runtime initial state */

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
        cmd_svc.runtime_status = &s_cmd_status;
        App_FillCommandRuntimeStatus();
        App_GetStatus(&s_status);
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

    Command_UpdateRuntime(now - s_start_ms, s_watchdog_active, &s_light_rt, &s_disp_rt, &s_co2_rt, s_reset_cause);

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
                s_config_boot_load_status = Config_GetStorageStatus();
                DeviceLifecycle_TransitionTo(LIFECYCLE_LOAD_IDENTITY);
                break;

            case LIFECYCLE_LOAD_IDENTITY:
            {
                if (DeviceIdentity_Load(&s_device_id))
                {
                    s_device_id_valid = true;
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
                        if (DeviceIdentity_GetLoadStatus() == STORAGE_READ_NOT_FOUND)
                            DeviceIdentity_Save(&derived);
                    }
                }
                s_identity_boot_load_status = DeviceIdentity_GetLoadStatus();
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
                if (s_config_boot_load_status == STORAGE_READ_NOT_FOUND)
                    Config_Save();
                /* Establish redundancy so first-boot / power-loss-heal devices
                   reach HEALTHY mirrors (Config + Identity). */
                App_BootEnsureRedundancy();
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

    /* SCD41 periodic runtime: advance the non-blocking state machine at its
       poll interval whenever it is measuring (STARTING/WAITING/READY). Never
       blocks; data-ready=false is simply a no-op poll. */
    if ((now - s_last_scd41_ms) >= SCD41_RUNTIME_POLL_INTERVAL_MS)
    {
        s_last_scd41_ms = now;
        if (s_scd41.state == DEVICE_STATE_STARTING ||
            s_scd41.state == DEVICE_STATE_WAITING ||
            s_scd41.state == DEVICE_STATE_READY)
        {
            App_DoPollScd41();
        }
    }

    /* SHT45 runtime: advance the non-blocking single-shot state machine. */
    if ((now - s_last_sht45_ms) >= SHT45_RUNTIME_POLL_INTERVAL_MS)
    {
        s_last_sht45_ms = now;
        if (s_sht45.state == DEVICE_STATE_STARTING ||
            s_sht45.state == DEVICE_STATE_WAITING ||
            s_sht45.state == DEVICE_STATE_READY)
        {
            App_DoPollSht45();
        }
    }

    /* BMP390 runtime: advance the non-blocking forced-mode state machine. */
    if ((now - s_last_bmp390_ms) >= BMP390_RUNTIME_POLL_INTERVAL_MS)
    {
        s_last_bmp390_ms = now;
        if (s_bmp390.state == DEVICE_STATE_STARTING ||
            s_bmp390.state == DEVICE_STATE_WAITING ||
            s_bmp390.state == DEVICE_STATE_READY)
        {
            App_DoPollBmp390();
        }
    }

    /* BMP380 runtime (Phase 17.7B): advance mirror non-blocking state machine. */
    if ((now - s_last_bmp380_ms) >= BMP380_RUNTIME_POLL_INTERVAL_MS)
    {
        s_last_bmp380_ms = now;
        if (s_bmp380.state == DEVICE_STATE_STARTING ||
            s_bmp380.state == DEVICE_STATE_WAITING ||
            s_bmp380.state == DEVICE_STATE_READY)
        {
            App_DoPollBmp380();
        }
    }

    /* Commit the active barometric provider into RoomState (atomic snapshot). */
    App_CommitBarometricRoomState();

    /* SGP41 runtime: advance the non-blocking VOC/NOx state machine. */
    if ((now - s_last_sgp41_ms) >= SGP41_RUNTIME_POLL_INTERVAL_MS)
    {
        s_last_sgp41_ms = now;
        if (s_sgp41.state == DEVICE_STATE_STARTING ||
            s_sgp41.state == DEVICE_STATE_WAITING ||
            s_sgp41.state == DEVICE_STATE_READY)
        {
            App_DoPollSgp41();
        }
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
               "CO2 state=%d ppm=%.0f valid=%d T=%.1f RH=%.1f ops=%lu err=%lu consec=%lu rec=%lu\r\n"
               "SHT45 state=%d T=%.1f RH=%.1f Tvalid=%d RHvalid=%d ops=%lu err=%lu consec=%lu rec=%lu\r\n"
               "SGP41 state=%d voc_raw=%.0f nox_raw=%.0f voc_idx=%d nox_idx=%d valid=%d/%d/%d ops=%lu err=%lu consec=%lu rec=%lu\r\n"
               "HEALTH=%d WDG=%d BUS_ATT=%lu SUC=%lu FAIL=%lu CFG read=%s health=%s "
               "IDENT read=%s health=%s COMM=%d sent=%lu failed=%lu\r\n",
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
               (int)s_co2_rt.state,
               (double)s_room.co2_ppm,
               (int)s_room.co2_valid,
               (double)s_room.scd41_temperature_c,
               (double)s_room.scd41_humidity_pct,
               (unsigned long)s_co2_rt.operation_successes,
               (unsigned long)s_co2_rt.operation_failures,
               (unsigned long)s_co2_rt.consecutive_errors,
               (unsigned long)s_co2_rt.recovery_count,
               (int)s_temp_rt.state,
               (double)s_room.sht45_temperature_c,
               (double)s_room.sht45_humidity_pct,
               (int)s_room.sht45_temperature_valid,
               (int)s_room.sht45_humidity_valid,
               (unsigned long)s_temp_rt.operation_successes,
               (unsigned long)s_temp_rt.operation_failures,
               (unsigned long)s_temp_rt.consecutive_errors,
               (unsigned long)s_temp_rt.recovery_count,
               (int)s_gas_rt.state,
               (double)s_room.voc_raw,
               (double)s_room.nox_raw,
               (int)s_room.voc_index,
               (int)s_room.nox_index,
               (int)s_room.voc_raw_valid,
               (int)s_room.voc_index_valid,
               (int)s_room.nox_index_valid,
               (unsigned long)s_gas_rt.operation_successes,
               (unsigned long)s_gas_rt.operation_failures,
               (unsigned long)s_gas_rt.consecutive_errors,
               (unsigned long)s_gas_rt.recovery_count,
               (int)s_health, (int)s_watchdog_active,
               (unsigned long)I2cBusHealth_GetBusRecoveryAttempts(&s_bus_health),
               (unsigned long)I2cBusHealth_GetBusRecoverySuccesses(&s_bus_health),
               (unsigned long)I2cBusHealth_GetBusRecoveryFailures(&s_bus_health),
               StorageReadStatus_Str(Config_GetStorageStatus()),
               StorageHealth_Str(Config_GetStorageHealth()),
               StorageReadStatus_Str(DeviceIdentity_GetPersistenceStatus()),
               StorageHealth_Str(DeviceIdentity_GetStorageHealth()),
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
            input.uptime_ms = now - s_start_ms;

            TelemetrySnapshot snap;
            if (Telemetry_CreateSnapshot(&snap, &input))
                Communication_SubmitSnapshot(&snap);
        }
    }

    Communication_Run();
    App_FillCommandRuntimeStatus();   /* keep CommandRuntimeStatus current for GET_STATUS */
    App_GetStatus(&s_status);         /* keep AppStatus.s_health etc current */
    Command_Run();

    Platform_WatchdogRefresh();
}

void App_GetStatus(AppStatus *status)
{
    if (status == NULL) return;

    /* Refresh the persistent live snapshot (cmd_svc.status points at s_status)
       so GET_STATUS always reads current runtime health. */
    /* AppStatus: co2 + thermal rely on the runtime diagnostic snapshots. */
    s_status.light_sensor = s_light_rt;
    s_status.display = s_disp_rt;
    s_status.co2_sensor = s_co2_rt;
    s_status.temp_humidity_sensor = s_temp_rt;
    s_status.pressure_sensor = s_pres_rt;
    s_status.gas_sensor = s_gas_rt;
    s_status.health = s_health;
    s_status.reset_cause = s_reset_cause;
    s_status.watchdog_active = s_watchdog_active;
    s_status.self_test = s_self_test;
    s_status.storage_initialized = s_storage_init_ok && Storage_IsInitialized();
    s_status.provisioning_initialized = Provisioning_IsHealthy();
    /* CURRENT persistence state from the owning modules (single source of
       truth), queried at call time so it is never stale. NOT the historical
       boot snapshot kept internally for the boot persistence policy. */
    s_status.config_storage_status = Config_GetStorageStatus();
    s_status.identity_storage_status = DeviceIdentity_GetPersistenceStatus();
    /* Redundancy health, separate from read status — surfaces a damaged mirror
       without claiming the record is unreadable. */
    s_status.config_storage_health = Config_GetStorageHealth();
    s_status.identity_storage_health = DeviceIdentity_GetStorageHealth();
    s_status.provisioning_storage_health = Provisioning_GetStorageHealth();
    s_status.uptime_ms = Platform_GetTickMs() - s_start_ms;

    if (status != &s_status)
        *status = s_status;
}

/* Populate the Command-facing runtime-status DTO from the authoritative owners.
   Command reads this snapshot only; ownership of state stays with App/modules. */
static void App_FillCommandRuntimeStatus(void)
{
    const ProvisioningRuntime *pr = Provisioning_GetRuntime();
    s_cmd_status.system_health = s_health;
    s_cmd_status.storage_initialized = s_storage_init_ok && Storage_IsInitialized();
    s_cmd_status.config_persistence = Config_GetStorageStatus();
    s_cmd_status.config_storage_health = Config_GetStorageHealth();
    s_cmd_status.identity_persistence = DeviceIdentity_GetPersistenceStatus();
    s_cmd_status.identity_storage_health = DeviceIdentity_GetStorageHealth();
    s_cmd_status.provisioning_state = (pr != NULL) ? pr->status.state : PROVISIONING_ERROR;
    s_cmd_status.provisioning_persistence = (pr != NULL) ? pr->storage_status : STORAGE_READ_IO_ERROR;
    s_cmd_status.provisioning_storage_health = Provisioning_GetStorageHealth();
}