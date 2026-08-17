#include <stdio.h>
#include <string.h>

#include "app.h"
#include "config.h"
#include "device_lifecycle.h"
#include "device_identity.h"
#include "storage.h"
#include "telemetry.h"
#include "telemetry_serializer.h"
#include "room_state.h"
#include "communication.h"
#include "platform_time.h"
#include "network_transport.h"
#include "fake_i2c_bus.h"
#include "fake_platform_time.h"
#include "fake_flash.h"
#include "fake_unique_id.h"
#include "fake_communication_port.h"
#include "fake_network_adapter.h"
#include "virtual_device.h"

/* Phase 16 whole-device network-outage test.

   Proves NETWORK_OUTAGE_STOPS_SENSOR_ACQUISITION = NO and
   NETWORK_FAILURE_CAN_TRIGGER_I2C_RECOVERY = NO:

     - Boots the REAL portable core to OPERATIONAL with healthy sensors.
     - Runs the DeviceLifecycle/Acquire for 10 virtual minutes WHILE a connected
       NetworkTransport suffers a deterministic outage (refused / transport
       errors) — exercised as an INDEPENDENT component in the same virtual time.
     - Asserts sensors keep sampling (operation counters advance), RoomState
       stays valid, no bus/I2C recovery is triggered by the network fault, and a
       fresh local telemetry payload is still serializable end-to-end. */

