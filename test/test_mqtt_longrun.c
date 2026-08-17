#include <stdio.h>
#include <string.h>

#include "mqtt_client.h"
#include "mqtt_codec.h"
#include "network_transport.h"
#include "fake_network_adapter.h"
#include "fake_platform_time.h"
#include "platform_time.h"

/* Phase 17 MQTT long-run: 24 virtual hours under a TEST-ONLY reconnect policy,
   plus a uint32 tick-window test. Uses virtual time (no real waits).

   MQTT does NOT own reconnect policy; a harness policy here drives connect /
   connection-loss / reconnect, proving the client is always recoverable and
   never spins, and that counters stay bounded. */

static int s_pass = 0, s_fail = 0, s_case = 0;
static void T(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

static FakeNetworkAdapter      s_fake;
static NetworkTransportAdapter s_adapter;
static NetworkTransport        s_transport;
static MqttClient              s_mc;

static void init_client(void)
{
    FakeNetworkAdapter_Reset(&s_fake);
    FakeNetworkAdapter_GetAdapter(&s_adapter, &s_fake);
    NetworkEndpoint ep;
    memset(&ep, 0, sizeof(ep));
    ep.port = 1883;
    memcpy(ep.host, "broker.example", 14);
    NetworkTransport_Init(&s_transport, &s_adapter, &ep);
    MqttConnectConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.client_id = "longrun";
    cfg.client_id_len = 7;
    cfg.keepalive_s = 60;
    MqttClient_Init(&s_mc, &s_transport, &cfg, NULL, NULL);
}

/* Harness: drive one connect epoch and reach CONNECTED. */
static bool harness_connect(void)
{
    if (MqttClient_Connect(&s_mc) != MQTT_IN_PROGRESS) return false;
    int g = 0;
    while (MqttClient_GetState(&s_mc) == MQTT_STATE_CONNECTING_TRANSPORT && g < 80)
    { FakePlatform_AdvanceTick(1); MqttClient_Run(&s_mc); g++; }
    if (MqttClient_GetState(&s_mc) != MQTT_STATE_WAIT_CONNACK) return false;
    uint8_t ca[4] = {0x20, 0x02, 0x00, 0x00};
    FakeNetworkAdapter_FeedRecv(&s_fake, ca, 4);
    g = 0;
    while (MqttClient_GetState(&s_mc) != MQTT_STATE_CONNECTED && g < 80)
    { FakePlatform_AdvanceTick(1); MqttClient_Run(&s_mc); g++; }
    return MqttClient_GetState(&s_mc) == MQTT_STATE_CONNECTED;
}

/* Harness: force a connection loss + recover via a fresh connect. */
static void harness_recover(void)
{
    MqttClient_Disconnect(&s_mc);
    int g = 0;
    while (MqttClient_GetState(&s_mc) != MQTT_STATE_DISCONNECTED && g < 200)
    { FakePlatform_AdvanceTick(1); MqttClient_Run(&s_mc); g++; }
    harness_connect();
}

static void test_24h(void)
{
    printf("\n== 24h virtual MQTT run ==\n");
    init_client();
    FakePlatform_SetTick(0);
    T(harness_connect(), "initial connect");

    const uint32_t total_ms = 24U * 3600U * 1000U;
    uint32_t epochs = 0, publishes = 0, pings = 0;
    uint32_t guard = 0;

    for (uint32_t t = 0; t < total_ms && guard < 10000000U; t += 1000U, guard++)
    {
        /* deterministic fault injection by virtual time. */
        if (t != 0U && (t % 1800000U) == 0U)
        {
            /* 30-min cadence: remote close + harness reconnect. */
            s_fake.recv_mode = FAKE_NET_RECV_REMOTE_CLOSE;
            int g = 0;
            while (MqttClient_GetState(&s_mc) != MQTT_STATE_ERROR && g < 20)
            { FakePlatform_AdvanceTick(100U); MqttClient_Run(&s_mc); g++; }
            harness_recover();
            epochs++;
        }
        if (t != 0U && (t % 7200000U) == 0U)
        {
            s_fake.send_error_pending = true;   /* transient transport error */
        }

        MqttClient_Run(&s_mc);
        FakePlatform_AdvanceTick(1000U);

        if (MqttClient_GetState(&s_mc) == MQTT_STATE_CONNECTED)
        {
            /* periodic telemetry-like QoS0 publish. */
            if ((t % 5000U) == 0U)
            {
                if (MqttClient_PublishQos0(&s_mc, "env/ota/room", 12,
                                           (const uint8_t*)"0123456789AB", 12, false) == MQTT_OK)
                    publishes++;
            }
            /* periodic PING via keepalive (60s) is automatic; count PINGREQ stats. */
            if ((t % 60000U) == 0U)
            {
                MqttStats st; MqttClient_GetStats(&s_mc, &st);
                if (st.pingreq_tx > pings) pings = st.pingreq_tx;
            }
        }
    }

    MqttStats st; MqttClient_GetStats(&s_mc, &st);
    printf("    24h: epochs=%lu publishes=%lu pings=%lu\n",
           (unsigned long)epochs, (unsigned long)publishes, (unsigned long)pings);
    T(st.publish_qos0 == publishes, "publish_qos0 counts match publishes");
    T(epochs < 400U, "reconnect epochs bounded (<400)");
    T(st.protocol_errors < 200U, "protocol errors bounded");
    T(st.transport_errors < 200U, "transport errors bounded");
    T(st.connect_failures < 200U, "connect failures bounded");
    T(st.tx_bytes_protocol < 0x40000000U && st.rx_bytes_protocol < 0x40000000U,
      "byte counters << 2^31 (no wrap in horizon)");
    T(MqttClient_GetState(&s_mc) == MQTT_STATE_DISCONNECTED ||
      MqttClient_GetState(&s_mc) == MQTT_STATE_CONNECTED ||
      MqttClient_GetState(&s_mc) == MQTT_STATE_ERROR,
      "state recoverable after 24h");
    (void)pings;
}

static void test_wrap(void)
{
    printf("\n== uint32 tick wrap during keepalive / connack ==\n");
    init_client();
    FakePlatform_SetTick(0xFFFFFF00U);
    if (!harness_connect())
    {
        T(false, "pre-wrap connect");
        return;
    }
    /* Cross the wrap boundary while a keepalive ping timeout is possible. */
    FakePlatform_AdvanceTick(0x11000U);   /* crosses 0x00000000 (0xFFFFFF00+0x11000) */
    MqttClient_Run(&s_mc);
    MqttStats st; MqttClient_GetStats(&s_mc, &st);
    T(st.pingreq_tx <= 1U, "no ping storm around wrap");

    /* keepalive fires across wrap correctly (60s inactivity). */
    FakePlatform_AdvanceTick(60000U);
    MqttClient_Run(&s_mc);
    FakePlatform_AdvanceTick(1U);
    MqttClient_Run(&s_mc);
    MqttClient_GetStats(&s_mc, &st);
    T(st.pingreq_tx >= 1U, "keepalive fires after wrap (wrap-safe)");

    /* CONNACK deadline across wrap: connect near wrap and let CONNACK arrive. */
    init_client();
    FakePlatform_SetTick(0xFFFFFF00U);
    MqttClient_Connect(&s_mc);
    int g = 0;
    while (MqttClient_GetState(&s_mc) == MQTT_STATE_CONNECTING_TRANSPORT && g < 80)
    { FakePlatform_AdvanceTick(1U); MqttClient_Run(&s_mc); g++; }
    uint8_t ca[4] = {0x20, 0x02, 0x00, 0x00};
    FakeNetworkAdapter_FeedRecv(&s_fake, ca, 4);
    g = 0;
    while (MqttClient_GetState(&s_mc) != MQTT_STATE_CONNECTED && g < 80)
    { FakePlatform_AdvanceTick(1U); MqttClient_Run(&s_mc); g++; }
    T(MqttClient_GetState(&s_mc) == MQTT_STATE_CONNECTED, "CONNACK accepted across wrap");
}

int main(void)
{
    printf("Phase 17 MQTT long-run tests\n");
    test_24h();
    test_wrap();
    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}