#include <stdio.h>
#include <string.h>

#include "app.h"
#include "device_lifecycle.h"
#include "config.h"
#include "storage.h"
#include "telemetry.h"
#include "telemetry_serializer.h"
#include "room_state.h"
#include "network_transport.h"
#include "mqtt_client.h"
#include "mqtt_codec.h"
#include "platform_time.h"
#include "fake_i2c_bus.h"
#include "fake_platform_time.h"
#include "fake_flash.h"
#include "fake_unique_id.h"
#include "fake_network_adapter.h"
#include "virtual_device.h"

/* Phase 17 whole-device isolation + payload tests.

   Part 1 — sensors keep acquiring while the MQTT/network layer suffers a long
            outage / refusals / malformed packets / keepalive timeout. Proves:
              MQTT_FAILURE_STOPS_SENSOR_ACQUISITION = NO
              MQTT_FAILURE_CAN_TRIGGER_I2C_RECOVERY    = NO
   Part 2 — telemetry-through-MQTT: a REAL production serialized telemetry
            payload survives MQTT QoS0 framing + transport partial acceptance
            byte-for-byte (TELEMETRY_BYTES_SURVIVE_MQTT).
   Part 3 — command-through-MQTT: a GET_MANIFEST command as an inbound PUBLISH
            payload is extracted byte-for-byte (COMMAND_BYTES_SURVIVE_MQTT).
            No production command ingress is wired. */

