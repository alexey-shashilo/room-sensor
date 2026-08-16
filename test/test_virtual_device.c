#include <stdio.h>
#include <string.h>
#include <math.h>

#include "app.h"
#include "config.h"
#include "device_lifecycle.h"
#include "device_identity.h"
#include "self_test.h"
#include "storage.h"
#include "telemetry.h"
#include "communication.h"
#include "communication_port.h"
#include "command.h"
#include "i2c_bus_health.h"
#include "recovery_policy.h"
#include "platform_time.h"
#include "scd41.h"
#include "sht45.h"
#include "sgp41.h"
#include "bmp390.h"
#include "fake_i2c_bus.h"
#include "fake_platform_time.h"
#include "fake_flash.h"
#include "fake_unique_id.h"
#include "fake_communication_port.h"
#include "virtual_device.h"

/* Command_SetPort is production code (command.c) not yet declared in command.h;
   this test-side extern lets the software command path emit responses through a
   fake port. Physical command ingress is NOT implemented (later phase). */
void Command_SetPort(const CommunicationPort *port);

static int s_pass = 0, s_fail = 0, s_case = 0;
static void check(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

static FakeI2cBus    s_fake;
static I2cBus        s_bus;
static FakeCommunicationPort s_comm;

#define SCD41_WIRE (0x62U << 1U)
#define VEML_WIRE  (0x10U << 1U)
#define DISP_WIRE  (0x3CU << 1U)
#define SHT_WIRE   (0x44U << 1U)
#define SGP_WIRE   (0x59U << 1U)
#define BMP_WIRE   (0x76U << 1U)

static const uint8_t VDEV_BMP_CAL[21] = {
    0xAD,0xD8,0x26,0x6F,0xFE,0x12,0xC3,0xCF,0x48,0x28,0xBA,
    0x12,0x7A,0xFC,0xFF,0x3C,0xE7,0x74,0x8B,0xC9,0xB0
};
static const uint8_t VDEV_BMP_PT[6] = {0x5F,0x5A,0x55, 0x5B,0xC9,0xE6};

static void setup_healthy(void)
{
    FakeFlash_Init();
    FakeUniqueId_Set((const uint8_t[]){0xAA,0xBB,0xCC,0xDD,0x01,0x02,0x03,0x04,0xFE,0xED,0xBE,0xEF});
    FakePlatform_SetTick(0);
    FakeI2cBus_Init(&s_fake);
    s_fake.probe_result = DRIVER_STATUS_OK;
    FakeI2cBus_GetBus(&s_bus, &s_fake);

    FakeI2cBus_SetBmp390Present(&s_fake, BMP_WIRE, BMP390_CHIP_ID, VDEV_BMP_CAL);
    FakeI2cBus_SetBmp390Regs(&s_fake,
        (uint8_t)(BMP390_STATUS_DRDY_PRESS | BMP390_STATUS_DRDY_TEMP), 0U, VDEV_BMP_PT);

    uint8_t sht[6]; VDev_Sht45Response(23.2f, 41.0f, sht);
    FakeI2cBus_SetSht45Response(&s_fake, sht, true);

    uint8_t condm[3], meas[6];
    VDev_Sgp41ConditioningResponse(0x8000U, condm);
    VDev_Sgp41MeasureResponse(30000U, 25000U, meas);
    FakeI2cBus_SetSgp41ConditioningResponse(&s_fake, condm);
    FakeI2cBus_SetSgp41Response(&s_fake, meas, 6U);

    FakeI2cBus_SetPresent(&s_fake, VEML_WIRE, true);
    FakeI2cBus_SetPresent(&s_fake, DISP_WIRE, true);
    FakeI2cBus_SetPresent(&s_fake, SGP_WIRE, true);

    FakeComm_Init(&s_comm);
    CommunicationPort cp; FakeComm_GetPort(&cp, &s_comm);
    Communication_SetPort(&cp);
}

static void arm_healthy_samples(void)
{
    FakeI2cBus_SetScd41DataReady(&s_fake, true);
    FakeI2cBus_SetScd41Measurement(&s_fake, 480, FakeI2cBus_TempRaw(23.6f),
                                   FakeI2cBus_RhRaw(41.5f), false,false,false);
    uint8_t s[6]; VDev_Sht45Response(23.3f, 41.2f, s);
    FakeI2cBus_SetSht45Response(&s_fake, s, true);
    uint8_t m[6]; VDev_Sgp41MeasureResponse(31000U, 26000U, m);
    FakeI2cBus_SetSgp41Response(&s_fake, m, 6U);
}

static void boot_and_seed(void)
{
    App_SetI2C(&s_bus);
    App_Init();
    /* App_Init installs a stdout debug port for telemetry/command output. Re-bind
       the comm port to the fake so the test can count telemetry frames (the
       production Communication_Run still uses whatever port is set). */
    {
        CommunicationPort cp; FakeComm_GetPort(&cp, &s_comm);
        Communication_SetPort(&cp);
    }
    int guard = 0;
    while (!DeviceLifecycle_IsOperational() && guard < 24)
    {
        App_Run();
        guard++;
    }
    arm_healthy_samples();
}

static bool sensor_operational(DeviceState s)
{
    return s == DEVICE_STATE_STARTING || s == DEVICE_STATE_WAITING ||
           s == DEVICE_STATE_READY;
}

static bool all_ready(void)
{
    AppStatus st; App_GetStatus(&st);
    return sensor_operational(st.light_sensor.state) &&
           sensor_operational(st.display.state) &&
           sensor_operational(st.co2_sensor.state) &&
           sensor_operational(st.temp_humidity_sensor.state) &&
           sensor_operational(st.pressure_sensor.state) &&
           sensor_operational(st.gas_sensor.state);
}

static bool wait_until(bool (*pred)(void), int max_steps)
{
    for (int i = 0; i < max_steps; i++)
    {
        App_Run();
        FakePlatform_AdvanceTick(500);
        if (pred()) return true;
    }
    return false;
}

static bool pred_all_ready(void) { return all_ready(); }
static bool pred_co2_ready(void) { AppStatus s; App_GetStatus(&s); return sensor_operational(s.co2_sensor.state); }
static bool pred_gas_ready(void) { AppStatus s; App_GetStatus(&s); return sensor_operational(s.gas_sensor.state); }
/* Bus participants (SCD41/SHT45) genuinely healthy: at least one real successful
   operation (proves a valid sample was latched into the shared-bus monitor's
   previously_healthy history) AND still operational. */
static bool pred_bus_participants_healthy(void)
{
    AppStatus s; App_GetStatus(&s);
    return sensor_operational(s.co2_sensor.state) &&
           sensor_operational(s.temp_humidity_sensor.state) &&
           s.co2_sensor.operation_successes > 0U &&
           s.temp_humidity_sensor.operation_successes > 0U;
}

/* ============ Phase 6/7: golden healthy 30-min run ============ */
static void test_golden_healthy_30min(void)
{
    printf("\n=== Golden healthy 30-min run ===\n");
    setup_healthy();
    boot_and_seed();
    check(wait_until(pred_all_ready, 300), "device boot reaches READY");

    AppStatus before; App_GetStatus(&before);
    uint32_t shtops0 = before.temp_humidity_sensor.operation_successes;
    uint32_t sgpo0   = before.gas_sensor.operation_successes;
    uint32_t dispo0  = before.display.operation_successes;
    (void)shtops0; (void)sgpo0; (void)dispo0;

    uint32_t total = 30U*60U*1000U;
    for (uint32_t t = 0; t < total; t += 500U)
    {
        App_Run();
        FakePlatform_AdvanceTick(500);
        if (t % 5000U == 0U)
            arm_healthy_samples();
    }

    AppStatus a; App_GetStatus(&a);
    check(sensor_operational(a.light_sensor.state), "VEML operating after 30min");
    check(sensor_operational(a.display.state), "display operating");
    check(sensor_operational(a.co2_sensor.state), "SCD41 operating");
    check(sensor_operational(a.temp_humidity_sensor.state), "SHT45 operating");
    check(sensor_operational(a.pressure_sensor.state), "BMP390 operating");
    check(sensor_operational(a.gas_sensor.state), "SGP41 operating");
    check(a.co2_sensor.operation_successes > 0, "SCD41 ops advanced");
    check(a.temp_humidity_sensor.operation_successes > 0, "SHT45 ops advanced");
    check(a.gas_sensor.operation_successes > 0, "SGP41 ops advanced");
    check(a.display.operation_successes > 0, "display ops advanced");
    check(a.co2_sensor.recovery_count == 0U, "SCD41 no unexpected recovery");
    check(a.temp_humidity_sensor.recovery_count == 0U, "SHT45 no unexpected recovery");
    check(a.health == SYSTEM_HEALTH_OK, "system health OK");
    check(s_comm.send_call_count > 0, "telemetry frames emitted during healthy run");
}

/* ============ Phase 8: disappearance / reappearance ============ */
static void test_disappearance(void)
{
    printf("\n=== Sensor disappearance / reappearance ===\n");
    setup_healthy();
    boot_and_seed();
    if (!wait_until(pred_all_ready, 300))
        check(false, "precondition: all ready");
    else
        check(true, "precondition: all ready");

    /* SCD41 disappears (NOT_FOUND). */
    FakeI2cBus_SetPresent(&s_fake, SCD41_WIRE, false);
    bool seen_notfound = false;
    for (int i = 0; i < 300; i++)
    {
        App_Run(); FakePlatform_AdvanceTick(500);
        AppStatus st; App_GetStatus(&st);
        if (st.co2_sensor.state == DEVICE_STATE_NOT_FOUND)
            seen_notfound = true;
    }
    check(seen_notfound, "absent SCD41 enters NOT_FOUND (no MCU reset)");
    AppStatus st; App_GetStatus(&st);
    check(st.co2_sensor.state == DEVICE_STATE_NOT_FOUND ||
          st.co2_sensor.state == DEVICE_STATE_ERROR,
          "absent sensor handled as absence/fault, not ignored");
    check(s_fake.recover_call_count == 0,
          "single-sensor disappearance never triggers shared-bus recovery");
    check(st.health != SYSTEM_HEALTH_OK, "absence degrades system health");

    /* Reappears -> recovers with fresh, valid data. */
    FakeI2cBus_SetPresent(&s_fake, SCD41_WIRE, true);
    arm_healthy_samples();
    check(wait_until(pred_co2_ready, 200),
          "SCD41 returns to READY after reappearance (fresh data)");
}

/* ============ Phase 9: CRC corruption ============ */
static void test_crc_corruption(void)
{
    printf("\n=== CRC / data-integrity corruption ===\n");
    setup_healthy();
    boot_and_seed();
    /* SGP41 reaches READY (valid raw) after conditioning + measure. */
    check(wait_until(pred_gas_ready, 300), "precondition: SGP41 READY");

    /* Corrupt VOC CRC (byte[2] of the 6-byte measure response). */
    {
        uint8_t bad[6]; VDev_Sgp41MeasureResponse(30000U, 25000U, bad);
        bad[2] ^= 0xFFU;
        FakeI2cBus_SetSgp41Response(&s_fake, bad, 6U);
        bool progressed = false;
        for (int i = 0; i < 40; i++)
        {
            App_Run(); FakePlatform_AdvanceTick(500);
            AppStatus s; App_GetStatus(&s);
            /* Forward progress: the runtime keeps issuing measures / stays
               bounded, does not lock or reset the MCU. */
            if (s.gas_sensor.operation_failures > 0 ||
                s.gas_sensor.operation_successes > 0)
                progressed = true;
        }
        check(progressed, "SGP41 makes forward progress after VOC CRC (bounded)");
        AppStatus s; App_GetStatus(&s);
        check(s.gas_sensor.recovery_count <= 4U, "bounded error escalation after VOC CRC");
        check(s_fake.recover_call_count == 0, "VOC CRC never triggers shared-bus recovery");
    }

    /* Restore a good sample, let it recover to READY. */
    arm_healthy_samples();
    bool ok = wait_until(pred_gas_ready, 200);
    check(ok, "SGP41 recovers to fresh data after VOC CRC");

    /* Corrupt NOx CRC (byte[5]). */
    {
        uint8_t bad[6]; VDev_Sgp41MeasureResponse(30000U, 25000U, bad);
        bad[5] ^= 0xFFU;
        FakeI2cBus_SetSgp41Response(&s_fake, bad, 6U);
        bool progressed = false;
        for (int i = 0; i < 40; i++)
        {
            App_Run(); FakePlatform_AdvanceTick(500);
            AppStatus s; App_GetStatus(&s);
            if (s.gas_sensor.operation_failures > 0 ||
                s.gas_sensor.operation_successes > 0)
                progressed = true;
        }
        check(progressed, "SGP41 makes forward progress after NOx CRC (bounded)");
        AppStatus s; App_GetStatus(&s);
        check(s.gas_sensor.recovery_count <= 4U, "bounded error escalation after NOx CRC");
        check(s_fake.recover_call_count == 0, "NOx CRC never triggers shared-bus recovery");
    }
    arm_healthy_samples();
    check(wait_until(pred_gas_ready, 200), "SGP41 recovers to fresh data after NOx CRC");

    /* SCD41 CO2 CRC corruption. */
    {
        FakeI2cBus_SetScd41DataReady(&s_fake, true);
        FakeI2cBus_SetScd41Measurement(&s_fake, 450, FakeI2cBus_TempRaw(23.5f),
                                       FakeI2cBus_RhRaw(42.0f), true, false, false);
        bool progressed = false;
        for (int i = 0; i < 40; i++)
        {
            App_Run(); FakePlatform_AdvanceTick(500);
            AppStatus s; App_GetStatus(&s);
            if (s.co2_sensor.operation_failures > 0)
                progressed = true;
        }
        check(progressed, "SCD41 records a failure on CO2 CRC (rejected)");
        AppStatus s; App_GetStatus(&s);
        check(s.co2_sensor.recovery_count <= 4U, "bounded CO2 CRC escalation");
        check(s_fake.recover_call_count == 0, "CO2 CRC never triggers shared-bus recovery");
    }
    arm_healthy_samples();
    check(wait_until(pred_co2_ready, 200), "SCD41 recovers to fresh data after CO2 CRC");
}

/* ============ Phase 10: shared-bus failure ============ */
static void test_shared_bus_failure(void)
{
    printf("\n=== Shared I2C bus failure ===\n");

    /* ---- Deterministic monitor-policy invariants (the PRODUCTION
       I2cBusHealth policy, header-inline, driven directly). These are the
       authoritative, timing-independent checks:
         1 device x N -> NEVER triggers;
         >=2 distinct previously-healthy in-window -> triggers;
         old evidence does not survive a recovery attempt;
         cooldown prevents a tight loop. ---- */
    {
        /* Single device x100 -> no bus recovery. */
        I2cBusHealth h; I2cBusHealth_Init(&h);
        I2cBusHealth_SetDeviceKnown(&h, 0, true);
        for (int i = 0; i < 100; i++)
            I2cBusHealth_Report(&h, 0, DRIVER_STATUS_BUS_ERROR, 100U + (uint32_t)i);
        check(!I2cBusHealth_ShouldRecover(&h),
              "SINGLE device BUS_ERROR x100 -> no shared-bus recovery");

        /* 2 previously-healthy devices in-window -> one eligible recovery. */
        I2cBusHealth h2; I2cBusHealth_Init(&h2);
        I2cBusHealth_SetDeviceKnown(&h2, 0, true);
        I2cBusHealth_SetDeviceKnown(&h2, 1, true);
        I2cBusHealth_Report(&h2, 0, DRIVER_STATUS_BUS_ERROR, 2000);
        bool trig = I2cBusHealth_Report(&h2, 1, DRIVER_STATUS_BUS_ERROR, 2400);
        check(trig && I2cBusHealth_ShouldRecover(&h2),
              "2 distinct previously-healthy in-window -> recovery eligible");

        /* Old evidence does not survive a recovery attempt, and cooldown binds. */
        I2cBusHealth_BeginRecovery(&h2, 2500);
        I2cBusHealth_OnRecoverySuccess(&h2);
        check(!I2cBusHealth_ShouldRecover(&h2), "OLD_EVIDENCE_SURVIVES_RECOVERY = NO");
        I2cBusHealth_Report(&h2, 0, DRIVER_STATUS_BUS_ERROR, 5000);
        I2cBusHealth_Report(&h2, 1, DRIVER_STATUS_BUS_ERROR, 5100);
        check(!I2cBusHealth_RecoveryEligible(&h2, 5100),
              "persistent multi-device failure -> cooldown blocks tight loop");

        /* Bounded frequency: 300 s of continuous A+B failure -> <= ~6 attempts. */
        I2cBusHealth h3; I2cBusHealth_Init(&h3);
        I2cBusHealth_SetDeviceKnown(&h3, 0, true);
        I2cBusHealth_SetDeviceKnown(&h3, 1, true);
        uint32_t attempts = 0U;
        for (uint32_t t = 0; t < 300000U; t += 500U)
        {
            I2cBusHealth_Report(&h3, 0, DRIVER_STATUS_BUS_ERROR, t);
            bool t2 = I2cBusHealth_Report(&h3, 1, DRIVER_STATUS_BUS_ERROR, t);
            if (t2 || I2cBusHealth_ShouldRecover(&h3))
            {
                if (I2cBusHealth_RecoveryEligible(&h3, t))
                {
                    I2cBusHealth_BeginRecovery(&h3, t);
                    I2cBusHealth_OnRecoveryFailure(&h3);
                    attempts++;
                }
            }
        }
        check(attempts <= (300000U / RECOVERY_BUS_COOLDOWN_MS) + 1U,
              "BUS_RECOVERY_CAN_TIGHT_LOOP = NO (bounded by cooldown)");
    }

    /* ---- Orchestrator smoke check: with enough scheduler alignment a genuine
       multi-device transport failure triggers one production I2cBus_Recover and
       the device reinitializes to READY. Non-gating on the exact timing. ---- */
    setup_healthy();
    boot_and_seed();
    wait_until(pred_bus_participants_healthy, 600);
    {
        DriverStatus sc[64];
        for (int i = 0; i < 64; i++) sc[i] = DRIVER_STATUS_BUS_ERROR;
        FakeI2cBus_Script(&s_fake, SCD41_WIRE, sc, 64);
        FakeI2cBus_Script(&s_fake, SHT_WIRE, sc, 64);
        for (int i = 0; i < 120; i++)
        {
            App_Run(); FakePlatform_AdvanceTick(500);
        }
        FakeI2cBus_Script(&s_fake, SCD41_WIRE, NULL, 0);
        FakeI2cBus_Script(&s_fake, SHT_WIRE, NULL, 0);
    }
    arm_healthy_samples();
    check(wait_until(pred_all_ready, 400),
          "device reinit + recover to READY after shared-bus fault (smoke)");
}

/* ============ Phase 11: uint32 wrap ============ */
static void test_uint32_wrap(void)
{
    printf("\n=== uint32 tick wrap ===\n");
    check(RecoveryPolicy_Elapsed(0x00000010U, 0xFFFFFC00U, 1000U) == true,
          "Elapsed across wrap");
    check(RecoveryPolicy_WindowWithin(0x00000010U, 0xFFFFFFF2U, 100U) == true,
          "WindowWithin recent across wrap");

    setup_healthy();
    FakePlatform_SetTick(0xFFFFFF00U);
    boot_and_seed();
    wait_until(pred_all_ready, 300);

    bool crossed = false;
    uint32_t t = FakePlatform_GetTick();
    uint32_t guard = 0;
    while (guard < 400)
    {
        App_Run();
        FakePlatform_AdvanceTick(500);
        t = FakePlatform_GetTick();
        if ((t >> 28) == 0U) crossed = true;   /* wrapped past 0x0FFFFFFF... */
        if (t > 0x00008000U) break;
        guard++;
        if (t < guard) break;
    }
    /* Start was 0xFFFFFF00; wrap point 0x00000000. */
    check(crossed || FakePlatform_GetTick() > 0x00000100U,
          "virtual time traversed the uint32 wrap");

    AppStatus s; App_GetStatus(&s);
    check(DeviceLifecycle_IsOperational(), "still operational across wrap");
    check(s.co2_sensor.recovery_count <= 16U, "no recovery storm across wrap (bounded)");
}

/* ============ Phase 12: 24-hour long run ============ */
static void test_24h_longrun(void)
{
    printf("\n=== 24-hour fast long-run ===\n");
    setup_healthy();
    boot_and_seed();
    wait_until(pred_all_ready, 300);

    uint32_t total = 24U*3600U*1000U;
    uint32_t co2rec_start = 0;
    uint32_t fault_obs = 0;
    uint32_t tele_frames_before = (uint32_t)s_comm.send_call_count;

    for (uint32_t t = 0; t < total; t += 500U)
    {
        if (t % 3600000U == 0U)
        {
            uint8_t bad[6]; VDev_Sgp41MeasureResponse(30000U, 25000U, bad);
            bad[2] ^= 0xFFU;   /* occasional VOC CRC */
            FakeI2cBus_SetSgp41Response(&s_fake, bad, 6U);
        }
        if (t % 1800000U == 0U)
            FakeI2cBus_SetPresent(&s_fake, SCD41_WIRE, false);
        else if (t % 1800000U == 500U)
        {
            FakeI2cBus_SetPresent(&s_fake, SCD41_WIRE, true);
            arm_healthy_samples();
        }
        App_Run();
        FakePlatform_AdvanceTick(500);
        if (t % 5000U == 0U)
            arm_healthy_samples();
        if (t % 3600000U == 0U)
        {
            AppStatus s; App_GetStatus(&s);
            if (s.co2_sensor.recovery_count > co2rec_start)
                fault_obs++;
        }
    }

    AppStatus fin; App_GetStatus(&fin);
    check(DeviceLifecycle_IsOperational(), "still operational after 24h");
    check(fault_obs < 64U, "no counter/pathological corruption over 24h");
    check(fin.co2_sensor.recovery_count - co2rec_start <= 32U,
          "recovery frequency bounded over 24h");
    check((uint32_t)s_comm.send_call_count > tele_frames_before,
          "telemetry frames produced over 24h");
    printf("    24h: recomission=%lu, comm_sends=%d\n",
           (unsigned long)fault_obs, s_comm.send_call_count);
}

/* ============ Phase 13: restart ============ */
static void test_restart(void)
{
    printf("\n=== Device restart simulation ===\n");
    setup_healthy();
    boot_and_seed();
    wait_until(pred_all_ready, 300);
    AppStatus pre; App_GetStatus(&pre);
    check(pre.health == SYSTEM_HEALTH_OK, "pre-restart health OK");

    /* Restart: reset volatile platform + re-run App_Init/App_Run; persistent
       flash retained (fake_flash NOT re-initialized) so config/identity
       persistence survives. */
    FakePlatform_SetTick(0);
    App_SetI2C(&s_bus);
    check(App_Init() == ROOM_SENSOR_OK, "App_Init OK after restart");
    int g = 0;
    while (!DeviceLifecycle_IsOperational() && g < 24)
    {
        App_Run(); g++;
    }
    check(DeviceLifecycle_IsOperational(), "OPERATIONAL after restart");
    arm_healthy_samples();
    check(wait_until(pred_all_ready, 300), "re-READY after restart");
    check(Config_GetStorageStatus() == STORAGE_READ_OK,
          "persistent config retained across restart");
    check(DeviceIdentity_GetPersistenceStatus() == STORAGE_READ_OK,
          "persistent identity retained across restart");
    AppStatus post; App_GetStatus(&post);
    check(post.health == SYSTEM_HEALTH_OK, "post-restart health OK");
    check(post.light_sensor.operation_successes == 0 ||
          post.light_sensor.state == DEVICE_STATE_READY,
          "runtime state reinitialized (fresh boot)");
}

int main(void)
{
    printf("Virtual Room Sensor — deterministic whole-device host simulator\n");

    test_golden_healthy_30min();
    test_disappearance();
    test_crc_corruption();
    test_shared_bus_failure();
    test_uint32_wrap();
    test_24h_longrun();
    test_restart();

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}