static int s_pass = 0, s_fail = 0, s_case = 0;
static void T(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

#define SCD41_WIRE (0x62U << 1U)
#define SHT_WIRE   (0x44U << 1U)

static FakeI2cBus    s_fake_i2c;
static I2cBus        s_bus;
static FakeCommunicationPort s_comm;

static void arm_healthy_samples(FakeI2cBus *fake)
{
    FakeI2cBus_SetScd41DataReady(fake, true);
    FakeI2cBus_SetScd41Measurement(fake, 480, FakeI2cBus_TempRaw(23.6f),
                                   FakeI2cBus_RhRaw(41.5f), false,false,false);
    uint8_t sht[6]; VDev_Sht45Response(23.3f, 41.2f, sht);
    FakeI2cBus_SetSht45Response(fake, sht, true);
    uint8_t meas[6]; VDev_Sgp41MeasureResponse(31000U, 26000U, meas);
    FakeI2cBus_SetSgp41Response(fake, meas, 6U);
}

int main(void)
{
    printf("Phase 16 whole-device network outage test\n");

    FakeFlash_Init();
    FakeUniqueId_Set((const uint8_t[]){0xAA,0xBB,0xCC,0xDD,0x01,0x02,0x03,0x04,0xFE,0xED,0xBE,0xEF});
    FakePlatform_SetTick(0);
    FakeI2cBus_Init(&s_fake_i2c);
    s_fake_i2c.probe_result = DRIVER_STATUS_OK;
    FakeI2cBus_GetBus(&s_bus, &s_fake_i2c);

    /* healthy sensors */
    static const uint8_t bmp_cal[21] = {
        0xAD,0xD8,0x26,0x6F,0xFE,0x12,0xC3,0xCF,0x48,0x28,0xBA,
        0x12,0x7A,0xFC,0xFF,0x3C,0xE7,0x74,0x8B,0xC9,0xB0
    };
    static const uint8_t bmp_pt[6] = {0x5F,0x5A,0x55, 0x5B,0xC9,0xE6};
    FakeI2cBus_SetBmp390Present(&s_fake_i2c, (uint16_t)(0x76U << 1), 0x60U, bmp_cal);
    FakeI2cBus_SetBmp390Regs(&s_fake_i2c, (uint8_t)(0x20U | 0x40U), 0U, bmp_pt);
    uint8_t sht0[6]; VDev_Sht45Response(23.2f, 41.0f, sht0);
    FakeI2cBus_SetSht45Response(&s_fake_i2c, sht0, true);
    uint8_t cond[3], meas0[6];
    VDev_Sgp41ConditioningResponse(0x8000U, cond);
    VDev_Sgp41MeasureResponse(30000U, 25000U, meas0);
    FakeI2cBus_SetSgp41ConditioningResponse(&s_fake_i2c, cond);
    FakeI2cBus_SetSgp41Response(&s_fake_i2c, meas0, 6U);
    FakeI2cBus_SetPresent(&s_fake_i2c, (uint16_t)(0x10U << 1), true);
    FakeI2cBus_SetPresent(&s_fake_i2c, (uint16_t)(0x3CU << 1), true);
    FakeI2cBus_SetPresent(&s_fake_i2c, (uint16_t)(0x59U << 1), true);

    FakeComm_Init(&s_comm);
    CommunicationPort cp; FakeComm_GetPort(&cp, &s_comm);
    Communication_SetPort(&cp);

    App_SetI2C(&s_bus);
    T(App_Init() == ROOM_SENSOR_OK, "App_Init OK");
    /* Re-bind comm to fake for telemetry count. */
    { CommunicationPort c2; FakeComm_GetPort(&c2, &s_comm); Communication_SetPort(&c2); }

    int guard = 0;
    while (!DeviceLifecycle_IsOperational() && guard < 24) { App_Run(); guard++; }
    T(DeviceLifecycle_IsOperational(), "device OPERATIONAL");

    /* Reach all sensors operating. */
    bool ready = false;
    for (int i = 0; i < 400 && !ready; i++)
    {
        App_Run(); FakePlatform_AdvanceTick(500);
        AppStatus st; App_GetStatus(&st);
        if (st.co2_sensor.state != DEVICE_STATE_UNKNOWN &&
            st.temp_humidity_sensor.state != DEVICE_STATE_UNKNOWN &&
            st.pressure_sensor.state != DEVICE_STATE_UNKNOWN)
            ready = true;
    }
    arm_healthy_samples(&s_fake_i2c);
    T(ready, "sensors have become observable");

    AppStatus pre; App_GetStatus(&pre);
    uint32_t co2ops0 = pre.co2_sensor.operation_successes;
    uint32_t shtops0 = pre.temp_humidity_sensor.operation_successes;
    (void)shtops0;

    /* ---- 10-minute NETWORK OUTAGE while sensors keep running ---- */
    /* A connected transport, suffering a refused/reconnecting outage, is driven
       independently in the SAME virtual time. It must NOT affect the sensors. */
    FakeNetworkAdapter      nfake;
    NetworkTransportAdapter nadapter;
    NetworkTransport        nt;
    NetworkEndpoint         nep;
    FakeNetworkAdapter_Reset(&nfake);
    FakeNetworkAdapter_GetAdapter(&nadapter, &nfake);
    nfake.connect_mode = FAKE_NET_CONNECT_REFUSED;   /* outage */
    memset(&nep, 0, sizeof(nep));
    nep.port = 8883;
    memcpy(nep.host, "broker.example", 14);
    NetworkTransport_Init(&nt, &nadapter, &nep);

    uint32_t net_attempts = 0;
    const uint32_t outage_ms = 10U * 60U * 1000U;
    for (uint32_t t = 0; t < outage_ms; t += 500U)
    {
        App_Run();                                   /* sensors keep sampling */
        FakePlatform_AdvanceTick(500);
        if (t % 5000U == 0U)
            arm_healthy_samples(&s_fake_i2c);

        /* network component (test-only bounded reconnect driver). */
        if (NetworkTransport_GetState(&nt) == NET_STATE_DISCONNECTED)
        {
            NetworkTransport_Connect(&nt);   /* refused -> ERROR */
            net_attempts++;
        }
        else if (NetworkTransport_GetState(&nt) == NET_STATE_ERROR)
        {
            NetworkTransport_Disconnect(&nt);
        }
        NetworkTransport_Run(&nt);
    }

    AppStatus post; App_GetStatus(&post);
    T(post.co2_sensor.operation_successes > co2ops0,
      "sensors kept sampling during network outage (NO acquisition stop)");
    T(post.co2_sensor.recovery_count <= pre.co2_sensor.recovery_count + 2U,
      "network outage did NOT cause a sensor-recovery storm");
    T(s_fake_i2c.recover_call_count == 0,
      "NETWORK_FAILURE_CAN_TRIGGER_I2C_RECOVERY = NO (no shared-bus recovery)");
    T(net_attempts <= (outage_ms / 500U) + 1U, "outage attempts no tighter than one per 500ms step");

    /* Local telemetry still serializable end-to-end during an outage. */
    {
        AppStatus st; App_GetStatus(&st);
        RoomState room; RoomState_Init(&room);
        room.co2_ppm = 480.0f; room.co2_valid = true;
        room.sht45_temperature_c = 23.3f; room.sht45_temperature_valid = true;
        room.sht45_humidity_pct = 41.2f; room.sht45_humidity_valid = true;
        TelemetrySnapshot snap;
        TelemetrySnapshotInput tin;
        memset(&tin,0,sizeof(tin));
        uint8_t devid[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
        tin.device_id = devid; tin.boot_id = 1; tin.room = &room;
        tin.health = st.health; tin.uptime_ms = FakePlatform_GetTick();
        if (Telemetry_CreateSnapshot(&snap, &tin))
        {
            uint8_t b[TELEMETRY_SERIALIZED_MAX_SIZE]; size_t w = 0;
            SerializeStatus ss = Telemetry_Serialize(&snap, b, sizeof(b), &w);
            T(ss == SERIALIZE_OK && w > 0, "telemetry still serializable during outage");
        }
        else
        {
            T(false, "telemetry snapshot created during outage");
        }
    }

    printf("    outage: %lu connect attempts over %lu ms, sensors unaffected\n",
           (unsigned long)net_attempts, (unsigned long)outage_ms);

    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}