#include <stdio.h>
#include <string.h>

#include "mqtt_client.h"
#include "mqtt_codec.h"
#include "network_transport.h"
#include "fake_network_adapter.h"
#include "fake_platform_time.h"
#include "platform_time.h"

/* Phase 17 MQTT contract / negative-control tests.
     A. fragmented inbound CONNACK (byte-by-byte) still reaches CONNECTED
     B. partial TX: exact wire bytes == encoded packet (no duplicate/skip)
     C. wrong PUBACK ID must not complete the QoS1 inflight; correct one does
     D. malformed remaining-length (>4 bytes) is rejected
     E. missing PINGRESP -> ERROR (bounded)
     F. connection loss with QoS1 inflight -> reconnect does not replay
     G. packet ID wraps 65535 -> 1 and never produces 0
     H. oversized inbound packet rejected without allocation
   Deterministic; virtual time. */

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

static void setup(void)
{
    FakePlatform_SetTick(0);
    FakeNetworkAdapter_Reset(&s_fake);
    FakeNetworkAdapter_GetAdapter(&s_adapter, &s_fake);
    NetworkEndpoint ep;
    memset(&ep, 0, sizeof(ep));
    ep.port = 1883;
    memcpy(ep.host, "broker.example", 14);
    NetworkTransport_Init(&s_transport, &s_adapter, &ep);
    MqttConnectConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.client_id = "sensor";
    cfg.client_id_len = 6;
    cfg.keepalive_s = 60;
    MqttClient_Init(&s_mc, &s_transport, &cfg, NULL, NULL);
}

static bool reach_waits_connack(void)
{
    if (MqttClient_Connect(&s_mc) != MQTT_IN_PROGRESS) return false;
    int g = 0;
    while (MqttClient_GetState(&s_mc) == MQTT_STATE_CONNECTING_TRANSPORT && g < 80)
    { FakePlatform_AdvanceTick(1); MqttClient_Run(&s_mc); g++; }
    return MqttClient_GetState(&s_mc) == MQTT_STATE_WAIT_CONNACK;
}

static bool connect_ok(void)
{
    if (!reach_waits_connack()) return false;
    uint8_t ca[4] = {0x20, 0x02, 0x00, 0x00};
    FakeNetworkAdapter_FeedRecv(&s_fake, ca, 4);
    int g = 0;
    while (MqttClient_GetState(&s_mc) != MQTT_STATE_CONNECTED && g < 80)
    { FakePlatform_AdvanceTick(1); MqttClient_Run(&s_mc); g++; }
    return MqttClient_GetState(&s_mc) == MQTT_STATE_CONNECTED;
}

static void drain_until_wire(uint32_t target)
{
    int g = 0;
    while (s_fake.tx_capture_len < target && g < 2000)
    { FakePlatform_AdvanceTick(1); MqttClient_Run(&s_mc); g++; }
}

/* A. fragmented CONNACK */
static void test_fragmented_conack(void)
{
    printf("\n== A. fragmented CONNACK ==\n");
    setup();
    T(reach_waits_connack(), "WAIT_CONNACK");
    uint8_t ca[4] = {0x20, 0x02, 0x00, 0x00};
    /* feed one byte per Run. */
    for (int i = 0; i < 4 && MqttClient_GetState(&s_mc) != MQTT_STATE_CONNECTED; i++)
    {
        FakeNetworkAdapter_FeedRecv(&s_fake, ca + i, 1);
        FakePlatform_AdvanceTick(1);
        MqttClient_Run(&s_mc);
    }
    T(MqttClient_GetState(&s_mc) == MQTT_STATE_CONNECTED,
      "byte-by-byte CONNACK reaches CONNECTED (fragmentation safe)");
}