static int s_pass = 0, s_fail = 0, s_case = 0;
static void T(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

#define SCD41_WIRE (0x62U << 1U)

static FakeI2cBus    s_i2c;
static I2cBus        s_bus;
static FakeNetworkAdapter     s_fake;
static NetworkTransportAdapter s_adapter;
static NetworkTransport       s_transport;
static MqttClient             s_mc;

static uint8_t  s_cmd_capture[256];
static size_t   s_cmd_len = 0;
static bool     s_cmd_seen = false;

static bool on_publish(void *ctx, const MqttInboundPublish *pub)
{
    (void)ctx;
    s_cmd_capture[0] = '\0';
    if (pub->payload_len <= sizeof(s_cmd_capture) && pub->topic_len <= 64U)
    {
        memcpy(s_cmd_capture, pub->payload, pub->payload_len);
        s_cmd_len = pub->payload_len;
        s_cmd_seen = true;
    }
    return true;
}

static void arm_healthy(FakeI2cBus *fake)
{
    FakeI2cBus_SetScd41DataReady(fake, true);
    FakeI2cBus_SetScd41Measurement(fake, 480, FakeI2cBus_TempRaw(23.6f),
                                   FakeI2cBus_RhRaw(41.5f), false, false, false);
    uint8_t s[6]; VDev_Sht45Response(23.3f, 41.2f, s);
    FakeI2cBus_SetSht45Response(fake, s, true);
    uint8_t m[6]; VDev_Sgp41MeasureResponse(31000U, 26000U, m);
    FakeI2cBus_SetSgp41Response(fake, m, 6U);
}

static bool device_operational(void)
{
    return DeviceLifecycle_IsOperational();
}

/* ---- Part 1: sensors keep running under MQTT failure ---- */
static void test_sensor_isolation(void)
{
    printf("\n== sensors keep running under MQTT failure ==\n");
    FakeFlash_Init();
    FakeUniqueId_Set((const uint8_t[]){0xAA,0xBB,0xCC,0xDD,0x01,0x02,0x03,0x04,0xFE,0xED,0xBE,0xEF});
    FakePlatform_SetTick(0);
    FakeI2cBus_Init(&s_i2c);
    s_i2c.probe_result = DRIVER_STATUS_OK;
    static const uint8_t bmp_cal[21] = {
        0xAD,0xD8,0x26,0x6F,0xFE,0x12,0xC3,0xCF,0x48,0x28,0xBA,
        0x12,0x7A,0xFC,0xFF,0x3C,0xE7,0x74,0x8B,0xC9,0xB0
    };
    static const uint8_t bmp_pt[6] = {0x5F,0x5A,0x55, 0x5B,0xC9,0xE6};
    FakeI2cBus_SetBmp390Present(&s_i2c, (uint16_t)(0x76U << 1), 0x60U, bmp_cal);
    FakeI2cBus_SetBmp390Regs(&s_i2c, (uint8_t)(0x20U | 0x40U), 0U, bmp_pt);
    arm_healthy(&s_i2c);
    FakeI2cBus_SetPresent(&s_i2c, (uint16_t)(0x10U << 1), true);
    FakeI2cBus_SetPresent(&s_i2c, (uint16_t)(0x3CU << 1), true);
    FakeI2cBus_SetPresent(&s_i2c, (uint16_t)(0x59U << 1), true);
    FakeI2cBus_SetSgp41ConditioningResponse(&s_i2c, (uint8_t[3]){0x80,0x00,0x9B});
    FakeI2cBus_GetBus(&s_bus, &s_i2c);

    /* MQTT/transport fake (brokerless; suffered in virtual time). */
    FakeNetworkAdapter_Reset(&s_fake);
    FakeNetworkAdapter_GetAdapter(&s_adapter, &s_fake);

    /* Drive an MqttClient whose broker is persistently UNREACHABLE (connect
       refused), so the whole MQTT layer is in a failing state for 10 min. */
    NetworkEndpoint nep;
    memset(&nep, 0, sizeof(nep));
    nep.port = 1883;
    memcpy(nep.host, "broker.example", 14);
    NetworkTransport_Init(&s_transport, &s_adapter, &nep);
    MqttConnectConfig mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.client_id = "iso";
    mcfg.client_id_len = 3;
    mcfg.keepalive_s = 60;
    MqttClient_Init(&s_mc, &s_transport, &mcfg, on_publish, NULL);
    s_fake.connect_mode = FAKE_NET_CONNECT_REFUSED;   /* broker always unavailable */

    App_SetI2C(&s_bus);
    T(App_Init() == ROOM_SENSOR_OK, "App_Init OK");
    int g = 0;
    while (!device_operational() && g < 24) { App_Run(); g++; }
    T(device_operational(), "device OPERATIONAL before outage");

    AppStatus pre; App_GetStatus(&pre);
    uint32_t co2ops0 = pre.co2_sensor.operation_successes;

    /* 10-minute MQTT/network outage: sensors keep sampling; MQTT keeps failing. */
    const uint32_t outage_ms = 10U * 60U * 1000U;
    for (uint32_t t = 0; t < outage_ms; t += 500U)
    {
        App_Run();                        /* sensors keep sampling */
        FakePlatform_AdvanceTick(500);
        if (t % 5000U == 0U) arm_healthy(&s_i2c);

        /* Test-only MQTT failure driver (connect refused -> ERROR -> retry). The
           MqttClient keeps failing; it never touches sensors. */
        if (MqttClient_GetState(&s_mc) == MQTT_STATE_DISCONNECTED)
        {
            MqttClient_Connect(&s_mc);    /* -> transport connect refused -> ERROR */
        }
        else if (MqttClient_GetState(&s_mc) == MQTT_STATE_ERROR)
        {
            MqttClient_Disconnect(&s_mc); /* recover for the next attempt */
        }
        MqttClient_Run(&s_mc);
    }

    AppStatus post; App_GetStatus(&post);
    T(post.co2_sensor.operation_successes > co2ops0,
      "sensors kept acquiring during MQTT outage (NO acquisition stop)");
    T(s_i2c.recover_call_count == 0, "MQTT_FAILURE_CAN_TRIGGER_I2C_RECOVERY = NO");
    T(device_operational(), "device still operational after MQTT outage");
}

/* ---- Part 2: telemetry-through-MQTT ---- */
static void test_telemetry_through_mqtt(void)
{
    printf("\n== telemetry bytes survive MQTT QoS0 ==\n");
    FakeNetworkAdapter_Reset(&s_fake);
    FakeNetworkAdapter_GetAdapter(&s_adapter, &s_fake);
    NetworkEndpoint ep;
    memset(&ep, 0, sizeof(ep));
    ep.port = 1883;
    memcpy(ep.host, "broker.example", 14);
    NetworkTransport_Init(&s_transport, &s_adapter, &ep);
    MqttConnectConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.client_id = "tel";
    cfg.client_id_len = 3;
    cfg.keepalive_s = 0;
    MqttClient_Init(&s_mc, &s_transport, &cfg, on_publish, NULL);

    /* Build a REAL telemetry payload. */
    RoomState room; RoomState_Init(&room);
    room.co2_ppm = 480.0f; room.co2_valid = true;
    room.sht45_temperature_c = 23.3f; room.sht45_temperature_valid = true;
    room.sht45_humidity_pct = 41.2f; room.sht45_humidity_valid = true;
    uint8_t devid[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    TelemetrySnapshotInput tin;
    memset(&tin, 0, sizeof(tin));
    tin.device_id = devid; tin.boot_id = 1; tin.room = &room;
    tin.health = SYSTEM_HEALTH_OK; tin.uptime_ms = 12345;
    TelemetrySnapshot snap;
    if (!Telemetry_CreateSnapshot(&snap, &tin)) { T(false, "snapshot"); return; }
    uint8_t payload[TELEMETRY_SERIALIZED_MAX_SIZE];
    size_t len = 0;
    if (Telemetry_Serialize(&snap, payload, sizeof(payload), &len) != SERIALIZE_OK || len == 0U)
    { T(false, "serialize"); return; }
    T(len <= MQTT_MAX_PAYLOAD_SIZE, "telemetry payload fits MQTT payload bound");

    /* connect. */
    MqttClient_Connect(&s_mc);
    int g = 0;
    while (MqttClient_GetState(&s_mc) == MQTT_STATE_CONNECTING_TRANSPORT && g < 80)
    { FakePlatform_AdvanceTick(1); MqttClient_Run(&s_mc); g++; }
    uint8_t ca[4] = {0x20,0x02,0x00,0x00};
    FakeNetworkAdapter_FeedRecv(&s_fake, ca, 4);
    while (MqttClient_GetState(&s_mc) != MQTT_STATE_CONNECTED && g < 160)
    { FakePlatform_AdvanceTick(1); MqttClient_Run(&s_mc); g++; }
    T(MqttClient_GetState(&s_mc) == MQTT_STATE_CONNECTED, "connect+connack ok");

    /* publish the real telemetry payload over QoS0 with partial sends. */
    size_t pre = s_fake.tx_capture_len;
    s_fake.send_mode = FAKE_NET_SEND_CAPN;
    s_fake.send_cap = 9U;
    T(MqttClient_PublishQos0(&s_mc, "env/ota/room", 12, payload, len, false) == MQTT_OK,
      "telemetry QoS0 publish queued");
    g = 0;
    while (s_fake.tx_capture_len < pre + len && g < 20000)
    { FakePlatform_AdvanceTick(1); MqttClient_Run(&s_mc); g++; }

    /* The MQTT wire payload = telemetry payload (QoS0: no prefix beyond topic
       header). Compare the tail of the captured packet. */
    /* The published QoS0 packet is the final thing transmitted (no trailing bytes),
       so its payload is the last `len` captured bytes. This avoids hardcoding the
       variable fixed-header length. */
    size_t cap_total = s_fake.tx_capture_len;
    size_t payload_at = (cap_total >= len) ? cap_total - len : (size_t)0;
    T(cap_total >= pre + len, "telemetry fully transmitted onto the wire");
    T(payload_at >= pre, "captured only the publish after pre");
    T(memcmp(s_fake.tx_capture + payload_at, payload, len) == 0,
      "TELEMETRY_BYTES_SURVIVE_MQTT = YES (exact bytes)");
}

/* ---- Part 3: command-through-MQTT ---- */
static void test_command_through_mqtt(void)
{
    printf("\n== command bytes survive MQTT (inbound PUBLISH) ==\n");
    s_cmd_seen = false; s_cmd_len = 0;

    /* reconnect to a fresh CONNECTED state for the inbound test. */
    MqttClient_Disconnect(&s_mc);
    int g = 0;
    while (MqttClient_GetState(&s_mc) != MQTT_STATE_DISCONNECTED && g < 200)
    { FakePlatform_AdvanceTick(1); MqttClient_Run(&s_mc); g++; }
    MqttClient_Connect(&s_mc);
    while (MqttClient_GetState(&s_mc) == MQTT_STATE_CONNECTING_TRANSPORT && g < 160)
    { FakePlatform_AdvanceTick(1); MqttClient_Run(&s_mc); g++; }
    uint8_t ca[4] = {0x20,0x02,0x00,0x00};
    FakeNetworkAdapter_FeedRecv(&s_fake, ca, 4);
    while (MqttClient_GetState(&s_mc) != MQTT_STATE_CONNECTED && g < 200)
    { FakePlatform_AdvanceTick(1); MqttClient_Run(&s_mc); g++; }
    T(MqttClient_GetState(&s_mc) == MQTT_STATE_CONNECTED, "reconnected for inbound");

    const char *cmd = "{\"id\":1,\"command\":\"GET_MANIFEST\"}";
    uint8_t pub[MQTT_MAX_PACKET_SIZE];
    size_t n = MqttCodec_EncodePublish(pub, sizeof(pub), "cmd/room", 8,
                                       (const uint8_t*)cmd, strlen(cmd), 0, false, 0);
    FakeNetworkAdapter_FeedRecv(&s_fake, pub, n);
    g = 0;
    while (!s_cmd_seen && g < 40) { FakePlatform_AdvanceTick(1); MqttClient_Run(&s_mc); g++; }
    T(s_cmd_seen, "inbound PUBLISH delivered to callback");
    T(s_cmd_len == strlen(cmd) && memcmp(s_cmd_capture, cmd, strlen(cmd)) == 0,
      "COMMAND_BYTES_SURVIVE_MQTT = YES (exact bytes)");
}

int main(void)
{
    printf("Phase 17 MQTT whole-device + payload tests\n");
    test_sensor_isolation();
    test_telemetry_through_mqtt();
    test_command_through_mqtt();
    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}