/* B. exact outgoing bytes under partial acceptance */
static void test_partial_tx_bytes(void)
{
    printf("\n== B. partial TX exact bytes ==\n");
    setup();
    connect_ok();

    /* Expected packet: QoS0 PUBLISH topic "t" payload "hello". */
    uint8_t pkt[MQTT_MAX_PACKET_SIZE];
    size_t n = MqttCodec_EncodePublish(pkt, sizeof(pkt), "t", 1,
                                       (const uint8_t*)"hello", 5, 0, false, 0);
    size_t pre = s_fake.tx_capture_len;   /* CONNECT already captured */
    s_fake.send_mode = FAKE_NET_SEND_CAPN;
    s_fake.send_cap  = 2U;       /* 2 bytes per adapter call -> heavy fragmentation */
    MqttClient_PublishQos0(&s_mc, "t", 1, (const uint8_t*)"hello", 5, false);
    drain_until_wire(pre + n);

    T(s_fake.tx_capture_len == pre + n, "wire captured exactly the publish length");
    T(memcmp(s_fake.tx_capture + pre, pkt, n) == 0,
      "wire bytes EXACTLY equal encoded packet (no duplicate / no skip)");
}

/* C. wrong/matching PUBACK id */
static void test_puback_id(void)
{
    printf("\n== C. PUBACK id matching ==\n");
    setup();
    connect_ok();
    size_t pre = s_fake.tx_capture_len;   /* CONNECT captured; QoS1 publish starts here. */
    /* QoS1 publish -> transport accepts -> inflight id = next_packet_id. */
    MqttClient_PublishQos1(&s_mc, "a/b", 3, (const uint8_t*)"x", 1);
    drain_until_wire(pre + 9U);
    /* PUBLISH QoS1 layout for topic len 3 (at offset pre): [pre+0]=0x32, [pre+1]=RL,
       [pre+2..3]=topiclen, [pre+4..6]="a/b", [pre+7..pre+8]=packet_id. */
    T(s_fake.tx_capture_len >= pre + 9U, "QoS1 PUBLISH captured on wire");
    uint16_t pid = (uint16_t)(((uint16_t)s_fake.tx_capture[pre + 7U] << 8) |
                              s_fake.tx_capture[pre + 8U]);
    T(pid != 0U, "captured packet id non-zero");

    MqttStats st;
    MqttClient_GetStats(&s_mc, &st);
    uint32_t puback0 = st.puback_rx;

    /* Wrong PUBACK id must NOT complete. */
    uint8_t wrong[4] = {0x40, 0x02, 0x00, 0xAA};
    FakeNetworkAdapter_FeedRecv(&s_fake, wrong, 4);
    FakePlatform_AdvanceTick(1);
    MqttClient_Run(&s_mc);
    MqttClient_GetStats(&s_mc, &st);
    T(st.puback_rx == puback0, "wrong PUBACK id does NOT complete inflight");
    T(s_mc.inflight_active, "inflight still active after wrong id");

    /* Correct PUBACK id completes. */
    uint8_t ok[4] = {0x40, 0x02, (uint8_t)(pid >> 8U), (uint8_t)(pid & 0xFFU)};
    FakeNetworkAdapter_FeedRecv(&s_fake, ok, 4);
    FakePlatform_AdvanceTick(1);
    MqttClient_Run(&s_mc);
    MqttClient_GetStats(&s_mc, &st);
    T(st.puback_rx == puback0 + 1U, "matching PUBACK completes inflight");
    T(!s_mc.inflight_active, "inflight cleared after matching PUBACK");
}

/* D. malformed remaining length (>4 bytes) rejected */
static void test_malformed_rl(void)
{
    printf("\n== D. malformed remaining length ==\n");
    setup();
    connect_ok();
    uint8_t bad[5] = {0x30, 0x80, 0x80, 0x80, 0x80};  /* 5 continuation bytes */
    FakeNetworkAdapter_FeedRecv(&s_fake, bad, 5);
    MqttStatus s = MQTT_OK;
    int g = 0;
    do { FakePlatform_AdvanceTick(1); s = MqttClient_Run(&s_mc); g++; }
    while (s == MQTT_OK && g < 20);
    T(MqttClient_GetState(&s_mc) == MQTT_STATE_ERROR, ">4-byte RL -> ERROR (malformed)");
}

/* E. missing PINGRESP -> ERROR bounded */
static void test_missing_pingresp(void)
{
    printf("\n== E. missing PINGRESP ==\n");
    setup();
    connect_ok();
    FakePlatform_AdvanceTick(60000U);
    MqttClient_Run(&s_mc);            /* fires PINGREQ */
    int g = 0;
    while (MqttClient_GetState(&s_mc) != MQTT_STATE_ERROR && g < 400)
    { FakePlatform_AdvanceTick(100U); MqttClient_Run(&s_mc); g++; }
    T(MqttClient_GetState(&s_mc) == MQTT_STATE_ERROR, "missing PINGRESP -> ERROR (bounded)");
}

/* F. connection loss with QoS1 inflight -> not replayed after reconnect */
static void test_inflight_not_replayed(void)
{
    printf("\n== F. QoS1 inflight not replayed ==\n");
    setup();
    connect_ok();
    MqttClient_PublishQos1(&s_mc, "a/b", 3, (const uint8_t*)"x", 1);
    int g = 0;
    while (s_fake.tx_capture_len < 10U && g < 200) { FakePlatform_AdvanceTick(1); MqttClient_Run(&s_mc); g++; }
    T(s_mc.inflight_active, "QoS1 inflight active before loss");

    /* Terminate connection (transport error), then fresh reconnect. */
    MqttClient_Disconnect(&s_mc);
    while (MqttClient_GetState(&s_mc) != MQTT_STATE_DISCONNECTED) { FakePlatform_AdvanceTick(1); MqttClient_Run(&s_mc); }
    connect_ok();
    T(!s_mc.inflight_active, "inflight cleared across reconnect (never replayed)");
    MqttStats st;
    MqttClient_GetStats(&s_mc, &st);
    T(st.publish_qos1 == 1U, "no phantom QoS1 republished on reconnect");
}

/* G. packet-id wrap 65535 -> 1, never 0 */
static void test_packet_id_wrap(void)
{
    printf("\n== G. packet id wrap ==\n");
    setup();
    connect_ok();
    s_mc.next_packet_id = 0xFFFFU;   /* white-box: struct is public */
    MqttClient_PublishQos1(&s_mc, "a/b", 3, (const uint8_t*)"x", 1);
    T(s_mc.inflight_packet_id == 0xFFFFU, "id 0xFFFF used");
    /* acked to advance */
    uint16_t pid = 0xFFFFU;
    uint8_t ok[4] = {0x40, 0x02, (uint8_t)(pid >> 8U), (uint8_t)(pid & 0xFFU)};
    FakeNetworkAdapter_FeedRecv(&s_fake, ok, 4);
    FakePlatform_AdvanceTick(1);
    MqttClient_Run(&s_mc);
    T(!s_mc.inflight_active, "acked id 0xFFFF");
    /* next publish must use id 1 (never 0). */
    MqttClient_PublishQos1(&s_mc, "a/b", 3, (const uint8_t*)"y", 1);
    T(s_mc.inflight_packet_id == 1U, "wrap 65535 -> 1, never 0");
}

/* H. oversized inbound rejected without allocation */
static void test_oversized(void)
{
    printf("\n== H. oversized inbound ==\n");
    setup();
    connect_ok();
    uint8_t ob[3] = {0x30, 0x90, 0x01};   /* RL=1440*? compute: 0x90|cont(16),0x01(1)->16+128=144 >? we need >2304.
                                             use two-byte RL: byte0=0xFF(cont,127),byte1=0x13(19) -> 127+19*128=2559>2304 */
    ob[1] = 0xFF; ob[2] = 0x13;           /* RL=2559 > 2304 */
    FakeNetworkAdapter_FeedRecv(&s_fake, ob, 3);
    MqttStatus s = MQTT_OK;
    int g = 0;
    do { FakePlatform_AdvanceTick(1); s = MqttClient_Run(&s_mc); g++; }
    while (s == MQTT_OK && g < 20);
    T(MqttClient_GetState(&s_mc) == MQTT_STATE_ERROR, "oversized RL -> ERROR (no alloc)");
    MqttStats st;
    MqttClient_GetStats(&s_mc, &st);
    T(st.oversized_packets == 1U, "oversized_packets counter incremented");
}

int main(void)
{
    printf("Phase 17 MQTT contract/negative-control tests\n");
    test_fragmented_conack();
    test_partial_tx_bytes();
    test_puback_id();
    test_malformed_rl();
    test_missing_pingresp();
    test_inflight_not_replayed();
    test_packet_id_wrap();
    test_oversized();
    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}